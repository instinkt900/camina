#pragma once

/**
 * @file
 * @brief The image an editor renders the scene into and shows in a panel.
 *
 * A runtime fills the window with the scene and the overlay floats over it. An
 * editor puts chrome around the picture instead, so the scene has to land in an
 * image of its own before ImGui can draw it inside a panel.
 *
 * This owns that image and the binding ImGui draws it through. It names no
 * ImGui type and no Vulkan type, so it follows the same rule the panels do.
 */

#include "gfx/device.h"
#include "gfx/imgui.h"
#include "gfx/types.h"

namespace engine::editor {

    /// @brief What Viewport::ensure() did.
    enum class ViewportChange {
        None,    ///< The target already matched. Nothing was rebuilt.
        Rebuilt, ///< A new image is in place, and the old one is gone.
        Failed,  ///< The new image did not build, and there is nothing to draw into.
    };

    /**
     * @brief The color target an editor viewport panel shows.
     *
     * It is an 8-bit sRGB image, the same format the swapchain uses, because
     * the tonemap pass is the last step of the scene and its pipeline declares
     * that format. The values are display colors by then, so the round trip
     * through this image and back out through the overlay changes nothing.
     *
     * @warning The target is never larger than the swapchain image.
     * gfx::cmd_begin_color_rendering() attaches the frame depth image, which is
     * the size of the window, and a render area has to fit inside every
     * attachment. A docked panel is always inside the window, so the clamp is a
     * guard rather than a live condition until a panel becomes an OS window of
     * its own. See issue #306.
     */
    class Viewport {
    public:
        /**
         * @brief Builds the first image.
         *
         * @param device The device to build on. Held, not owned.
         * @param extent The size to start at, usually the size of the window.
         * @param limit The size of the swapchain image, which @p extent is
         * clamped to.
         * @return False when the image or its binding did not build.
         */
        [[nodiscard]] bool create(gfx::Device* device, gfx::Extent2D extent,
                                  gfx::Extent2D limit);

        /// @brief Releases the image and its binding. Safe to call twice.
        void destroy();

        /**
         * @brief Makes the image match what the panel asked for.
         *
         * A rebuild waits for the device, because a frame in flight may still
         * be reading the old image and the overlay may still be holding its
         * binding. So this belongs at the top of a frame, before
         * gfx::begin_frame(), and never in the middle of one.
         *
         * @param wanted The size the panel reported. Zero in either direction
         * does nothing, which is what a closed or collapsed panel reports.
         * @param limit The size of the swapchain image, which @p wanted is
         * clamped to.
         * @return What it did. A caller that gets ViewportChange::Rebuilt tells
         * the renderer to forget the old image's state.
         */
        ViewportChange ensure(gfx::Extent2D wanted, gfx::Extent2D limit);

        /// @brief The image the scene is tonemapped into.
        /// @return The handle, or a null handle before create() succeeded.
        [[nodiscard]] gfx::TextureHandle target() const { return target_; }

        /// @brief What the panel draws.
        /// @return The binding, or gfx::kInvalidImGuiTexture when there is none.
        [[nodiscard]] gfx::ImGuiTextureId picture() const { return picture_; }

        /// @brief How big the image is, which is what the scene renders at.
        /// @return The size, or zero before create() succeeded.
        [[nodiscard]] gfx::Extent2D extent() const { return extent_; }

        /// @brief Whether there is an image to draw into.
        /// @return True once create() succeeded and no rebuild has failed since.
        [[nodiscard]] bool ready() const { return target_.valid(); }

    private:
        /// Builds the image and its binding at @p extent, into the members.
        [[nodiscard]] bool build(gfx::Extent2D extent);
        /// Releases the image and its binding, and clears both members.
        void release();

        gfx::Device* device_ = nullptr;
        gfx::TextureHandle target_;
        gfx::ImGuiTextureId picture_ = gfx::kInvalidImGuiTexture;
        gfx::Extent2D extent_{};
    };

} // namespace engine::editor
