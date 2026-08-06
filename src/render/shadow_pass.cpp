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

        /// The size of one cascade layer, on each axis.
        constexpr std::uint32_t kShadowMapSize = 2048;

        /**
         * How much of the split is logarithmic rather than uniform.
         *
         * A uniform split gives every cascade the same depth and wastes the
         * near ones, because perspective puts most of the pixels close to the
         * camera. A purely logarithmic split is the opposite and leaves the far
         * cascade covering almost everything. The usual answer is a blend, and
         * this is how much of it comes from the logarithmic term.
         *
         * A frustum slice is a sphere whose radius follows the width of the
         * view, not the depth of the slice, so a near cascade has to be pulled
         * in hard before it covers less world than the one behind it. That is
         * why this leans as far towards logarithmic as it does.
         */
        constexpr float kSplitBlend = 0.85F;

        /**
         * How many texels of slack the depth bias covers, before the slope term.
         *
         * The bias each cascade needs is worked out from its own texel size, so
         * this is the one number that is the same for all of them. See
         * ShadowPass::cascade_biases().
         */
        constexpr float kBiasTexels = 1.75F;

        /// Keeps the near plane off the geometry, so a caster just outside the
        /// fitted volume still writes depth rather than being clipped away.
        constexpr float kLightMargin = 1.0F;

        /// A scene with nothing in it has no volume to fit, and a zero-sized
        /// projection divides by zero. This is the smallest half extent used.
        constexpr float kSmallestExtent = 0.01F;

        /// The eight corners of an axis aligned box, as (x, y, z) selectors.
        constexpr std::array<std::array<int, 3>, 8> kBoxCorners{ {
            { 0, 0, 0 },
            { 1, 0, 0 },
            { 0, 1, 0 },
            { 1, 1, 0 },
            { 0, 0, 1 },
            { 1, 0, 1 },
            { 0, 1, 1 },
            { 1, 1, 1 },
        } };

        /**
         * Where the cascades start, in meters in front of the camera.
         *
         * This is the camera's own near plane. It has to match `kNearPlane` in
         * the runtime, because the split turns a distance into a clip depth with
         * it and a mismatch would slide every cascade boundary.
         */
        constexpr float kNearDistance = 0.1F;

        /// A frustum slice is a box, and a box has eight corners.
        constexpr std::size_t kFrustumCorners = 8;

        /// Half, for a midpoint or a radius from a diameter.
        constexpr float kHalf = 0.5F;

        /// The smallest depth range a cascade may report, so nothing divides by
        /// zero when a slice collapses.
        constexpr float kSmallestRange = 1.0e-4F;

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

    ShadowPass::CascadeFit ShadowPass::fit_slice(const Mat4& to_world, float near_distance,
                                                 float far_distance) {
        // The projection is infinite reverse-Z, so a world distance d in front
        // of the camera lands at clip depth near/d. That is what turns a split
        // distance back into the depth the unprojection needs.
        const float near_depth = kNearDistance / std::max(near_distance, kNearDistance);
        const float far_depth = kNearDistance / std::max(far_distance, kNearDistance);

        std::array<Vec3, kFrustumCorners> corners{};
        std::size_t at = 0;
        for (const float depth : { near_depth, far_depth }) {
            for (const float y : { -1.0F, 1.0F }) {
                for (const float x : { -1.0F, 1.0F }) {
                    const Vec4 point = to_world * Vec4{ x, y, depth, 1.0F };
                    corners[at] = Vec3{ point } / point.w;
                    ++at;
                }
            }
        }

        // A sphere around the slice rather than a box in light space. A box
        // changes size as the camera turns, and the shadow then swims because
        // every texel covers a different amount of world from frame to frame. A
        // sphere is the same whichever way the camera points.
        Vec3 center{ 0.0F };
        for (const Vec3& corner : corners) {
            center += corner;
        }
        center /= static_cast<float>(corners.size());

        float radius = 0.0F;
        for (const Vec3& corner : corners) {
            radius = glm::max(radius, glm::distance(center, corner));
        }

        return CascadeFit{ .center = center, .radius = glm::max(radius, kSmallestExtent) };
    }

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
            // Both faces. A wall of the room is a single quad with no back, so
            // culling either side of it would drop it out of the map and let
            // the light through a solid surface.
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
            .layer_count = static_cast<std::uint32_t>(kCascadeCount),
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

    bool ShadowPass::fit_cascades(const scene::World& world, const assets::Content& content,
                                  MeshCache& meshes, const Mat4& camera_view_projection) {
        // The first directional light. The frame block carries several, and only
        // one casts, because there is one map.
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

        // How far back the light has to stand to catch a caster that is outside
        // the slice but still between the light and it. The whole scene is the
        // safe answer, and the depth format is a 32-bit float, so spending range
        // here costs little.
        float scene_radius = 0.0F;
        {
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
            scene_radius = glm::length(high - low) * kHalf;
        }

        const Mat4 to_world = glm::inverse(camera_view_projection);
        // A light aimed straight down has no usable up vector, because the cross
        // product with world up is zero. Pick another one in that case.
        const Vec3 up = std::abs(glm::dot(direction, world_up)) > 0.99F ? world_right : world_up;

        float near_distance = kNearDistance;
        for (std::size_t cascade = 0; cascade < kCascadeCount; ++cascade) {
            // The practical split: part logarithmic, part uniform. See
            // kSplitBlend.
            const float part = static_cast<float>(cascade + 1) /
                               static_cast<float>(kCascadeCount);
            const float logarithmic =
                kNearDistance * std::pow(kShadowDistance / kNearDistance, part);
            const float uniform = kNearDistance + ((kShadowDistance - kNearDistance) * part);
            const float far_distance =
                (kSplitBlend * logarithmic) + ((1.0F - kSplitBlend) * uniform);
            splits_[cascade] = far_distance;

            const CascadeFit fit = fit_slice(to_world, near_distance, far_distance);

            // One texel of this cascade, in world units. Everything below is
            // sized from it, which is what makes the bias hold across cascades.
            const float texel_world = (fit.radius / kHalf) / static_cast<float>(kShadowMapSize);

            // Snap the center to whole texels in light space. Without this the
            // volume slides by a fraction of a texel whenever the camera moves,
            // and every shadow edge crawls even though nothing in the scene has
            // moved.
            const Mat4 snap_view = glm::lookAt(fit.center - (direction * fit.radius), fit.center, up);
            Vec3 center_light{ snap_view * Vec4{ fit.center, 1.0F } };
            center_light.x = std::floor(center_light.x / texel_world) * texel_world;
            center_light.y = std::floor(center_light.y / texel_world) * texel_world;
            const Vec3 center{ glm::inverse(snap_view) * Vec4{ center_light, 1.0F } };

            const float back = fit.radius + scene_radius;
            const Vec3 eye = center - (direction * back);
            const Mat4 view = glm::lookAt(eye, center, up);
            const float far_plane = back + fit.radius;
            const Mat4 projection =
                orthographic_reverse_z(fit.radius, fit.radius, kLightMargin, far_plane);
            light_view_projections_[cascade] = projection * view;

            // The depth this cascade spans, so a world offset becomes a value in
            // the map. A cascade covering more world has a longer range, and
            // that is exactly why one bias cannot serve them all.
            const float depth_range = far_plane - kLightMargin;
            biases_[cascade] = (kBiasTexels * texel_world) / std::max(depth_range, kSmallestRange);

            near_distance = far_distance;
        }
        return true;
    }

    void ShadowPass::draw(gfx::CommandList* commands, const scene::World& world,
                          const assets::Content& content, MeshCache& meshes,
                          const Mat4& camera_view_projection) {
        draw_count_ = 0;
        has_light_ = fit_cascades(world, content, meshes, camera_view_projection);

        for (std::size_t cascade = 0; cascade < kCascadeCount; ++cascade) {
            // Every layer opens a scope even with no light, because the clear is
            // what puts the far plane in a layer nobody rendered into. A layer
            // left alone holds whatever the allocation had.
            if (!gfx::cmd_begin_depth_rendering(commands, map_,
                                                static_cast<std::uint32_t>(cascade))) {
                continue;
            }
            if (!has_light_) {
                gfx::cmd_end_rendering(commands);
                continue;
            }

            gfx::cmd_bind_pipeline(commands, pipeline_);
            // Both faces, for the reason build_pipeline() gives.
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

                const Push push{ .light_view_projection = light_view_projections_[cascade],
                                 .model = transform.matrix };
                gfx::cmd_push_constants(commands, pipeline_, &push, sizeof(push));
                gfx::cmd_bind_vertex_buffer(commands, mesh->vertices);
                gfx::cmd_bind_index_buffer(commands, mesh->indices);

                // One draw for the whole mesh rather than one for each submesh.
                // A submesh exists to change material, and depth has no
                // material. The index ranges are contiguous, so the whole buffer
                // is one draw.
                std::uint32_t indices = 0;
                for (const assets::MeshSubmesh& submesh : mesh->submeshes) {
                    indices += submesh.index_count;
                }
                gfx::cmd_draw_indexed(commands, indices, 1, 0, 0);
                ++draw_count_;
            }

            gfx::cmd_end_rendering(commands);
        }
    }

} // namespace engine::render
