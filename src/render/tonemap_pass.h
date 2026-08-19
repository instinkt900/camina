#pragma once

/**
 * @file
 * @brief Writes the scene image to the swapchain, through a full-screen pass.
 */

#include "assets/asset_source.h"
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
     * It applies exposure and then the ACES curve. Exposure scales the scene
     * first, because the curve is not a straight line, and the curve maps what
     * is left into the range a display can show.
     *
     * @warning The target is the size of the window, so resize() has to run
     * whenever the swapchain is rebuilt. Nothing else notices.
     */
    class TonemapPass {
    public:
        /**
         * @brief What the pass reads and writes, for the render graph.
         *
         * It reads the scene color the mesh pass wrote and writes whichever
         * image the caller maps it down into. It touches no depth image, which
         * is what #143 closed: the triangle covers every pixel and neither
         * tests depth nor writes it.
         *
         * The spans point at storage with static lifetime.
         *
         * @param target Where the picture goes. kFrameColor fills the window,
         * and kViewportColor puts it in an image an editor shows in a panel.
         * @return The declaration.
         */
        [[nodiscard]] static PassDesc declare(ResourceId target = kFrameColor);

        /**
         * @brief Builds the target, the pipeline, and the set that binds them.
         *
         * @param device The device to build on. Held, not owned.
         * @param content The engine content tree, which holds the tonemap
         * shaders.
         * @param extent The size of the swapchain, which the target matches.
         * @return False when the target or the pipeline did not build.
         */
        [[nodiscard]] bool create(gfx::Device* device, const assets::AssetSource& content,
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
        [[nodiscard]] bool reload_shaders(const assets::AssetSource& content);

        /**
         * @brief The image the scene renders into.
         *
         * @return The handle, which is null until create() succeeds. The mesh
         * pass draws into this rather than into the swapchain image.
         */
        [[nodiscard]] gfx::TextureHandle target() const { return target_; }

        /**
         * @brief Draws the full-screen triangle that writes the frame out.
         *
         * @param commands The open command list.
         * @param exposure A linear scale on the scene before the curve. One is
         * neutral, and a value at or below zero is refused with a message
         * because it would make the whole frame black.
         *
         * @warning Call this inside a gfx::cmd_begin_rendering() scope, which is
         * the scope over the swapchain image. The scene renders in a scope of
         * its own that has already closed by then.
         */
        void draw(gfx::CommandList* commands, float exposure);

    private:
        /// Builds the half float target at @p extent.
        [[nodiscard]] bool build_target(gfx::Extent2D extent);
        /// Builds a pipeline from the cooked shaders into @p out.
        [[nodiscard]] bool build_pipeline(const assets::AssetSource& content,
                                          gfx::PipelineHandle& out);
        /**
         * Builds the set that binds the target into @p out.
         *
         * It takes the pipeline rather than reading the member, so a reload can
         * build the replacement set against the new pipeline while the old one
         * is still the live one. A set is allocated against a pipeline's layout,
         * so the two cannot be swapped in separate steps without a window where
         * neither is whole.
         *
         * @param pipeline The pipeline whose layout the set is allocated against.
         * @param out Receives the set. Untouched on failure.
         * @return False when the set could not be built.
         */
        [[nodiscard]] bool build_set(gfx::PipelineHandle pipeline, gfx::DescriptorSetHandle& out);

        gfx::Device* device_ = nullptr;
        gfx::PipelineHandle pipeline_;
        gfx::TextureHandle target_;
        gfx::DescriptorSetHandle set_;
    };

} // namespace engine::render
