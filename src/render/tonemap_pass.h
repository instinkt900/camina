#pragma once

/**
 * @file
 * @brief Writes the scene image to the swapchain, through a full-screen pass.
 */

#include "assets/content.h"
#include "gfx/device.h"
#include "render/render_graph.h"

namespace engine::render {

    /**
     * @brief Owns the scene color target and writes it to the swapchain.
     *
     * The mesh pass used to render straight into the swapchain image, which is
     * an 8-bit sRGB format. Every value above 1 clipped at the moment the
     * fragment shader wrote it, and image based lighting and real lights both
     * produce those. So the scene renders into a half float image this pass
     * owns, and this pass reads it and writes the frame out.
     *
     * It applies no curve yet. The ACES fit and the exposure are M5.6b, in
     * issue #142. This half exists to put the target and the pass in place
     * without moving a pixel, which is a test the second half cannot have.
     *
     * @warning The target is the size of the window, so resize() has to run
     * whenever the swapchain is rebuilt. Nothing else notices.
     */
    class TonemapPass {
    public:
        /**
         * @brief What the pass reads and writes, for the render graph.
         *
         * It reads the scene color the mesh pass wrote and writes the swapchain
         * image. It also writes the frame depth, which it does not draw with:
         * the rendering scope attaches the depth image and clears it, and a
         * declaration that left that out would be a write the graph never
         * ordered. See issue #143.
         *
         * The span points at storage with static lifetime.
         *
         * @return The declaration.
         */
        [[nodiscard]] static PassDesc declare();

        /**
         * @brief Builds the target, the pipeline, and the set that binds them.
         *
         * @param device The device to build on. Held, not owned.
         * @param content The engine content tree, which holds the cooked
         * tonemap shaders.
         * @param extent The size of the swapchain, which the target matches.
         * @return False when the target or the pipeline did not build.
         */
        [[nodiscard]] bool create(gfx::Device* device, const assets::Content& content,
                                  gfx::Extent2D extent);

        /// @brief Releases everything create() built. Safe to call twice.
        void destroy();

        /**
         * @brief Rebuilds the target at a new size.
         *
         * The set names the old image, so it is rebuilt with it. This waits for
         * the device, because a frame in flight may still be reading either.
         *
         * @param extent The new size of the swapchain.
         * @return False when the new target did not build, which leaves the
         * pass with no target and the frame unable to draw.
         */
        [[nodiscard]] bool resize(gfx::Extent2D extent);

        /**
         * @brief Builds a new pipeline from the cooked shaders and swaps it in.
         *
         * The old pipeline stays when the new one will not build, because
         * somebody editing a shader breaks it often.
         *
         * @param content The engine content tree.
         * @return True when the pipeline was replaced.
         */
        [[nodiscard]] bool reload_shaders(const assets::Content& content);

        /**
         * @brief The image the scene renders into.
         *
         * @return The handle, which is null until create() succeeds. The mesh
         * pass draws into this rather than into the swapchain image.
         */
        [[nodiscard]] gfx::TextureHandle target() const { return target_; }

        /**
         * @brief Draws the full screen triangle that writes the frame out.
         *
         * @param commands The open command list.
         *
         * @warning Call this inside a gfx::cmd_begin_rendering() scope, which is
         * the scope over the swapchain image. The scene renders in a scope of
         * its own that has already closed by then.
         */
        void draw(gfx::CommandList* commands);

    private:
        /// Builds the half float target at @p extent.
        [[nodiscard]] bool build_target(gfx::Extent2D extent);
        /// Builds a pipeline from the cooked shaders into @p out.
        [[nodiscard]] bool build_pipeline(const assets::Content& content,
                                          gfx::PipelineHandle& out);
        /// Builds the set that binds the target. Needs a live pipeline and target.
        [[nodiscard]] bool build_set();

        gfx::Device* device_ = nullptr;
        gfx::PipelineHandle pipeline_;
        gfx::TextureHandle target_;
        gfx::DescriptorSetHandle set_;
    };

} // namespace engine::render
