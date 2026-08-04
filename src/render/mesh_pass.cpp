#include "render/mesh_pass.h"

#include "assets/mesh.h"
#include "assets/shader.h"
#include "core/log.h"
#include "scene/components.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace engine::render {

    namespace {

        /// What this pass reads out of the engine content tree, by source path.
        constexpr const char* kVertexShaderSource = "mesh.vert";
        constexpr const char* kFragmentShaderSource = "mesh.frag";

        /// The push constant block, which must match mesh.vert exactly.
        struct Push {
            Mat4 model;
        };

        // 128 bytes is the smallest push constant block Vulkan guarantees, and
        // one matrix is well inside it. The view projection used to travel here
        // too, and it moved into the frame block below when the shading needed
        // the camera position and there was no room left.
        constexpr std::size_t kPushLimit = 128;
        static_assert(sizeof(Push) <= kPushLimit,
                      "The push block must fit the size every Vulkan device promises.");

        /**
         * How many lights one frame can carry.
         *
         * The block is a fixed-size array rather than a buffer that grows,
         * because the sandbox lights a scene with a handful and rule 4.6 says
         * to build the system when something needs more. A light past this
         * count is dropped with a message rather than ignored quietly.
         */
        constexpr std::uint32_t kMaxLights = 8;

        /// One light, as the shader reads it. Two vec4 and nothing else.
        struct GpuLight {
            /// xyz is the direction it points for a directional light, or where
            /// it is for a point light. w is 0 for directional and 1 for point.
            std::array<float, 4> position{};
            /// rgb is the color times the intensity. a is the range in meters,
            /// which a directional light leaves at zero.
            std::array<float, 4> color{};
        };

        /**
         * The per-frame block, which must match the Frame block in both shaders.
         *
         * It is std140, so the vec4 sits on a 16-byte boundary. A Vec3 would pad
         * to the same 16 bytes, and a written padding word is easier to read
         * than an implied one.
         */
        struct FrameUniforms {
            Mat4 view_projection{ 1.0F };
            std::array<float, 4> camera_position{};
            /// x is how many entries of `lights` are real. The rest is padding,
            /// because std140 puts the array on a 16-byte boundary anyway.
            std::array<std::uint32_t, 4> light_count{};
            std::array<GpuLight, kMaxLights> lights{};
        };

        /// Collects every light in the world into the frame block.
        /// @return True when the world held more lights than the block can carry.
        bool gather_lights(const scene::World& world, FrameUniforms& frame) {
            std::uint32_t count = 0;
            const auto add = [&frame, &count](const std::array<float, 4>& position,
                                              const Vec3& color, float intensity, float range) {
                if (count >= kMaxLights) {
                    return false;
                }
                frame.lights[count] = GpuLight{
                    .position = position,
                    .color = { color.x * intensity, color.y * intensity, color.z * intensity,
                               range },
                };
                ++count;
                return true;
            };

            bool room = true;
            const auto directional =
                world.registry()
                    .view<const scene::WorldTransform, const scene::DirectionalLight>();
            for (const auto [entity, transform, light] : directional.each()) {
                // Forward is local −Z turned into world space, per DESIGN.md
                // section 3. So a light is aimed by turning its entity.
                const Vec3 forward = glm::normalize(Vec3{ -transform.matrix[2] });
                room = add({ forward.x, forward.y, forward.z, 0.0F }, light.color,
                           light.intensity, 0.0F);
                if (!room) {
                    break;
                }
            }

            if (room) {
                const auto points =
                    world.registry().view<const scene::WorldTransform, const scene::PointLight>();
                for (const auto [entity, transform, light] : points.each()) {
                    const Vec3 at{ transform.matrix[3] };
                    room = add({ at.x, at.y, at.z, 1.0F }, light.color, light.intensity,
                               light.range);
                    if (!room) {
                        break;
                    }
                }
            }

            frame.light_count[0] = count;
            return !room;
        }

        /// Which descriptor set the frame block binds to. The material is set 1.
        constexpr std::uint32_t kFrameSet = 0;

        /**
         * Reads every cooked form of one shader source.
         *
         * A source with no variant list cooks to one form, so this is the read
         * for both cases. It goes through the manifest entry rather than
         * Content::read_bytes(source), because that call refuses a source with
         * more than one output and cannot say which form was wanted.
         */
        [[nodiscard]] bool read_stage(const assets::Content& content, const char* source,
                                      std::vector<assets::Shader>& out) {
            const assets::ManifestEntry* entry = content.find(source);
            if (entry == nullptr || entry->outputs.empty()) {
                ENGINE_LOG_ERROR("{} is not in the cooked content tree.", source);
                return false;
            }

            out.clear();
            out.reserve(entry->outputs.size());
            for (const assets::ManifestOutput& output : entry->outputs) {
                std::vector<std::byte> bytes;
                if (!content.read_bytes(output, bytes)) {
                    ENGINE_LOG_ERROR("{}: the cooked form {} would not read.", source,
                                     output.cooked);
                    return false;
                }
                assets::Shader form;
                if (!assets::read_shader(bytes, form, output.cooked)) {
                    return false;
                }
                out.push_back(std::move(form));
            }
            return true;
        }

        /// Whether two set layouts declare the same thing.
        [[nodiscard]] bool same_bindings(const std::vector<gfx::DescriptorBinding>& a,
                                         const std::vector<gfx::DescriptorBinding>& b) {
            if (a.size() != b.size()) {
                return false;
            }
            for (std::size_t at = 0; at < a.size(); ++at) {
                if (a[at].set != b[at].set || a[at].binding != b[at].binding ||
                    a[at].kind != b[at].kind || a[at].count != b[at].count ||
                    a[at].stages != b[at].stages) {
                    return false;
                }
            }
            return true;
        }

        /// The gfx name for a kind the cooked shader reports.
        [[nodiscard]] gfx::DescriptorKind to_gfx_kind(assets::DescriptorKind kind) {
            switch (kind) {
            case assets::DescriptorKind::UniformBuffer:
                return gfx::DescriptorKind::UniformBuffer;
            case assets::DescriptorKind::StorageBuffer:
                return gfx::DescriptorKind::StorageBuffer;
            case assets::DescriptorKind::CombinedImageSampler:
                break;
            }
            return gfx::DescriptorKind::CombinedImageSampler;
        }

        /// The gfx stage bits for the ones the cooked shader reports.
        [[nodiscard]] std::uint32_t to_gfx_stages(std::uint32_t stages) {
            std::uint32_t out = 0;
            if ((stages & assets::kStageBitVertex) != 0) {
                out |= gfx::kStageBitVertex;
            }
            if ((stages & assets::kStageBitFragment) != 0) {
                out |= gfx::kStageBitFragment;
            }
            if ((stages & assets::kStageBitCompute) != 0) {
                out |= gfx::kStageBitCompute;
            }
            return out;
        }

        /**
         * Joins the descriptors of the two stages into one layout.
         *
         * A binding both stages read appears once with both stage bits set.
         * Vulkan takes one set layout for the whole pipeline, so declaring it
         * twice would be two layouts for one set.
         *
         * The two stages have to agree about a slot they share. The cooker
         * cannot check that, because it reflects one module at a time and never
         * sees the pair. So this is the only place the disagreement can be
         * caught, and catching it here beats a validation error or a wrong read
         * later.
         *
         * The result comes out sorted by set and then by binding, which is what
         * GraphicsPipelineDesc::bindings asks for.
         */
        [[nodiscard]] bool merge_bindings(const assets::Shader& vertex,
                                          const assets::Shader& fragment,
                                          std::vector<gfx::DescriptorBinding>& merged) {
            bool ok = true;
            const auto add = [&merged, &ok](const assets::Shader& shader) {
                for (const assets::ShaderBinding& source : shader.bindings) {
                    const auto found = std::find_if(
                        merged.begin(), merged.end(),
                        [&source](const gfx::DescriptorBinding& entry) {
                            return entry.set == source.set && entry.binding == source.binding;
                        });
                    if (found != merged.end()) {
                        const gfx::DescriptorKind kind = to_gfx_kind(source.kind);
                        if (found->kind != kind || found->count != source.count) {
                            ENGINE_LOG_ERROR(
                                "The two stages declare set {} binding {} differently, so one "
                                "layout cannot serve both. One says {} of kind {}, the other "
                                "says {} of kind {}.",
                                source.set, source.binding, found->count,
                                static_cast<std::uint32_t>(found->kind), source.count,
                                static_cast<std::uint32_t>(kind));
                            ok = false;
                            continue;
                        }
                        found->stages |= to_gfx_stages(source.stages);
                        continue;
                    }
                    merged.push_back(gfx::DescriptorBinding{
                        .set = source.set,
                        .binding = source.binding,
                        .count = source.count,
                        .stages = to_gfx_stages(source.stages),
                        .kind = to_gfx_kind(source.kind),
                    });
                }
            };
            add(vertex);
            add(fragment);

            std::sort(merged.begin(), merged.end(),
                      [](const gfx::DescriptorBinding& a, const gfx::DescriptorBinding& b) {
                          return a.set != b.set ? a.set < b.set : a.binding < b.binding;
                      });
            return ok;
        }

    } // namespace

    std::size_t mesh_variant_index(std::uint32_t has_maps) {
        const auto has = [has_maps](MaterialMap map) {
            return (has_maps & static_cast<std::uint32_t>(map)) != 0;
        };
        std::size_t index = 0;
        if (has(MaterialMap::Normal)) {
            index |= 1U;
        }
        if (has(MaterialMap::Occlusion)) {
            index |= 2U;
        }
        return index;
    }

    std::span<const std::string_view> mesh_variant_defines(std::size_t variant) {
        using namespace std::string_view_literals;
        // The order inside each row does not matter, because pick_shader_variant
        // matches on the set and not on the sequence.
        static constexpr std::array<std::string_view, 1> kNormal{ "HAS_NORMAL_MAP"sv };
        static constexpr std::array<std::string_view, 1> kOcclusion{ "HAS_OCCLUSION_MAP"sv };
        static constexpr std::array<std::string_view, 2> kBoth{ "HAS_NORMAL_MAP"sv,
                                                                "HAS_OCCLUSION_MAP"sv };
        switch (variant) {
        case 1:
            return kNormal;
        case 2:
            return kOcclusion;
        case 3:
            return kBoth;
        default:
            break;
        }
        return {};
    }

    const assets::Shader* pick_shader_variant(std::span<const assets::Shader> forms,
                                              std::span<const std::string_view> defines) {
        for (const assets::Shader& form : forms) {
            if (form.defines.size() != defines.size()) {
                continue;
            }
            // Every define the caller asked for is present. The sizes already
            // match, so nothing extra can hide, and a module cannot list the
            // same define twice because the cooker passes the list to glslc as
            // it stands.
            const bool all =
                std::all_of(defines.begin(), defines.end(), [&form](std::string_view want) {
                    return std::find(form.defines.begin(), form.defines.end(), want) !=
                           form.defines.end();
                });
            if (all) {
                return &form;
            }
        }
        return nullptr;
    }

    MeshPass::~MeshPass() {
        destroy();
    }

    bool MeshPass::create(gfx::Device* device, const assets::Content& content) {
        if (device == nullptr) {
            ENGINE_LOG_ERROR("MeshPass::create needs a device.");
            return false;
        }
        device_ = device;

        // Before the pipeline, because every draw call binds a texture and a
        // submesh with no material has to bind this one.
        if (!textures_.create(device)) {
            return false;
        }

        if (!build_pipelines(content, pipelines_)) {
            return false;
        }
        return build_frame_sets();
    }

    /**
     * Builds the per-frame blocks and the sets that bind them.
     *
     * Separate from create() because a set is allocated against the layout of a
     * pipeline, so a rebuilt pipeline needs rebuilt sets. The buffers outlive a
     * rebuild, because nothing about them depends on the layout.
     */
    bool MeshPass::build_frame_sets() {
        const FrameUniforms empty;
        for (std::uint32_t i = 0; i < gfx::kFramesInFlight; ++i) {
            const gfx::BufferDesc desc{ .data = &empty,
                                        .size = sizeof(empty),
                                        .usage = gfx::BufferUsage::Uniform };
            if (!frame_uniforms_[i].valid() &&
                !gfx::succeeded(gfx::create_buffer(device_, desc, &frame_uniforms_[i]))) {
                ENGINE_LOG_ERROR("A frame block could not be allocated.");
                return false;
            }

            const std::array<gfx::DescriptorWrite, 1> writes{ {
                { .binding = 0,
                  .kind = gfx::DescriptorKind::UniformBuffer,
                  .texture = {},
                  .buffer = frame_uniforms_[i] },
            } };
            if (!gfx::succeeded(gfx::create_descriptor_set(device_, layout_pipeline(), kFrameSet,
                                                           writes.data(), writes.size(),
                                                           &frame_sets_[i]))) {
                ENGINE_LOG_ERROR("A frame descriptor set could not be built.");
                return false;
            }
        }
        return true;
    }

    void MeshPass::destroy_frame_sets() {
        for (gfx::DescriptorSetHandle& set : frame_sets_) {
            gfx::destroy_descriptor_set(device_, set);
            set = gfx::DescriptorSetHandle{};
        }
    }

    /**
     * Builds every pipeline from the shaders in the engine content tree.
     *
     * Separate from create() because reload_shaders() needs the same work, and
     * because it must be able to fail without touching the pipelines that are
     * already drawing.
     *
     * The shaders are read once here and not once for each pipeline. There are
     * eight pipelines over four fragment modules, and reading each module twice
     * would be four wasted reads and four wasted reflections on every reload.
     */
    bool MeshPass::build_pipelines(const assets::Content& content, PipelineSet& out) {
        std::vector<assets::Shader> vertex_forms;
        std::vector<assets::Shader> fragment_forms;
        if (!read_stage(content, kVertexShaderSource, vertex_forms) ||
            !read_stage(content, kFragmentShaderSource, fragment_forms)) {
            return false;
        }

        // The vertex stage has no variants, so it wants the form built with
        // nothing defined. Asking by defines rather than taking the first
        // output means adding a variant to it later changes nothing here.
        const assets::Shader* vertex_shader = pick_shader_variant(vertex_forms, {});
        if (vertex_shader == nullptr) {
            ENGINE_LOG_ERROR("{} has no form built with no defines, and the pass needs one.",
                             kVertexShaderSource);
            return false;
        }

        // The push block is the vertex stage's, and it must still match Push
        // above. Reflection now says how big the shader thinks it is, so the two
        // are checked against each other rather than only asserted here.
        if (vertex_shader->push_constant_size != sizeof(Push)) {
            ENGINE_LOG_ERROR("{} reads {} bytes of push constants and this pass sends {}.",
                             kVertexShaderSource, vertex_shader->push_constant_size,
                             sizeof(Push));
            return false;
        }

        out = PipelineSet{};
        std::vector<gfx::DescriptorBinding> first_bindings;
        for (std::size_t variant = 0; variant < kMeshVariantCount; ++variant) {
            const std::span<const std::string_view> defines = mesh_variant_defines(variant);
            const assets::Shader* fragment = pick_shader_variant(fragment_forms, defines);
            if (fragment == nullptr) {
                ENGINE_LOG_ERROR("{} has no form built with {}. Add it to the variant list "
                                 "in the .meta sidecar beside the shader.",
                                 kFragmentShaderSource,
                                 defines.empty() ? "no defines" : defines.front());
                destroy_pipelines(out);
                return false;
            }

            // The layout comes from the two modules rather than from this file.
            // A hand-written layout beside a shader that declares the same
            // thing used to drift from it with nothing to catch the difference.
            std::vector<gfx::DescriptorBinding> bindings;
            if (!merge_bindings(*vertex_shader, *fragment, bindings)) {
                destroy_pipelines(out);
                return false;
            }

            // Every form has to declare the same descriptors. One material
            // descriptor set is allocated once and bound against whichever form
            // a submesh needs, and Vulkan calls that undefined when the two
            // layouts differ. It shows up as a wrong texture or as nothing at
            // all, so it is checked here rather than left to the validation
            // layer.
            if (variant == 0) {
                first_bindings = bindings;
            } else if (!same_bindings(first_bindings, bindings)) {
                ENGINE_LOG_ERROR("{}: the form built with {} declares different descriptors "
                                 "than the base form. A declaration must sit outside the "
                                 "#ifdef, so that every form shares one set layout.",
                                 kFragmentShaderSource,
                                 defines.empty() ? "no defines" : defines.front());
                destroy_pipelines(out);
                return false;
            }

            // The bindings say a uniform block sits at set 1, and nothing yet
            // says what is inside it. MaterialUniforms is written by hand and
            // the GPU reads raw bytes, so a member the shader renamed or moved
            // would read the wrong field with no error anywhere.
            if (!check_material_block(*fragment, kFragmentShaderSource)) {
                ENGINE_LOG_ERROR("{} and render::MaterialUniforms do not agree, so the pass "
                                 "would upload the wrong bytes. Fix one of the two.",
                                 kFragmentShaderSource);
                destroy_pipelines(out);
                return false;
            }

            if (!build_pipeline(*vertex_shader, *fragment, bindings, false,
                                out.opaque[variant]) ||
                !build_pipeline(*vertex_shader, *fragment, bindings, true, out.blend[variant])) {
                destroy_pipelines(out);
                return false;
            }
        }
        return true;
    }

    void MeshPass::destroy_pipelines(PipelineSet& set) {
        for (std::size_t at = 0; at < kMeshVariantCount; ++at) {
            gfx::destroy_pipeline(device_, set.opaque[at]);
            gfx::destroy_pipeline(device_, set.blend[at]);
        }
        set = PipelineSet{};
    }

    /// Builds one pipeline from two modules that were already read and checked.
    bool MeshPass::build_pipeline(const assets::Shader& vertex_shader,
                                  const assets::Shader& fragment_shader,
                                  const std::vector<gfx::DescriptorBinding>& bindings, bool blend,
                                  gfx::PipelineHandle& out) {
        // The layout of assets::MeshVertex. The offsets come from the struct
        // rather than from constants, so the two cannot drift apart.
        using assets::MeshVertex;
        // All four now. The tangent went undeclared until M5.2, because an
        // attribute nothing consumes is a validation warning, and normal
        // mapping is the first thing that reads one.
        const std::array<gfx::VertexAttribute, 4> attributes{ {
            { .location = 0,
              .offset = offsetof(MeshVertex, position),
              .format = gfx::VertexFormat::Float3 },
            { .location = 1,
              .offset = offsetof(MeshVertex, normal),
              .format = gfx::VertexFormat::Float3 },
            { .location = 2,
              .offset = offsetof(MeshVertex, tangent),
              .format = gfx::VertexFormat::Float4 },
            { .location = 3,
              .offset = offsetof(MeshVertex, uv),
              .format = gfx::VertexFormat::Float2 },
        } };

        const gfx::GraphicsPipelineDesc desc{
            .vertex = { .spirv = vertex_shader.spirv.data(),
                        .word_count = vertex_shader.spirv.size() },
            .fragment = { .spirv = fragment_shader.spirv.data(),
                          .word_count = fragment_shader.spirv.size() },
            .attributes = attributes.data(),
            .attribute_count = attributes.size(),
            .vertex_stride = sizeof(MeshVertex),
            .push_constant_size = sizeof(Push),
            .bindings = bindings.data(),
            .binding_count = bindings.size(),
            .depth_test = true,
            // A blended surface tests depth and does not write it, so two of
            // them both survive what the opaque pass left behind.
            .depth_write = !blend,
            .blend = blend,
            // A glTF material can ask for both faces, and the cooked material
            // records it. Honoring it needs a second pipeline or a dynamic cull
            // state, so M5 does that with the rest of the material state.
            .cull_back = true,
        };

        const gfx::Result result = gfx::create_graphics_pipeline(device_, desc, &out);
        if (!gfx::succeeded(result)) {
            ENGINE_LOG_ERROR("The {} mesh pipeline did not build: {}", blend ? "blend" : "opaque",
                             gfx::result_name(result));
            return false;
        }
        return true;
    }

    bool MeshPass::reload_shaders(const assets::Content& content) {
        if (device_ == nullptr) {
            return false;
        }

        // Into a new set, so a shader that will not build leaves the pipelines
        // that are drawing alone. A person editing a shader gets a broken one
        // often, and losing the picture on every typo would make the loop
        // useless.
        //
        // All of them, or none. Keeping some rebuilt forms beside stale ones
        // would draw a scene half from the new shader and half from the old,
        // which is worse than keeping the old.
        PipelineSet rebuilt;
        if (!build_pipelines(content, rebuilt)) {
            ENGINE_LOG_ERROR("The shaders did not build, so the pass keeps the pipelines it "
                             "has. Fix them and save again.");
            return false;
        }

        // The old pipelines may still be bound by a frame the GPU has not
        // finished. See the note in reload() about what this wait costs and
        // what replaces it.
        gfx::device_wait_idle(device_);
        destroy_pipelines(pipelines_);
        pipelines_ = rebuilt;

        // Every descriptor set was allocated against the layout of the pipeline
        // that just went. A rebuilt shader may declare a different set, and a
        // set that no longer matches its layout is undefined to bind. So they
        // all go, and the next draw builds them again.
        destroy_frame_sets();
        materials_.destroy(device_);
        if (!build_frame_sets()) {
            ENGINE_LOG_ERROR("The frame sets did not rebuild, so nothing will draw.");
            return false;
        }

        ENGINE_LOG_INFO("The mesh shaders were built again.");
        return true;
    }

    void MeshPass::destroy() {
        if (device_ == nullptr) {
            return;
        }
        materials_.destroy(device_);
        destroy_frame_sets();
        for (gfx::BufferHandle& buffer : frame_uniforms_) {
            gfx::destroy_buffer(device_, buffer);
            buffer = gfx::BufferHandle{};
        }
        textures_.destroy(device_);
        meshes_.destroy(device_);
        destroy_pipelines(pipelines_);
        device_ = nullptr;
    }

    void MeshPass::reload(std::span<const Guid> changed) {
        if (device_ == nullptr || changed.empty()) {
            return;
        }

        // A frame that has not finished may still read the buffer or the
        // texture about to be freed. That is a use-after-free the validation
        // layer may or may not report, and it does not happen on every run.
        //
        // A reload follows a person saving a file, so a wait here costs a
        // stall nobody can see. Streaming cannot pay that. Issue #60 holds the
        // queue that frees behind the frames instead.
        gfx::device_wait_idle(device_);

        for (const Guid guid : changed) {
            // An identity is a mesh, a texture, or a material, and the caller
            // does not know which. Asking all three costs one lookup each and
            // it keeps the caller out of the question.
            meshes_.drop(device_, guid);
            textures_.drop(device_, guid);
            materials_.drop(device_, guid);
        }
    }

    void MeshPass::draw(gfx::CommandList* commands, const scene::World& world,
                        const assets::Content& content, const Mat4& view_projection,
                        const Vec3& camera_position) {
        draw_count_ = 0;
        pipeline_switches_ = 0;
        if (!layout_pipeline().valid() || !frame_sets_[frame_slot_].valid()) {
            return;
        }

        // Round the ring first, so this frame writes the slot the frame two back
        // used. That frame has finished, because the fence for it was waited on
        // before this one began. Writing the slot the last frame used would
        // change what the GPU is reading right now.
        frame_slot_ = (frame_slot_ + 1) % gfx::kFramesInFlight;

        FrameUniforms frame{
            .view_projection = view_projection,
            .camera_position = { camera_position.x, camera_position.y, camera_position.z, 1.0F },
        };
        // Report the overflow when it starts and not on every frame after it.
        // draw() runs sixty times a second, so a scene with nine lights would
        // otherwise write sixty lines a second and hide everything else.
        const bool overflowed = gather_lights(world, frame);
        if (overflowed && !lights_overflowed_) {
            ENGINE_LOG_WARN("The scene has more than {} lights, and the rest are not lit. "
                            "See kMaxLights in mesh_pass.cpp.",
                            kMaxLights);
        }
        lights_overflowed_ = overflowed;

        gfx::update_buffer(device_, frame_uniforms_[frame_slot_], &frame, sizeof(frame));

        // The frame set and the push constants are bound against this one
        // layout. Every form declares the same descriptors, so the layout is
        // compatible with all of them and both survive a pipeline change.
        // build_pipelines() refuses a set of forms where that is not true.
        gfx::cmd_bind_pipeline(commands, layout_pipeline());
        gfx::cmd_set_cull_mode(commands, true);
        gfx::cmd_bind_descriptor_set(commands, layout_pipeline(), kFrameSet,
                                     frame_sets_[frame_slot_]);
        gfx::PipelineHandle bound = layout_pipeline();

        blended_.clear();

        const auto view =
            world.registry().view<const scene::WorldTransform, const scene::MeshRenderer>();
        for (const auto [entity, transform, renderer] : view.each()) {
            if (!renderer.mesh.valid()) {
                continue;
            }
            const GpuMesh* mesh = meshes_.get(device_, content, renderer.mesh);
            if (mesh == nullptr) {
                continue;
            }

            const Push push{ .model = transform.matrix };
            gfx::cmd_push_constants(commands, layout_pipeline(), &push, sizeof(push));
            gfx::cmd_bind_vertex_buffer(commands, mesh->vertices);
            gfx::cmd_bind_index_buffer(commands, mesh->indices);

            // One draw call for each submesh. They share the two buffers, so
            // only the material and the index range change between them. The
            // material is the reason they are separate calls.
            for (const assets::MeshSubmesh& submesh : mesh->submeshes) {
                const GpuMaterial& material = materials_.get(device_, content, textures_,
                                                             layout_pipeline(), submesh.material);
                if (!material.set.valid()) {
                    continue;
                }

                // The maps the material named decide which compiled form draws
                // it. A material with no normal map runs the form that has the
                // tangent frame compiled out of it.
                const std::size_t variant = mesh_variant_index(material_maps(material.source));

                // A blended surface waits. It has to draw after every opaque
                // one, and after the blended ones behind it, so it cannot go out
                // in the order the view happens to hand it over.
                if (material.source.alpha_mode == assets::AlphaMode::Blend) {
                    // The bounds belong to the whole mesh, so every submesh of
                    // one mesh sorts together. That is right for a window and
                    // wrong for two blended parts of one model that overlap.
                    // Issue #99 holds the per-submesh bounds that would fix it.
                    const Vec3 center = Vec3{ transform.matrix *
                                              Vec4{ (mesh->min + mesh->max) * 0.5F, 1.0F } };
                    blended_.push_back(BlendedDraw{
                        .model = transform.matrix,
                        .vertices = mesh->vertices,
                        .indices = mesh->indices,
                        .set = material.set,
                        .index_count = submesh.index_count,
                        .first_index = submesh.first_index,
                        .variant = variant,
                        .double_sided = material.source.double_sided,
                        .depth = glm::distance(center, camera_position),
                    });
                    continue;
                }

                // Only when it changes. The view hands entities over in no
                // useful order, so a scene that alternates between two forms
                // rebinds on every submesh. Sorting the opaque draws by form
                // would fix that, and it belongs with the render graph in
                // M5.3. Issue #105 holds it.
                if (!(bound == pipelines_.opaque[variant])) {
                    bound = pipelines_.opaque[variant];
                    gfx::cmd_bind_pipeline(commands, bound);
                    ++pipeline_switches_;
                }

                gfx::cmd_bind_descriptor_set(commands, bound, kMaterialSet, material.set);
                gfx::cmd_set_cull_mode(commands, !material.source.double_sided);
                gfx::cmd_draw_indexed(commands, submesh.index_count, 1, submesh.first_index, 0);
                ++draw_count_;
            }
        }

        draw_blended(commands);
    }

    void MeshPass::draw_blended(gfx::CommandList* commands) {
        if (blended_.empty() || !pipelines_.blend[0].valid()) {
            return;
        }

        // Back to front. Blending reads what is already in the attachment, so
        // the far surface has to be there before the near one blends over it.
        // Nothing in the pipeline can do this, because a fragment sees only
        // itself.
        std::sort(blended_.begin(), blended_.end(),
                  [](const BlendedDraw& a, const BlendedDraw& b) { return a.depth > b.depth; });

        // The frame set stays bound. Every pipeline declares the same
        // descriptors, so their layouts are compatible and the binding survives
        // the pipeline change.
        //
        // The order is the sort's, and the form is whatever each draw needs, so
        // this can rebind on every draw. Depth order cannot be traded for fewer
        // binds here, because drawing a blended surface out of order is wrong
        // rather than slow.
        gfx::PipelineHandle bound;
        for (const BlendedDraw& draw : blended_) {
            if (!(bound == pipelines_.blend[draw.variant])) {
                bound = pipelines_.blend[draw.variant];
                gfx::cmd_bind_pipeline(commands, bound);
                ++pipeline_switches_;
            }
            gfx::cmd_push_constants(commands, bound, &draw.model, sizeof(draw.model));
            gfx::cmd_bind_vertex_buffer(commands, draw.vertices);
            gfx::cmd_bind_index_buffer(commands, draw.indices);
            gfx::cmd_bind_descriptor_set(commands, bound, kMaterialSet, draw.set);
            gfx::cmd_set_cull_mode(commands, !draw.double_sided);
            gfx::cmd_draw_indexed(commands, draw.index_count, 1, draw.first_index, 0);
            ++draw_count_;
        }
    }

} // namespace engine::render
