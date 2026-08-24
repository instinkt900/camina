#include "render/mesh_pass.h"

#include "assets/mesh.h"
#include "assets/reference.h"
#include "assets/shader.h"
#include "core/log.h"
#include "math/bounds.h"
#include "render/shader_bindings.h"
#include "scene/components.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace engine::render {

    namespace {

        /// What this pass reads out of the engine content tree, by source path.
        constexpr const char* kVertexShaderSource = "mesh.vert";
        constexpr const char* kFragmentShaderSource = "mesh.frag";
        constexpr const char* kBrdfSource = "ibl.brdf";
        constexpr const char* kClusterCullSource = "cluster_cull.comp";

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
         * How many lights the storage buffer holds when it is first allocated.
         *
         * It is a starting size and not a limit. gather_lights() reports how
         * many the world holds and the buffer grows to fit, so a scene is never
         * refused for having too many. The eight-light ceiling this replaced is
         * gone. See issue #98 and DESIGN.md section 9.
         */
        constexpr std::size_t kInitialLightCapacity = 64;

        /// How many cells one compute work group handles. Must match
        /// local_size_x in cluster_cull.comp, which decides how many threads a
        /// group runs and therefore how many groups the dispatch needs.
        constexpr std::uint32_t kClusterCullGroupSize = 64;

        /// How many bytes one cluster grid takes when a cell holds
        /// @p cell_capacity light indices. There is one for each frame in flight.
        [[nodiscard]] std::size_t cluster_grid_bytes(std::uint32_t cell_capacity) {
            return static_cast<std::size_t>(kClusterCellCount) *
                   cluster_cell_stride(cell_capacity) * sizeof(std::uint32_t);
        }

        /**
         * The cluster cull uniform block, which must match ClusterParams in
         * cluster_cull.comp. It is std140 for the mat4.
         */
        struct ClusterUniforms {
            Mat4 inv_view_projection{ 1.0F };
            /// x is the light count and y is how many indices one cell holds.
            /// The fragment shader reads the same capacity out of the frame
            /// block, because it has to index the grid the same way.
            std::array<std::uint32_t, 4> grid_info{};
            /// x is the near plane and y is how far the slices reach. The
            /// fragment shader reads the same pair out of the frame block, so
            /// this struct and FrameUniforms have to agree.
            std::array<float, 4> depth_range{};
        };

        /**
         * The per-frame block, which must match the Frame block in both shaders.
         *
         * It is std140, so the vec4 sits on a 16-byte boundary. A Vec3 would pad
         * to the same 16 bytes, and a written padding word is easier to read
         * than an implied one.
         *
         * The lights are no longer in here. A uniform block declares the
         * length of an array inside it, and that length was the ceiling on the
         * count. They moved to a storage buffer the shader indexes instead.
         */
        struct FrameUniforms {
            Mat4 view_projection{ 1.0F };
            /// One for each cascade, from `render::ShadowPass`. Identity when
            /// nothing casts.
            std::array<Mat4, kCascadeCount> light_view_projection{};
            std::array<float, 4> camera_position{};
            /// Where each cascade ends, as a distance in front of the camera.
            std::array<float, 4> cascade_splits{};
            /// The depth bias each cascade needs in its own clip space.
            std::array<float, 4> cascade_biases{};
            /// x is how many lights the storage buffer holds. y is 1 when a
            /// directional light casts a shadow. z is how many cascades are in
            /// use. w is how many light indices one cluster cell holds, which
            /// must match what ClusterUniforms carried to the cull.
            std::array<std::uint32_t, 4> light_count{};
            /// x is the near plane and y is how far the cluster slices reach.
            /// zw is the viewport in pixels. The fragment shader needs all four
            /// to work out which cell it is in, and the first two must match
            /// what ClusterUniforms carried to the cull.
            std::array<float, 4> cluster_view{};
            /// The irradiance of the environment. Each entry is one coefficient
            /// in rgb, and the fourth word is padding std140 would add anyway.
            std::array<std::array<float, 4>, assets::kIrradianceCoefficients> irradiance{};
        };

        /// Finds the cubemap the world names.
        /// @return The GUID, and a null one when no entity carries the component.
        /// @param out_extra True when more than one entity carried it.
        Guid find_environment(const scene::World& world, bool& out_extra) {
            Guid found;
            std::size_t count = 0;
            for (const auto [entity, environment] :
                 world.registry().view<const scene::Environment>().each()) {
                if (count == 0) {
                    found = environment.cubemap;
                }
                ++count;
            }
            out_extra = count > 1;
            return found;
        }

        /// Which descriptor set the frame block binds to. The material is set 1.
        constexpr std::uint32_t kFrameSet = 0;

        /**
         * The irradiance of the fallback cubemap.
         *
         * That cubemap is one constant radiance in every direction, and the
         * irradiance of a constant radiance L is pi times L in every direction.
         * So the first coefficient carries all of it and the other eight are
         * zero, which is the same thing the cooker writes for a flat sky.
         *
         * This is not an invented number. It is the right answer for the
         * environment that is actually bound, so a scene with no environment
         * lights its diffuse and its specular from one source.
         */
        assets::IrradianceSH fallback_irradiance() {
            constexpr float kPi = 3.14159265358979323846F;
            assets::IrradianceSH out;
            out.c[0].fill(kPi * kFallbackCubeRadiance);
            return out;
        }

        /**
         * Reads every cooked form of one shader source.
         *
         * A source with no variant list cooks to one form, so this is the read
         * for both cases. It goes through the manifest entry rather than
         * Content::read_bytes(source), because that call refuses a source with
         * more than one output and cannot say which form was wanted.
         */
        [[nodiscard]] bool read_stage(const assets::AssetSource& content, const char* source,
                                      std::vector<assets::Shader>& out) {
            // assets_for() says which source it could not find, so there is no
            // message here. It answers in the order the importer made the
            // forms, which is the order mesh_variant_index() numbers them in.
            std::vector<assets::AssetRecord> forms;
            if (!content.assets_for(source, forms)) {
                return false;
            }

            out.clear();
            out.reserve(forms.size());
            for (const assets::AssetRecord& record : forms) {
                std::vector<std::byte> bytes;
                if (!content.read(record.guid, bytes)) {
                    ENGINE_LOG_ERROR("{}: the form {} would not read.", source, record.name);
                    return false;
                }
                assets::Shader form;
                if (!assets::read_shader(bytes, form, record.name)) {
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

    std::uint32_t cluster_cell_capacity_for(std::size_t light_count, std::uint32_t ceiling) {
        // The budget wins over the argument. A caller that asks for more would
        // otherwise get a grid past the size kMaxLightsPerCell promises, and the
        // doubling below would run past a uint32 for a ceiling near its limit.
        const std::uint32_t limit = std::min(ceiling, kMaxLightsPerCell);
        std::uint32_t capacity = kMinLightsPerCell;
        while (capacity < light_count && capacity < limit) {
            capacity *= 2;
        }
        return std::min(capacity, limit);
    }

    std::uint32_t grow_cluster_cell_capacity(std::uint32_t current, std::size_t light_count,
                                             std::uint32_t ceiling) {
        const std::uint32_t limit = std::min(ceiling, kMaxLightsPerCell);
        const std::uint32_t wanted = cluster_cell_capacity_for(light_count, limit);
        return std::min(std::max(wanted, current), limit);
    }

    PassDesc MeshPass::declare() {
        // Static, because a PassDesc holds spans and the caller may keep it.
        // Both are writes: the color target is drawn into and the depth target
        // is both tested and updated, which is one access and not two.
        //
        // The color target is the half float scene image rather than the
        // swapchain. The tonemap pass reads it and writes the swapchain.
        static constexpr std::array<ResourceWrite, 2> kWrites{ {
            { kSceneColor, gfx::ResourceState::ColorTarget },
            { kFrameDepth, gfx::ResourceState::DepthTarget },
        } };
        // The shadow map, which the shadow pass wrote. This is the read that
        // makes the graph derive a real producer-consumer barrier rather than
        // only the two transitions a lone pass needs.
        static constexpr std::array<ResourceRead, 2> kReads{ {
            { kShadowMap, gfx::ResourceState::ShaderRead },
            // The per-cell light lists the cull pass wrote a moment ago. This
            // is what turns the dispatch and the draws into an ordered pair.
            { kClusterGrid, gfx::ResourceState::ShaderRead },
        } };
        return PassDesc{ .name = "mesh", .reads = kReads, .writes = kWrites };
    }

    PassDesc MeshPass::declare_cull() {
        static constexpr std::array<ResourceWrite, 1> kWrites{ {
            { kClusterGrid, gfx::ResourceState::ComputeWrite },
        } };
        // No reads. The light list it walks is a buffer the host wrote before
        // the submit, and the submit itself makes a host write visible.
        return PassDesc{ .name = "cluster cull", .reads = {}, .writes = kWrites };
    }

    void MeshPass::set_shadow_view(const std::array<Mat4, kCascadeCount>& light_view_projections,
                                   const std::array<float, kCascadeCount>& splits,
                                   const std::array<float, kCascadeCount>& biases, bool casts) {
        shadow_views_ = light_view_projections;
        shadow_splits_ = splits;
        shadow_biases_ = biases;
        shadow_casts_ = casts;
    }

    MeshPass::~MeshPass() {
        destroy();
    }

    bool MeshPass::create(gfx::Device* device, const assets::AssetSource& content,
                          gfx::TextureHandle shadow_map) {
        if (device == nullptr) {
            ENGINE_LOG_ERROR("MeshPass::create needs a device.");
            return false;
        }
        if (!shadow_map.valid()) {
            // The shader declares a sampler2DShadow and there is nothing else to
            // put there. Carrying on would bind a plain texture to a comparison
            // sampler, which Vulkan calls undefined rather than an error.
            ENGINE_LOG_ERROR("MeshPass::create needs the shadow map from the shadow pass.");
            return false;
        }
        device_ = device;
        shadow_map_ = shadow_map;

        // Before the pipeline, because every draw call binds a texture and a
        // submesh with no material has to bind this one.
        if (!textures_.create(device)) {
            return false;
        }

        // The frame sets below need a cubemap to write, and no world has been
        // seen yet. Starting on the fallback also means a scene that names no
        // environment matches on the first update_environment() and rebuilds
        // nothing.
        environment_ = textures_.fallback_cube();
        irradiance_ = fallback_irradiance();

        // The lookup table lives in the engine content tree beside the shaders,
        // so it is read once here and never again for a scene. A missing one is
        // fatal, because there is no sensible thing to bind in its place: a
        // white texel would say every reflection survives every surface whole.
        if (!resolve_brdf_lut(content)) {
            return false;
        }

        if (!build_pipelines(content, pipelines_)) {
            return false;
        }
        if (!build_compute_pipeline(content)) {
            return false;
        }
        if (!build_frame_sets()) {
            return false;
        }
        return build_compute_sets();
    }

    /**
     * Finds the split sum lookup table and uploads it.
     *
     * It comes through the manifest rather than from a GUID written into this
     * file. `src/render/content/ibl.brdf` carries the identity in its sidecar,
     * the same way every other asset does, so nothing here has to be kept in
     * step with a number in a file.
     */
    bool MeshPass::resolve_brdf_lut(const assets::AssetSource& content) {
        std::vector<assets::AssetRecord> table;
        if (!content.assets_for(kBrdfSource, table)) {
            ENGINE_LOG_ERROR("{} is not in the engine content, so there is no split sum "
                             "lookup and no image based lighting.",
                             kBrdfSource);
            return false;
        }

        const gfx::TextureHandle resolved = textures_.get(device_, content, table.front().guid);
        if (!resolved.valid() || resolved == textures_.fallback()) {
            ENGINE_LOG_ERROR("{} did not load, and the cache has said why. The pass cannot "
                             "shade without the table.",
                             kBrdfSource);
            return false;
        }

        brdf_guid_ = table.front().guid;
        brdf_lut_ = resolved;
        return true;
    }

    std::size_t MeshPass::gather_lights(const scene::World& world, const Frustum& frustum) {
        visible_lights_.clear();

        const auto directional =
            world.registry().view<const scene::WorldTransform, const scene::DirectionalLight>();
        for (const auto [entity, transform, light] : directional.each()) {
            // Forward is local -Z turned into world space, per DESIGN.md section
            // 3. So a light is aimed by turning its entity.
            const Vec3 forward = glm::normalize(Vec3{ -transform.matrix[2] });
            visible_lights_.push_back(GpuLight{
                .position = { forward.x, forward.y, forward.z, 0.0F },
                .color = { light.color.x * light.intensity, light.color.y * light.intensity,
                           light.color.z * light.intensity, 0.0F },
            });
        }

        std::size_t culled = 0;
        const auto points =
            world.registry().view<const scene::WorldTransform, const scene::PointLight>();
        for (const auto [entity, transform, light] : points.each()) {
            const Vec3 at{ transform.matrix[3] };
            if (!frustum_contains_sphere(frustum, at, light.range)) {
                ++culled;
                continue;
            }
            visible_lights_.push_back(GpuLight{
                .position = { at.x, at.y, at.z, 1.0F },
                .color = { light.color.x * light.intensity, light.color.y * light.intensity,
                           light.color.z * light.intensity, light.range },
            });
        }
        return culled;
    }

    /**
     * Builds the per-frame blocks and the sets that bind them.
     *
     * Separate from create() because a set is allocated against the layout of a
     * pipeline, so a rebuilt pipeline needs rebuilt sets. The buffers outlive a
     * rebuild, because nothing about them depends on the layout.
     */
    bool MeshPass::build_frame_sets() {
        if (light_capacity_ == 0) {
            light_capacity_ = kInitialLightCapacity;
        }

        // The cluster grid buffers must exist before the frame sets reference
        // them at binding 5. ensure_cluster_grid() creates them once and does
        // nothing on every later call.
        if (!ensure_cluster_grid()) {
            return false;
        }

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

            // The light list. One for each frame in flight, for the reason the
            // frame block has one: update_buffer() writes straight into memory
            // the GPU may still be reading.
            //
            // No data, so it allocates without uploading. A frame writes it
            // before it draws, and a frame with no lights leaves it untouched
            // because light_count is zero and the shader reads nothing.
            if (!light_buffers_[i].valid()) {
                const gfx::BufferDesc lights{ .data = nullptr,
                                              .size = light_capacity_ * sizeof(GpuLight),
                                              .usage = gfx::BufferUsage::Storage };
                if (!gfx::succeeded(gfx::create_buffer(device_, lights, &light_buffers_[i]))) {
                    ENGINE_LOG_ERROR("A light buffer could not be allocated for {} lights.",
                                     light_capacity_);
                    return false;
                }
            }

            // The environment is written into the set rather than bound beside
            // it, because a descriptor set is filled once and a cubemap changes
            // only when the scene names another one. update_environment() calls
            // this again when that happens.
            const std::array<gfx::DescriptorWrite, 6> writes{ {
                { .binding = 0,
                  .kind = gfx::DescriptorKind::UniformBuffer,
                  .texture = {},
                  .buffer = frame_uniforms_[i] },
                { .binding = 1,
                  .kind = gfx::DescriptorKind::CombinedImageSampler,
                  .texture = environment_.valid() ? environment_ : textures_.fallback_cube(),
                  .buffer = {} },
                { .binding = 2,
                  .kind = gfx::DescriptorKind::CombinedImageSampler,
                  .texture = brdf_lut_.valid() ? brdf_lut_ : textures_.fallback(),
                  .buffer = {} },
                // The shadow map. There is no fallback for this one, because the
                // shader reads it as a sampler2DShadow and the flat white texel
                // carries no comparison sampler. Binding one where the other is
                // declared is undefined rather than an error, so create()
                // refuses to run without a real map instead.
                { .binding = 3,
                  .kind = gfx::DescriptorKind::CombinedImageSampler,
                  .texture = shadow_map_,
                  .buffer = {} },
                // The lights. A storage buffer rather than part of the block
                // above. A uniform block declares how long an array inside it
                // is, and that length capped the count at eight.
                { .binding = 4,
                  .kind = gfx::DescriptorKind::StorageBuffer,
                  .texture = {},
                  .buffer = light_buffers_[i] },
                // The cluster grid. The compute cull writes it and the fragment
                // shader reads it, so each cell loops over far fewer lights.
                { .binding = 5,
                  .kind = gfx::DescriptorKind::StorageBuffer,
                  .texture = {},
                  .buffer = cluster_grids_[i] },
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

    bool MeshPass::ensure_capacity(std::size_t needed) {
        const std::uint32_t cells =
            grow_cluster_cell_capacity(cluster_capacity_, needed, cluster_ceiling_);
        if (needed <= light_capacity_ && cells == cluster_capacity_) {
            return true;
        }

        // Double until it fits, so a scene that grows one light at a time does
        // not reallocate once for each. Starting from the count rather than the
        // old capacity covers the jump from an empty scene to a large one.
        std::size_t capacity = std::max(light_capacity_, kInitialLightCapacity);
        while (capacity < needed) {
            capacity *= 2;
        }

        // Every frame in flight may be reading its own buffer, and the sets name
        // the buffers, so both go together and both wait. This is why the
        // capacity doubles rather than growing to fit exactly.
        gfx::device_wait_idle(device_);
        destroy_frame_sets();
        for (gfx::BufferHandle& buffer : light_buffers_) {
            gfx::destroy_buffer(device_, buffer);
            buffer = gfx::BufferHandle{};
        }

        // The grid only goes when its shape changes, because it is the larger of
        // the two by far and a light buffer that doubles often leaves the
        // per-cell capacity where it was.
        const std::uint32_t previous_cells = cluster_capacity_;
        if (cells != cluster_capacity_) {
            for (gfx::BufferHandle& buffer : cluster_grids_) {
                gfx::destroy_buffer(device_, buffer);
                buffer = gfx::BufferHandle{};
            }
            cluster_capacity_ = cells;
        }

        const std::size_t previous = light_capacity_;
        light_capacity_ = capacity;
        if (!build_frame_sets()) {
            ENGINE_LOG_ERROR("The light buffer could not grow to {}, so nothing can draw.",
                             capacity);
            return false;
        }
        if (!build_compute_sets()) {
            ENGINE_LOG_ERROR("The compute sets could not be rebuilt for the new light buffers.");
            return false;
        }
        if (capacity != previous) {
            ENGINE_LOG_INFO("The light buffer grew from {} to {} to fit {} lights.", previous,
                            capacity, needed);
        }
        if (cells != previous_cells) {
            ENGINE_LOG_INFO("A cluster cell holds {} lights now, up from {}, which is {} MiB "
                            "for each frame in flight.",
                            cells, previous_cells, cluster_grid_bytes(cells) / (1024 * 1024));
        }
        if (needed > cells) {
            ENGINE_LOG_WARN("A cluster cell holds {} lights and this frame has {} visible, so a "
                            "crowded cell drops the rest. See issue #175.",
                            cells, needed);
        }
        return true;
    }

    void MeshPass::order_lights_for_overflow() {
        if (!cluster_may_drop()) {
            return;
        }

        // Rec. 709 luminance, over a color that already carries the intensity.
        // Times the range, because a light that reaches further covers more of
        // the cells that are crowded. A directional light sorts above every
        // point light, which costs nothing: the cull writes them first anyway.
        const auto rank = [](const GpuLight& light) {
            const float luminance = (0.2126F * light.color[0]) + (0.7152F * light.color[1]) +
                                    (0.0722F * light.color[2]);
            const bool directional = light.position[3] < 0.5F;
            return directional ? std::numeric_limits<float>::max() : luminance * light.color[3];
        };
        // Stable, so two lights of equal rank keep the order the scene gave
        // them and the frame stays the same from one run to the next.
        std::stable_sort(visible_lights_.begin(), visible_lights_.end(),
                         [&rank](const GpuLight& a, const GpuLight& b) {
                             return rank(a) > rank(b);
                         });
    }

    void MeshPass::destroy_frame_sets() {
        for (gfx::DescriptorSetHandle& set : frame_sets_) {
            gfx::destroy_descriptor_set(device_, set);
            set = gfx::DescriptorSetHandle{};
        }
    }

    bool MeshPass::build_compute_pipeline(const assets::AssetSource& content) {
        std::vector<assets::Shader> forms;
        if (!read_stage(content, kClusterCullSource, forms) || forms.empty()) {
            ENGINE_LOG_ERROR("{} would not read from the content tree.", kClusterCullSource);
            return false;
        }

        // By what the form declares rather than by where it sits. The base form
        // is part 0 today, and a variant added to the sidecar later must not be
        // able to move it. build_pipelines() picks the same way.
        const assets::Shader* picked = pick_shader_variant(forms, {});
        if (picked == nullptr) {
            ENGINE_LOG_ERROR("{} has no form that was built with no defines.",
                             kClusterCullSource);
            return false;
        }
        const assets::Shader& shader = *picked;
        std::vector<gfx::DescriptorBinding> bindings;
        bindings.reserve(shader.bindings.size());
        for (const assets::ShaderBinding& source : shader.bindings) {
            gfx::DescriptorKind kind = gfx::DescriptorKind::CombinedImageSampler;
            switch (source.kind) {
            case assets::DescriptorKind::UniformBuffer:
                kind = gfx::DescriptorKind::UniformBuffer;
                break;
            case assets::DescriptorKind::StorageBuffer:
                kind = gfx::DescriptorKind::StorageBuffer;
                break;
            case assets::DescriptorKind::CombinedImageSampler:
                break;
            }

            std::uint32_t stages = 0;
            if ((source.stages & assets::kStageBitCompute) != 0) {
                stages |= gfx::kStageBitCompute;
            }

            bindings.push_back(gfx::DescriptorBinding{
                .set = source.set,
                .binding = source.binding,
                .count = source.count,
                .stages = stages,
                .kind = kind,
            });
        }

        const gfx::ComputePipelineDesc desc{
            .compute = { shader.spirv.data(), shader.spirv.size() },
            .bindings = bindings.data(),
            .binding_count = bindings.size(),
        };

        gfx::PipelineHandle pipeline;
        if (!gfx::succeeded(gfx::create_compute_pipeline(device_, desc, &pipeline))) {
            ENGINE_LOG_ERROR("The cluster cull pipeline did not build.");
            return false;
        }

        if (compute_pipeline_.valid()) {
            gfx::destroy_pipeline(device_, compute_pipeline_);
        }
        compute_pipeline_ = pipeline;

        return true;
    }

    bool MeshPass::build_compute_sets() {
        if (!compute_pipeline_.valid()) {
            return true;
        }
        if (!ensure_cluster_grid()) {
            return false;
        }

        // Into a spare array first. A set that is half rebuilt is worse than
        // the old one, because a failure part way through would leave some
        // slots pointing at a layout that no longer exists. The same argument
        // build_pipelines() makes about a half-reloaded shader.
        std::array<gfx::DescriptorSetHandle, gfx::kFramesInFlight> rebuilt;
        for (std::uint32_t i = 0; i < gfx::kFramesInFlight; ++i) {
            const std::array<gfx::DescriptorWrite, 3> compute_writes{ {
                { .binding = 0,
                  .kind = gfx::DescriptorKind::UniformBuffer,
                  .texture = {},
                  .buffer = cluster_uniforms_[i] },
                { .binding = 1,
                  .kind = gfx::DescriptorKind::StorageBuffer,
                  .texture = {},
                  .buffer = light_buffers_[i] },
                { .binding = 2,
                  .kind = gfx::DescriptorKind::StorageBuffer,
                  .texture = {},
                  .buffer = cluster_grids_[i] },
            } };
            if (!gfx::succeeded(gfx::create_descriptor_set(device_, compute_pipeline_, 0,
                                                           compute_writes.data(),
                                                           compute_writes.size(), &rebuilt[i]))) {
                ENGINE_LOG_ERROR("The compute descriptor set could not be built for frame {}.", i);
                for (gfx::DescriptorSetHandle& set : rebuilt) {
                    gfx::destroy_descriptor_set(device_, set);
                }
                return false;
            }
        }

        for (std::uint32_t i = 0; i < gfx::kFramesInFlight; ++i) {
            gfx::destroy_descriptor_set(device_, compute_sets_[i]);
            compute_sets_[i] = rebuilt[i];
        }
        return true;
    }

    bool MeshPass::ensure_cluster_grid() {
        const std::size_t size = cluster_grid_bytes(cluster_capacity_);
        for (std::uint32_t i = 0; i < gfx::kFramesInFlight; ++i) {
            if (!cluster_grids_[i].valid()) {
                // Device-local. The compute shader is the only writer and the
                // fragment shader is the only reader, so the host has no reason
                // to reach it. Left host-visible it would be system memory
                // across PCIe on a discrete GPU, written whole every frame and
                // then read for every pixel.
                const gfx::BufferDesc desc{ .data = nullptr,
                                            .size = size,
                                            .usage = gfx::BufferUsage::Storage,
                                            .memory = gfx::BufferMemory::DeviceLocal };
                if (!gfx::succeeded(gfx::create_buffer(device_, desc, &cluster_grids_[i]))) {
                    ENGINE_LOG_ERROR("A cluster grid buffer could not be allocated.");
                    return false;
                }
            }

            if (!cluster_uniforms_[i].valid()) {
                const ClusterUniforms empty;
                const gfx::BufferDesc uniform_desc{ .data = &empty,
                                                    .size = sizeof(empty),
                                                    .usage = gfx::BufferUsage::Uniform };
                if (!gfx::succeeded(
                        gfx::create_buffer(device_, uniform_desc, &cluster_uniforms_[i]))) {
                    ENGINE_LOG_ERROR("A cluster uniform buffer could not be allocated.");
                    return false;
                }
            }
        }
        return true;
    }

    /**
     * Resolves the cubemap the world names and rebuilds the frame sets for it.
     *
     * A descriptor set is written when it is built, so a new cubemap needs new
     * sets. That is why this compares against what the sets already bind and
     * does nothing on almost every frame. It runs when a scene loads, when a
     * person names another environment, and when the cooked file changes.
     *
     * The wait is the same one reload() takes, and for the same reason: a frame
     * the GPU has not finished may still read a set about to be freed. It costs
     * a stall nobody can see, because it follows a person saving a file or a
     * scene appearing.
     */
    void MeshPass::update_environment(const scene::World& world,
                                      const assets::AssetSource& content) {
        bool extra = false;
        const Guid wanted = find_environment(world, extra);
        if (extra && !environments_overflowed_) {
            ENGINE_LOG_WARN("The scene holds more than one Environment component, and the "
                            "first one wins. One frame binds one cubemap.");
        }
        environments_overflowed_ = extra;

        if (wanted == environment_guid_ && environment_.valid()) {
            return;
        }

        // A GUID that will not load gives the grey fallback, which is a valid
        // cubemap. So this always ends with something to bind, and the cache
        // has already said by name what went wrong.
        const gfx::TextureHandle resolved = textures_.get_cube(device_, content, wanted);

        // The diffuse half, read here rather than in draw() for the same reason
        // the cubemap is: it changes when the scene names another environment
        // and on no other frame.
        update_irradiance(content, wanted, resolved == textures_.fallback_cube());

        // Recorded before the early exit below, so a GUID that failed is asked
        // for once rather than on every frame.
        environment_guid_ = wanted;
        if (resolved == environment_) {
            return;
        }
        environment_ = resolved;

        gfx::device_wait_idle(device_);
        destroy_frame_sets();
        if (!build_frame_sets()) {
            ENGINE_LOG_ERROR("The frame sets did not rebuild for the environment, so nothing "
                             "will draw.");
        }
    }

    /**
     * Reads the irradiance that belongs to the cubemap now bound.
     *
     * The cooker writes it as a sub-asset of the panorama, under a GUID derived
     * from the source. So a scene names one identity and this works out the
     * other, the same way a prefab names one mesh of a glTF file.
     *
     * @param environment The identity the scene named, which may be null.
     * @param fallback True when the grey cubemap is what got bound, either
     * because the scene named none or because the one it named would not load.
     */
    void MeshPass::update_irradiance(const assets::AssetSource& content, Guid environment,
                                     bool fallback) {
        if (fallback) {
            irradiance_ = fallback_irradiance();
            return;
        }

        const Guid derived = Guid::derive(environment, assets::kIrradiancePartKind, 0);
        std::vector<std::byte> bytes;
        assets::IrradianceSH read;
        if (content.read(derived, bytes) &&
            assets::read_irradiance(bytes, read, "the environment irradiance")) {
            irradiance_ = read;
            return;
        }

        // Zero, and not the fallback constant. The cubemap that got bound is a
        // real environment, so a grey diffuse beside it would light the scene
        // from two different places and hide that anything is wrong.
        ENGINE_LOG_WARN("The environment has no irradiance beside it, so nothing lights the "
                        "diffuse of this scene. Cook the panorama again.");
        irradiance_ = assets::IrradianceSH{};
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
    bool MeshPass::build_pipelines(const assets::AssetSource& content, PipelineSet& out) {
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
            .depth_only = false,
            // Half float, because the scene renders into the intermediate the
            // tonemap pass reads. Writing an 8-bit sRGB image here would clip
            // every value above 1 before anything could map it down.
            .color_format = gfx::ColorTargetFormat::RGBA16F,
        };

        const gfx::Result result = gfx::create_graphics_pipeline(device_, desc, &out);
        if (!gfx::succeeded(result)) {
            ENGINE_LOG_ERROR("The {} mesh pipeline did not build: {}", blend ? "blend" : "opaque",
                             gfx::result_name(result));
            return false;
        }
        return true;
    }

    bool MeshPass::reload_shaders(const assets::AssetSource& content) {
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
        if (!build_compute_pipeline(content)) {
            ENGINE_LOG_ERROR("The compute pipeline did not rebuild, so the cluster cull "
                             "will not run.");
            return false;
        }
        if (!build_compute_sets()) {
            ENGINE_LOG_ERROR("The compute sets did not rebuild, so the cluster cull "
                             "will not run.");
            return false;
        }

        ENGINE_LOG_INFO("The mesh shaders were built again.");
        return true;
    }

    bool MeshPass::reload_brdf_lut(const assets::AssetSource& content) {
        if (device_ == nullptr) {
            return false;
        }

        // Save the old table before trying the new one. A resolve that fails
        // must leave the active table in place, and the old texture must not
        // be freed while a submitted frame may still be reading it.
        const Guid old_guid = brdf_guid_;
        const gfx::TextureHandle old_lut = brdf_lut_;

        if (!resolve_brdf_lut(content)) {
            // Restore what the frame sets already bind.
            brdf_guid_ = old_guid;
            brdf_lut_ = old_lut;
            ENGINE_LOG_WARN("The split sum table would not reload, so the pass keeps the one "
                            "it has. Fix {} and save again.",
                            kBrdfSource);
            return false;
        }

        // The new table is ready. Now it is safe to wait for the GPU and
        // retire the old one. The frame sets bake the BRDF handle at creation
        // time, so they go with it.
        gfx::device_wait_idle(device_);
        textures_.drop(device_, old_guid);
        destroy_frame_sets();
        if (!build_frame_sets()) {
            ENGINE_LOG_ERROR("The frame sets did not rebuild for the table, so nothing "
                             "will draw.");
            return false;
        }

        ENGINE_LOG_INFO("The split sum table was read again.");
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
        for (gfx::BufferHandle& buffer : light_buffers_) {
            gfx::destroy_buffer(device_, buffer);
            buffer = gfx::BufferHandle{};
        }
        gfx::destroy_pipeline(device_, compute_pipeline_);
        compute_pipeline_ = gfx::PipelineHandle{};
        for (gfx::DescriptorSetHandle& set : compute_sets_) {
            gfx::destroy_descriptor_set(device_, set);
            set = gfx::DescriptorSetHandle{};
        }
        for (gfx::BufferHandle& buffer : cluster_grids_) {
            gfx::destroy_buffer(device_, buffer);
            buffer = gfx::BufferHandle{};
        }
        for (gfx::BufferHandle& buffer : cluster_uniforms_) {
            gfx::destroy_buffer(device_, buffer);
            buffer = gfx::BufferHandle{};
        }
        // Back to zero, not left at whatever the last scene needed. A create()
        // after this builds the buffers again, and build_frame_sets() only
        // chooses a starting capacity when this is zero. Leaving it set would
        // size the new buffers from the old scene and skip the message that says
        // what happened.
        light_capacity_ = 0;
        visible_lights_.clear();
        culled_lights_ = 0;
        textures_.destroy(device_);
        // The fallback cubemap and the lookup table just went with the cache, so
        // the handles beside them are stale. A second destroy() must not hand
        // one of them to the device.
        environment_ = gfx::TextureHandle{};
        environment_guid_ = Guid{};
        brdf_lut_ = gfx::TextureHandle{};
        brdf_guid_ = Guid{};
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

            // The frame sets still write the cubemap that just went. Forgetting
            // the handle makes the next draw resolve it again and rebuild them,
            // which is what a person saving an environment expects to see.
            //
            // The irradiance is matched as well, though today it can never
            // arrive alone: it is a sub-asset of the same source, and
            // assets::Content hashes a whole manifest entry rather than each
            // output, so
            // the two identities always change together. That is a fact about
            // another file. Matching both keeps this one right on its own.
            if (guid == environment_guid_ ||
                (environment_guid_.valid() &&
                 guid == Guid::derive(environment_guid_, assets::kIrradiancePartKind, 0))) {
                environment_ = gfx::TextureHandle{};
            }
        }
    }

    void MeshPass::cull(gfx::CommandList* commands, const scene::World& world,
                        const assets::AssetSource& content, const Mat4& view_projection,
                        const Vec3& camera_position, const ClusterView& view) {
        if (!layout_pipeline().valid()) {
            return;
        }

        // Before anything is recorded, because this may rebuild the frame sets
        // and it waits for the device to go idle when it does.
        update_environment(world, content);

        // Round the ring first, so this frame writes the slot the frame two back
        // used. That frame has finished, because the fence for it was waited on
        // before this one began.
        frame_slot_ = (frame_slot_ + 1) % gfx::kFramesInFlight;

        FrameUniforms frame{
            .view_projection = view_projection,
            .light_view_projection = shadow_views_,
            .camera_position = { camera_position.x, camera_position.y, camera_position.z, 1.0F },
            .cascade_splits = { shadow_splits_[0], shadow_splits_[1], shadow_splits_[2],
                                shadow_splits_[3] },
            .cascade_biases = { shadow_biases_[0], shadow_biases_[1], shadow_biases_[2],
                                shadow_biases_[3] },
            .cluster_view = { view.z_near, kClusterFar, view.viewport_width,
                              view.viewport_height },
        };

        for (std::size_t i = 0; i < assets::kIrradianceCoefficients; ++i) {
            frame.irradiance[i] = { irradiance_.c[i][0], irradiance_.c[i][1],
                                    irradiance_.c[i][2], 0.0F };
        }

        // Extracted once here. draw() culls meshes against the same planes, so
        // the two cannot disagree about which camera the frame belongs to.
        frustum_ = frustum_from_view_projection(view_projection);

        const std::size_t culled = gather_lights(world, frustum_);
        culled_lights_ = culled;

        // Before the block is written, because this decides how a cell is
        // indexed and both shaders read the number out of a uniform.
        if (!ensure_capacity(visible_lights_.size())) {
            return;
        }
        order_lights_for_overflow();

        frame.light_count[0] = static_cast<std::uint32_t>(visible_lights_.size());
        frame.light_count[1] = shadow_casts_ ? 1U : 0U;
        frame.light_count[2] = static_cast<std::uint32_t>(kCascadeCount);
        frame.light_count[3] = cluster_capacity_;

        gfx::update_buffer(device_, frame_uniforms_[frame_slot_], &frame, sizeof(frame));
        if (!visible_lights_.empty()) {
            gfx::update_buffer(device_, light_buffers_[frame_slot_], visible_lights_.data(),
                               visible_lights_.size() * sizeof(GpuLight));
        }

        if (!compute_pipeline_.valid() || !compute_sets_[frame_slot_].valid()) {
            return;
        }

        ClusterUniforms cull_uniforms;
        cull_uniforms.inv_view_projection = glm::inverse(view_projection);
        cull_uniforms.grid_info[0] = static_cast<std::uint32_t>(visible_lights_.size());
        cull_uniforms.grid_info[1] = cluster_capacity_;
        cull_uniforms.depth_range = { view.z_near, kClusterFar, 0.0F, 0.0F };
        gfx::update_buffer(device_, cluster_uniforms_[frame_slot_], &cull_uniforms,
                           sizeof(cull_uniforms));

        gfx::cmd_bind_compute_pipeline(commands, compute_pipeline_);
        gfx::cmd_bind_compute_descriptor_set(commands, compute_pipeline_, 0,
                                             compute_sets_[frame_slot_]);

        // Every frame, including one with no lights at all. The shader writes a
        // count for every cell whatever the light count is, so a zero count is
        // something it writes rather than something the host has to clear
        // first. Skipping the dispatch is what would leave the frame before
        // this one in the buffer.
        const std::uint32_t group_count =
            (kClusterCellCount + kClusterCullGroupSize - 1) / kClusterCullGroupSize;
        gfx::cmd_dispatch(commands, group_count, 1, 1);

        // No barrier here. The cull is a pass in the frame graph now, and the
        // barrier between this write and the fragment reads falls out of
        // derive_barriers() the way every other one does. See declare_cull().
        culled_this_frame_ = true;
    }

    void MeshPass::draw(gfx::CommandList* commands, const scene::World& world,
                        const assets::AssetSource& content, const Vec3& camera_position) {
        draw_count_ = 0;
        pipeline_switches_ = 0;
        culled_meshes_ = 0;
        if (!frame_sets_[frame_slot_].valid()) {
            return;
        }

        // cull() owns the frame slot and the frame block, so drawing without it
        // would bind the camera of the frame before this one. That shows up as
        // a picture one frame stale, which is easy to miss and hard to place.
        if (!culled_this_frame_) {
            if (!warned_missing_cull_) {
                ENGINE_LOG_WARN("MeshPass::draw ran without MeshPass::cull for this frame, so "
                                "nothing drew. Call cull() first, outside a rendering scope.");
                warned_missing_cull_ = true;
            }
            return;
        }
        culled_this_frame_ = false;

        // The frame set and the push constants are bound against this one
        // layout. Every form declares the same descriptors, so the layout is
        // compatible with all of them and both survive a pipeline change.
        // build_pipelines() refuses a set of forms where that is not true.
        gfx::cmd_bind_descriptor_set(commands, layout_pipeline(), kFrameSet,
                                     frame_sets_[frame_slot_]);

        gather_draws(world, content, camera_position);

        // Sort opaque draws by variant so that the pipeline is rebound only
        // when the form changes, and not on every submesh that alternates
        // between two materials.
        //
        // Depth order does not matter here: the depth test decides what wins.
        // The blended draws are a different case, and draw_blended() keeps
        // the back-to-front sort.
        std::sort(opaque_.begin(), opaque_.end(),
                  [](const OpaqueDraw& a, const OpaqueDraw& b) { return a.variant < b.variant; });

        gfx::PipelineHandle bound;
        for (const OpaqueDraw& draw : opaque_) {
            if (!(bound == pipelines_.opaque[draw.variant])) {
                bound = pipelines_.opaque[draw.variant];
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

        draw_blended(commands);
    }

    void MeshPass::gather_draws(const scene::World& world, const assets::AssetSource& content,
                                const Vec3& camera_position) {
        blended_.clear();
        opaque_.clear();

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

            // Off screen entities issue nothing. The test is per entity and not
            // per submesh, because the bounds of the whole mesh are what decide
            // whether any of it can be seen. A mesh that is in view draws every
            // part of it, and a tighter per-submesh test is issue #177.
            //
            // This runs before the opaque and blended split, so both kinds get
            // it. The sphere is conservative, so a mesh it keeps may still be
            // wholly outside. That costs a draw call and never a hole.
            // The box rather than the sphere, for the reason ShadowPass::draw
            // gives. A light keeps its sphere test above, because a point light
            // range really is a sphere and there is nothing tighter to use.
            const Obb bounds = world_box_from_bounds(transform.matrix, mesh->min, mesh->max);
            if (!frustum_contains_box(frustum_, bounds.center, bounds.axis_x, bounds.axis_y,
                                      bounds.axis_z)) {
                ++culled_meshes_;
                continue;
            }

            for (std::size_t s = 0; s < mesh->submeshes.size(); ++s) {
                const assets::MeshSubmesh& submesh = mesh->submeshes[s];
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
                    // Per-submesh bounds sort each blended part by its own
                    // depth, not the depth of the whole mesh. See issue #99.
                    const bool has_sub_bounds =
                        s < mesh->submesh_min.size() && s < mesh->submesh_max.size();
                    const Vec3 center = Vec3{
                        transform.matrix *
                        Vec4{ (has_sub_bounds
                                   ? (mesh->submesh_min[s] + mesh->submesh_max[s]) * 0.5F
                                   : (mesh->min + mesh->max) * 0.5F),
                              1.0F }
                    };
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

                opaque_.push_back(OpaqueDraw{
                    .model = transform.matrix,
                    .vertices = mesh->vertices,
                    .indices = mesh->indices,
                    .set = material.set,
                    .index_count = submesh.index_count,
                    .first_index = submesh.first_index,
                    .variant = variant,
                    .double_sided = material.source.double_sided,
                });
            }
        }
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
