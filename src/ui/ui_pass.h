#pragma once

/**
 * @file ui_pass.h
 * @brief Draws what engine::ui::Renderer recorded, over the finished frame.
 */

#include "assets/asset_source.h"
#include "gfx/device.h"
#include "ui/renderer.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>

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
        [[nodiscard]] bool create(gfx::Device* device, const assets::AssetSource& content);

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

        /**
         * @brief The descriptor set that binds one texture, built on first use.
         *
         * A set is kept for the life of the pass, because a layout draws the
         * same handful of images every frame and building one for each draw
         * would empty the descriptor pool in seconds.
         *
         * @param texture The texture to bind, or a null handle for the white
         * default a run with no image uses.
         * @return The set, or a null handle when it could not be built.
         */
        [[nodiscard]] gfx::DescriptorSetHandle set_for(gfx::TextureHandle texture);

        /**
         * @brief Reports once that a recorded texture filter is not applied.
         *
         * @param filter What the run asked for.
         */
        void report_filter_gap(moth_ui::TextureFilter filter);

        gfx::Device* device_ = nullptr;

        /// @brief The pipeline for a run that replaces what is under it.
        gfx::PipelineHandle opaque_;
        /// @brief The pipeline for a run that blends over what is under it.
        gfx::PipelineHandle blended_;

        /**
         * @brief One white texel, for a run that draws no image.
         *
         * The fragment stage multiplies the vertex colour by the texture, and
         * white is the identity of that multiply. So a rectangle and an image
         * take the same pipeline and the same set layout, and the pass holds
         * two pipelines rather than four.
         *
         * This is not `render::TextureCache::fallback()`. That cache belongs to
         * whoever loads the images, and this pass has no reason to know about
         * one.
         */
        gfx::TextureHandle white_;

        /**
         * @brief One descriptor set for each texture a recording named.
         *
         * Keyed on the raw handle value, because a handle is generational and
         * two live textures cannot share one. The null key is @c white_.
         *
         * @warning A texture that is freed while a set here still names it
         * leaves the set pointing at nothing. Nothing frees a UI texture today,
         * because @c engine::ui::ImageFactory holds its own cache for the whole
         * run. Hot reload of a UI image is issue #210.
         */
        std::map<std::uint64_t, gfx::DescriptorSetHandle> sets_;

        /// Whether a run has already reported that its filter was ignored. The
        /// gap is issue #209, and one line for the whole run says enough.
        bool reported_filter_ = false;

        /// One buffer set for each slot, so a buffer a frame in flight still
        /// reads is never destroyed. Two frames are in flight, and three slots
        /// leave a margin rather than relying on that number staying two.
        static constexpr std::size_t kSlots = 3;
        std::array<gfx::BufferHandle, kSlots> vertices_{};
        std::array<gfx::BufferHandle, kSlots> indices_{};
        std::size_t slot_ = 0;
    };

}
