#include "render/shadow_pass.h"

#include "assets/mesh.h"
#include "assets/shader.h"
#include "core/log.h"
#include "scene/components.h"

#include <array>
#include <limits>
#include <vector>

namespace engine::render {

    namespace {

        /**
         * The map is square and fixed.
         *
         * One map over the whole scene, so the texel density follows the size of
         * the scene rather than the camera. 2048 covers the sandbox room at
         * about a centimeter for each texel. Issue #135 replaces this with a
         * cascade set, which is what makes the density follow the view.
         */
        constexpr std::uint32_t kShadowMapSize = 2048;

        /// Keeps the near plane off the geometry, so a caster just outside the
        /// fitted volume still writes depth rather than being clipped away.
        constexpr float kLightMargin = 1.0F;

        /// A scene with nothing in it has no volume to fit, and a zero-sized
        /// projection divides by zero. This is the smallest half extent used.
        constexpr float kSmallestExtent = 0.01F;

        /// The eight corners of an axis aligned box, as (x, y, z) selectors.
        constexpr std::array<std::array<int, 3>, 8> kBoxCorners{ {
            { 0, 0, 0 }, { 1, 0, 0 }, { 0, 1, 0 }, { 1, 1, 0 },
            { 0, 0, 1 }, { 1, 0, 1 }, { 0, 1, 1 }, { 1, 1, 1 },
        } };

        /// Reads the cooked shadow shader, which has one form and no variants.
        [[nodiscard]] bool read_shadow_shader(const assets::Content& content,
                                              assets::Shader& out) {
            const assets::ManifestEntry* entry = content.find("shadow.vert");
            if (entry == nullptr || entry->outputs.empty()) {
                ENGINE_LOG_ERROR("shadow.vert is not in the cooked content tree.");
                return false;
            }
            std::vector<std::byte> bytes;
            if (!content.read_bytes(entry->outputs.front(), bytes)) {
                ENGINE_LOG_ERROR("shadow.vert would not read.");
                return false;
            }
            return assets::read_shader(bytes, out, entry->outputs.front().cooked);
        }

    } // namespace

    ShadowPass::~ShadowPass() = default;

    PassDesc ShadowPass::declare() {
        static constexpr std::array<ResourceWrite, 1> kWrites{ {
            { kShadowMap, gfx::ResourceState::DepthTarget },
        } };
        return PassDesc{ .name = "shadow", .reads = {}, .writes = kWrites };
    }

    bool ShadowPass::build_pipeline(const assets::Content& content, gfx::PipelineHandle& out) {
        assets::Shader vertex;
        if (!read_shadow_shader(content, vertex)) {
            return false;
        }

        // Position alone, at location 0. The stride is still the whole cooked
        // vertex, because the stream is interleaved. See issue #87.
        const std::array<gfx::VertexAttribute, 1> attributes{ {
            { .location = 0, .offset = 0, .format = gfx::VertexFormat::Float3 },
        } };

        const gfx::GraphicsPipelineDesc desc{
            .vertex = { .spirv = vertex.spirv.data(),
                        .word_count = static_cast<std::uint32_t>(vertex.spirv.size()) },
            .fragment = {},
            .attributes = attributes.data(),
            .attribute_count = attributes.size(),
            .vertex_stride = sizeof(assets::MeshVertex),
            .push_constant_size = sizeof(Push),
            .bindings = nullptr,
            .binding_count = 0,
            .depth_test = true,
            .depth_write = true,
            .blend = false,
            // Front faces are culled rather than back ones. Rendering the far
            // side of a caster puts the recorded depth behind the surface that
            // is lit, which moves the acne out of view without a bias large
            // enough to detach a contact shadow. See the note in mesh.frag.
            .cull_back = false,
            .depth_only = true,
        };

        return gfx::succeeded(gfx::create_graphics_pipeline(device_, desc, &out));
    }

    bool ShadowPass::create(gfx::Device* device, const assets::Content& content) {
        ENGINE_ASSERT(device != nullptr, "ShadowPass::create needs a device.");
        device_ = device;

        const gfx::DepthTargetDesc target{
            .width = kShadowMapSize,
            .height = kShadowMapSize,
            .sampler = { .filter = gfx::Filter::Linear,
                         .address = gfx::AddressMode::ClampToZeroBorder,
                         .compare = true },
        };
        if (!gfx::succeeded(gfx::create_depth_target(device_, target, &map_))) {
            ENGINE_LOG_ERROR("The shadow map was not created.");
            return false;
        }

        if (!build_pipeline(content, pipeline_)) {
            ENGINE_LOG_ERROR("The shadow pipeline did not build.");
            return false;
        }
        return true;
    }

    void ShadowPass::destroy() {
        if (device_ == nullptr) {
            return;
        }
        gfx::destroy_pipeline(device_, pipeline_);
        pipeline_ = gfx::PipelineHandle{};
        gfx::destroy_texture(device_, map_);
        map_ = gfx::TextureHandle{};
        device_ = nullptr;
    }

    bool ShadowPass::reload_shaders(const assets::Content& content) {
        if (device_ == nullptr) {
            return false;
        }

        gfx::PipelineHandle rebuilt;
        if (!build_pipeline(content, rebuilt)) {
            // The one that is drawing stays. Somebody editing a shader breaks it
            // often, and losing the picture on every typo makes the loop useless.
            ENGINE_LOG_WARN("The shadow shader would not build. Keeping the old pipeline.");
            return false;
        }

        gfx::device_wait_idle(device_);
        gfx::destroy_pipeline(device_, pipeline_);
        pipeline_ = rebuilt;
        ENGINE_LOG_INFO("The shadow pipeline was rebuilt.");
        return true;
    }

    bool ShadowPass::fit_light(const scene::World& world, const assets::Content& content,
                               MeshCache& meshes) {
        // The first directional light. The frame block carries several, and only
        // one casts until #135 gives each its own map.
        Vec3 direction = world_forward;
        bool found = false;
        for (const auto [entity, transform, light] :
             world.registry()
                 .view<const scene::WorldTransform, const scene::DirectionalLight>()
                 .each()) {
            // Local -Z, which is what the component documents and what the
            // shading already reads.
            direction = glm::normalize(Vec3{ transform.matrix * Vec4{ world_forward, 0.0F } });
            found = true;
            break;
        }
        if (!found) {
            return false;
        }

        // The volume to cover is everything that would be drawn. A tighter fit
        // would follow the camera, and that belongs with the cascades.
        Vec3 low{ std::numeric_limits<float>::max() };
        Vec3 high{ std::numeric_limits<float>::lowest() };
        bool any = false;
        for (const auto [entity, transform, renderer] :
             world.registry()
                 .view<const scene::WorldTransform, const scene::MeshRenderer>()
                 .each()) {
            if (!renderer.mesh.valid()) {
                continue;
            }
            const GpuMesh* mesh = meshes.get(device_, content, renderer.mesh);
            if (mesh == nullptr) {
                continue;
            }
            // Every corner of the box, because a rotation turns the box and the
            // two original corners no longer bound it.
            const std::array<Vec3, 2> ends{ mesh->min, mesh->max };
            for (const std::array<int, 3>& pick : kBoxCorners) {
                const Vec3 corner{ ends[static_cast<std::size_t>(pick[0])].x,
                                   ends[static_cast<std::size_t>(pick[1])].y,
                                   ends[static_cast<std::size_t>(pick[2])].z };
                const Vec3 world_corner{ transform.matrix * Vec4{ corner, 1.0F } };
                low = glm::min(low, world_corner);
                high = glm::max(high, world_corner);
            }
            any = true;
        }
        if (!any) {
            return false;
        }

        const Vec3 center = (low + high) * 0.5F;
        const float radius = glm::length(high - low) * 0.5F;

        // A sphere around the scene, so the fitted volume does not change size
        // as the light turns. Fitting the box in light space instead would make
        // the texel density wobble while the sun moves.
        const float extent = glm::max(radius, kSmallestExtent);
        const float distance = extent + kLightMargin;

        // Look from outside the volume, back along the light.
        const Vec3 eye = center - (direction * distance);
        // A light aimed straight down has no usable up vector, because the cross
        // product with world up is zero. Pick another one in that case.
        const Vec3 up = std::abs(glm::dot(direction, world_up)) > 0.99F ? world_right : world_up;
        const Mat4 view = glm::lookAt(eye, center, up);

        const Mat4 projection = orthographic_reverse_z(extent, extent, kLightMargin,
                                                       distance + extent + kLightMargin);
        light_view_projection_ = projection * view;
        return true;
    }

    void ShadowPass::draw(gfx::CommandList* commands, const scene::World& world,
                          const assets::Content& content, MeshCache& meshes) {
        draw_count_ = 0;
        has_light_ = fit_light(world, content, meshes);

        // The scope opens either way. A map left unwritten holds whatever the
        // allocation had, and the mesh pass samples it whether a light was found
        // or not, so it has to be cleared to the far plane.
        gfx::cmd_begin_depth_rendering(commands, map_);
        if (!has_light_) {
            gfx::cmd_end_rendering(commands);
            return;
        }

        gfx::cmd_bind_pipeline(commands, pipeline_);
        // Front faces, for the reason build_pipeline() gives.
        gfx::cmd_set_cull_mode(commands, false);

        for (const auto [entity, transform, renderer] :
             world.registry()
                 .view<const scene::WorldTransform, const scene::MeshRenderer>()
                 .each()) {
            if (!renderer.mesh.valid()) {
                continue;
            }
            const GpuMesh* mesh = meshes.get(device_, content, renderer.mesh);
            if (mesh == nullptr) {
                continue;
            }

            const Push push{ .light_view_projection = light_view_projection_,
                             .model = transform.matrix };
            gfx::cmd_push_constants(commands, pipeline_, &push, sizeof(push));
            gfx::cmd_bind_vertex_buffer(commands, mesh->vertices);
            gfx::cmd_bind_index_buffer(commands, mesh->indices);

            // One draw for the whole mesh rather than one for each submesh. A
            // submesh exists to change material, and depth has no material. The
            // index ranges are contiguous, so the whole buffer is one draw.
            std::uint32_t indices = 0;
            for (const assets::MeshSubmesh& submesh : mesh->submeshes) {
                indices += submesh.index_count;
            }
            gfx::cmd_draw_indexed(commands, indices, 1, 0, 0);
            ++draw_count_;
        }

        gfx::cmd_end_rendering(commands);
    }

} // namespace engine::render
