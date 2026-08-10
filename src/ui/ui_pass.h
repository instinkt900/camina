#pragma once

/**
 * @file ui_pass.h
 * @brief Draws what engine::ui::Renderer recorded, over the finished frame.
 */

#include "assets/content.h"
#include "gfx/device.h"
#include "render/render_graph.h"
#include "ui/renderer.h"

namespace engine::ui {

    /**
     * @brief Uploads a recording and issues one draw for each batch.
     *
     * This runs after the tonemap, straight onto the swapchain image. Game UI
     * is not part of the scene, so it takes no exposure and no tone curve. It
     * is authored in the colours it should appear in.
     *
     * The vertex and index buffers grow to fit and never shrink, because a UI
     * reaches a steady size within a few frames and a free every frame would
     * cost more than the memory does.
     *
     * @warning The buffers are host visible, and one set of them serves every
     *          frame in flight. draw() must therefore run inside the frame that
     *          recorded it, which it does, because the runtime records and
     *          draws in one place.
     */
    class UiPass {
    public:
        /**
         * @brief What the pass reads and writes, for the render graph.
         *
         * It writes the swapchain image and reads nothing. It also writes the
         * frame depth for the reason TonemapPass::declare gives: the scope
         * attaches the depth image, so a declaration that left it out would be
         * a write the graph never ordered.
         *
         * The span points at storage with static lifetime.
         *
         * @return The declaration.
         */
        [[nodiscard]] static render::PassDesc declare();

        /**
         * @brief Builds the pipelines this pass draws with.
         *
         * @param device The device to build on. Held, not owned.
         * @param content The engine content tree, which holds the cooked ui
         * shaders.
         * @return False when a pipeline did not build.
         */
        [[nodiscard]] bool create(gfx::Device* device, const assets::Content& content);

        /// @brief Releases everything create() built. Safe to call twice.
        void destroy();

        /**
         * @brief Uploads a recording and draws it.
         *
         * A recording with no batch draws nothing and costs no upload.
         *
         * @param commands The command list from begin_frame().
         * @param renderer The recorder, after its end() call.
         * @param extent The size of the swapchain image being drawn into.
         */
        void draw(gfx::CommandList* commands, const Renderer& renderer, gfx::Extent2D extent);

    private:
        /// @brief Grows a buffer to hold at least @p bytes, keeping no contents.
        [[nodiscard]] bool ensure_capacity(gfx::BufferHandle& buffer, std::size_t& capacity,
                                           std::size_t bytes, gfx::BufferUsage usage);

        gfx::Device* device_ = nullptr;

        /// @brief The pipeline for a run that replaces what is under it.
        gfx::PipelineHandle opaque_;
        /// @brief The pipeline for a run that blends over what is under it.
        gfx::PipelineHandle blended_;

        gfx::BufferHandle vertices_;
        gfx::BufferHandle indices_;
        std::size_t vertex_capacity_ = 0;
        std::size_t index_capacity_ = 0;
    };

}
