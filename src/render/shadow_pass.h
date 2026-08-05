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

namespace engine::render {

    /**
     * @brief Renders scene depth from the point of view of a directional light.
     *
     * This is the first pass that writes a resource another pass reads. The
     * mesh pass samples what this one wrote, and the render graph derives the
     * barrier between them. See issue #87.
     *
     * The pass owns one shadow map and renders one directional light into it.
     * A cascade set is issue #135, and it changes how the map is sized and
     * chosen rather than anything about the pass being here.
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
         * Works out the light matrix from the first directional light in the
         * world and the bounds of everything that would be drawn, then renders
         * depth alone. A world with no directional light renders nothing and
         * leaves has_light() false, and the mesh pass then lights the scene
         * without a shadow term.
         *
         * @param commands The open command list.
         * @param world The world to read.
         * @param content The game content tree, which holds the meshes.
         * @param meshes The cache the mesh pass fills. Shared, not owned.
         *
         * @warning Call this outside the frame's own rendering scope. It opens
         * one of its own over the shadow map.
         */
        void draw(gfx::CommandList* commands, const scene::World& world,
                  const assets::Content& content, MeshCache& meshes);

        /// @brief The map the mesh pass samples.
        /// @return The handle, which is null until create() succeeds.
        [[nodiscard]] gfx::TextureHandle map() const { return map_; }

        /**
         * @brief The matrix that takes a world position into the light's clip space.
         * @return The matrix the last draw() worked out. Identity when there is
         * no directional light.
         */
        [[nodiscard]] const Mat4& light_view_projection() const { return light_view_projection_; }

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

        /// Builds the depth-only pipeline from the shader in @p content.
        [[nodiscard]] bool build_pipeline(const assets::Content& content,
                                          gfx::PipelineHandle& out);

        /// Works out the light matrix from the light and what the scene holds.
        /// Returns false when there is no directional light, or nothing to cast.
        [[nodiscard]] bool fit_light(const scene::World& world, const assets::Content& content,
                                     MeshCache& meshes);

        gfx::Device* device_ = nullptr;
        gfx::TextureHandle map_;
        gfx::PipelineHandle pipeline_;
        Mat4 light_view_projection_{ 1.0F };
        bool has_light_ = false;
        std::size_t draw_count_ = 0;
    };

} // namespace engine::render
