#pragma once

/**
 * @file
 * @brief Renders the scene depth from a directional light, for shadowing.
 */

#include "assets/content.h"
#include "gfx/device.h"
#include "math/conventions.h"
#include "render/mesh_cache.h"
#include "render/render_graph.h"
#include "scene/world.h"

#include <array>

namespace engine::render {

    /**
     * @brief How many cascades the shadow map holds.
     *
     * Four is the usual answer and it is what the frame block is sized for.
     * `mesh.frag` reads the count from the frame block rather than this, so the
     * shader works for fewer. This is the ceiling, not the setting.
     */
    inline constexpr std::size_t kCascadeCount = 4;

    /**
     * @brief How far from the camera the cascades reach, in meters.
     *
     * The camera projection is infinite, so there is no far plane to split and
     * this is the distance that stands in for one. Past it nothing is shadowed.
     *
     * This is what decides how sharp the near cascade is, and it was measured
     * rather than chosen. The sandbox room is about 21 meters across its
     * diagonal. At 60 the near cascade fitted a sphere of radius 8.45 meters,
     * which is 0.83 cm for each texel, against 1.04 cm for the single map this
     * replaced. Four cascades for a fifth of the texel size is a poor trade.
     *
     * At 40 the near cascade fits 2.51 meters, which is 0.25 cm for each texel
     * and four times sharper than the single map. A scene that needs to see
     * further is what turns this into a component field.
     */
    inline constexpr float kShadowDistance = 40.0F;

    /**
     * @brief Renders scene depth from the point of view of a directional light.
     *
     * This is the first pass that writes a resource another pass reads. The
     * mesh pass samples what this one wrote, and the render graph derives the
     * barrier between them. See issue #87.
     *
     * The pass owns one shadow map with a layer for each cascade, and renders
     * one directional light into every layer. The frustum is split by depth so
     * the texel density follows the camera rather than the size of the scene.
     *
     * @warning It does not own the meshes. draw() takes the cache the mesh pass
     * already filled, because uploading a second copy of every vertex to render
     * depth would double the memory for nothing.
     */
    class ShadowPass {
    public:
        /// @brief Frees nothing. Call destroy() before this runs.
        ~ShadowPass();

        ShadowPass() = default;
        ShadowPass(const ShadowPass&) = delete;
        ShadowPass& operator=(const ShadowPass&) = delete;
        ShadowPass(ShadowPass&&) = delete;
        ShadowPass& operator=(ShadowPass&&) = delete;

        /**
         * @brief Builds the shadow map and the depth-only pipeline.
         * @param device The device to draw with.
         * @param content The engine content tree, which holds the shader.
         * @return True when both were built.
         */
        [[nodiscard]] bool create(gfx::Device* device, const assets::Content& content);

        /// @brief Frees the map and the pipeline.
        void destroy();

        /**
         * @brief Builds the pipeline again from the shader on disk.
         *
         * The same contract MeshPass::reload_shaders() has. A shader that will
         * not build leaves the pipeline that is drawing alone.
         *
         * @param content The engine content tree, which holds the shader.
         * @return True when a new pipeline was built and swapped in.
         *
         * @warning Call this between frames and never while a command list is
         * open. It waits for the GPU, which cannot happen mid-frame.
         */
        [[nodiscard]] bool reload_shaders(const assets::Content& content);

        /**
         * @brief What this pass reads and writes, for the render graph.
         *
         * It writes the shadow map as a depth target and reads nothing. The
         * mesh pass declares the matching read, and the graph turns the pair
         * into the barrier between them.
         *
         * The span points at storage with static lifetime, so the result can be
         * held for as long as the caller likes.
         *
         * @return The declaration.
         */
        [[nodiscard]] static PassDesc declare();

        /**
         * @brief Renders every shadow caster into the map.
         *
         * Splits the camera frustum by depth, fits a cascade to each slice, and
         * renders depth alone into one layer for each. A world with no
         * directional light renders nothing and leaves has_light() false, and
         * the mesh pass then lights the scene without a shadow term.
         *
         * @param commands The open command list.
         * @param world The world to read.
         * @param content The game content tree, which holds the meshes.
         * @param meshes The cache the mesh pass fills. Shared, not owned.
         * @param camera_view_projection The camera this frame. The cascades are
         * fitted to slices of its frustum, so they follow where a person looks.
         *
         * @warning Call this outside the frame's own rendering scope. It opens
         * one of its own over the shadow map.
         */
        void draw(gfx::CommandList* commands, const scene::World& world,
                  const assets::Content& content, MeshCache& meshes,
                  const Mat4& camera_view_projection);

        /// @brief The map the mesh pass samples.
        /// @return The handle, which is null until create() succeeds.
        [[nodiscard]] gfx::TextureHandle map() const { return map_; }

        /**
         * @brief One matrix for each cascade, world into that cascade's clip space.
         * @return The matrices the last draw() worked out. Identity when there
         * is no directional light.
         */
        [[nodiscard]] const std::array<Mat4, kCascadeCount>& light_view_projections() const {
            return light_view_projections_;
        }

        /**
         * @brief Where each cascade ends, as a distance in front of the camera.
         *
         * The shader compares a fragment's view depth against these to pick a
         * cascade, so they are what makes the split data rather than a constant.
         *
         * @return The far distance of each cascade, increasing.
         */
        [[nodiscard]] const std::array<float, kCascadeCount>& cascade_splits() const {
            return splits_;
        }

        /**
         * @brief The depth bias each cascade needs, in its own clip space.
         *
         * A cascade covering more world has larger texels and a longer depth
         * range, so a bias tuned for one does not carry to another. This is
         * worked out for each from its own texel size, and the shader scales it
         * by the slope of the surface.
         *
         * @return The base bias of each cascade.
         */
        [[nodiscard]] const std::array<float, kCascadeCount>& cascade_biases() const {
            return biases_;
        }

        /// @brief Whether the world has a directional light to cast from.
        /// @return False when the last draw() found none, so nothing shadows.
        [[nodiscard]] bool has_light() const { return has_light_; }

        /// @brief How many draw calls the last draw() made.
        /// @return The count, one for each mesh rather than each submesh.
        [[nodiscard]] std::size_t draw_count() const { return draw_count_; }

    private:
        /// Both matrices, in the order shadow.vert declares them.
        struct Push {
            Mat4 light_view_projection{ 1.0F }; ///< World into the light's clip space.
            Mat4 model{ 1.0F };                 ///< The caster being drawn.
        };

        /// What one cascade covers, before it becomes a matrix.
        struct CascadeFit {
            Vec3 center{ 0.0F }; ///< The middle of the slice, in world space.
            float radius = 0.0F; ///< The sphere around the slice.
        };

        /// Builds the depth-only pipeline from the shader in @p content.
        [[nodiscard]] bool build_pipeline(const assets::Content& content,
                                          gfx::PipelineHandle& out);

        /// The sphere around one slice of the camera frustum, in world space.
        /// @p to_world is the inverse of the camera's view projection.
        [[nodiscard]] static CascadeFit fit_slice(const Mat4& to_world, float near_distance,
                                                  float far_distance);

        /// Works out every cascade matrix from the light, the camera, and the
        /// scene. Returns false when there is no directional light, or nothing
        /// to cast.
        [[nodiscard]] bool fit_cascades(const scene::World& world, const assets::Content& content,
                                        MeshCache& meshes, const Mat4& camera_view_projection);

        gfx::Device* device_ = nullptr;
        gfx::TextureHandle map_;
        gfx::PipelineHandle pipeline_;
        std::array<Mat4, kCascadeCount> light_view_projections_{};
        std::array<float, kCascadeCount> splits_{};
        std::array<float, kCascadeCount> biases_{};
        bool has_light_ = false;
        std::size_t draw_count_ = 0;
    };

} // namespace engine::render
