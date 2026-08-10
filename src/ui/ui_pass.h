#pragma once

/**
 * @file ui_pass.h
 * @brief Draws what engine::ui::Renderer recorded, over the finished frame.
 */

#include "assets/content.h"
#include "gfx/device.h"
#include "ui/renderer.h"

#include <array>
#include <cstddef>

namespace engine::ui {

    /**
     * @brief Uploads a recording and issues one draw for each batch.
     *
     * This draws inside the same rendering scope as the tonemap, straight onto
     * the swapchain image. Game UI is not part of the scene, so it takes no
     * exposure and no tone curve. It is authored in the colours it should
     * appear in.
     *
     * It is not a pass in the render graph. It writes the image the tonemap
     * already declared and shares that scope, so there is no barrier to
     * derive. Rule 4.6 says to build the declaration when something needs it.
     *
     * @warning The vertex and index buffers are rebuilt every frame, because
     *          gfx has no dynamic vertex buffer. See issue #204. That is the
     *          largest gap the M6 spike found, and it is why this class does
     *          not keep a capacity.
     */
    class UiPass {
    public:
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
        /// @brief Replaces a buffer with a new one holding @p bytes of @p data.
        [[nodiscard]] bool upload(gfx::BufferHandle& buffer, const void* data, std::size_t bytes,
                                  gfx::BufferUsage usage);

        gfx::Device* device_ = nullptr;

        /// @brief The pipeline for a run that replaces what is under it.
        gfx::PipelineHandle opaque_;
        /// @brief The pipeline for a run that blends over what is under it.
        gfx::PipelineHandle blended_;

        /// One buffer set for each slot, so a buffer a frame in flight still
        /// reads is never destroyed. Two frames are in flight, and three slots
        /// leave a margin rather than relying on that number staying two.
        static constexpr std::size_t kSlots = 3;
        std::array<gfx::BufferHandle, kSlots> vertices_{};
        std::array<gfx::BufferHandle, kSlots> indices_{};
        std::size_t slot_ = 0;
    };

}
