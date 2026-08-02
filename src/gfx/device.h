#pragma once

/**
 * @file
 * @brief Device lifetime, the frame loop, and command recording.
 *
 * Every function here takes an opaque pointer. The backend owns the definition,
 * so no caller can reach a Vulkan type. See rule 4.1 in DESIGN.md.
 */

#include "gfx/types.h"

namespace engine::gfx {

    /**
     * @brief A live connection to one GPU, with its swapchain and frame state.
     *
     * The backend defines this type. Create one with create_device() and release
     * it with destroy_device().
     */
    struct Device;

    /**
     * @brief A recording context for one frame.
     *
     * begin_frame() hands one out. It stays valid until the matching end_frame().
     * Do not keep it past that point.
     */
    struct CommandList;

    /// @brief What begin_frame() reports about the frame it opened.
    struct FrameInfo {
        /// @brief Where to record commands for this frame.
        CommandList* commands = nullptr;
        /// @brief The size of the image this frame draws into.
        Extent2D extent;
        /// @brief Which slot in the frames-in-flight ring this frame uses.
        std::uint32_t frame_index = 0;
    };

    /**
     * @brief Starts the loader, picks a GPU, and builds the swapchain.
     *
     * @param desc The window, the application name, and the debug options.
     * @param out_device Receives the new device on success, and nullptr on failure.
     * @return Result::Success, or the reason the device did not start.
     *
     * @warning The device holds a surface built from `desc.window`. Destroy the
     * device before you destroy the window.
     *
     * @code
     * engine::gfx::Device* device = nullptr;
     * const auto result = engine::gfx::create_device(
     *     { .window = window.native(), .enable_validation = true }, &device);
     * @endcode
     */
    [[nodiscard]] Result create_device(const DeviceDesc& desc, Device** out_device);

    /**
     * @brief Waits for the GPU to go idle, then releases everything the device owns.
     * @param device The device to release. A null pointer is allowed and does nothing.
     */
    void destroy_device(Device* device);

    /**
     * @brief The name the driver reports for the chosen GPU.
     * @param device The device to query.
     * @return A static string owned by the device. It stays valid until destroy_device().
     */
    [[nodiscard]] const char* device_name(const Device* device);

    /**
     * @brief Blocks until the GPU finishes all submitted work.
     * @param device The device to wait on.
     */
    void device_wait_idle(Device* device);

    /**
     * @brief Rebuilds the swapchain at a new size.
     *
     * Call this when the window changes size, and when begin_frame() reports
     * Result::OutOfDate. A width or a height of zero means the window is
     * minimized, so the call does nothing and reports success.
     *
     * @param device The device to rebuild.
     * @param size The new client size in pixels.
     * @return Result::Success, or the reason the swapchain did not rebuild.
     */
    [[nodiscard]] Result device_resize(Device* device, Extent2D size);

    /**
     * @brief Waits for a free frame slot and acquires the next swapchain image.
     *
     * @param device The device to record against.
     * @param out_frame Receives the command list and the image size on success.
     * @return Result::Success when the frame opened. Result::OutOfDate when the
     * swapchain no longer matches the window, in which case no frame opened and
     * the caller must call device_resize() and try again.
     *
     * @warning Every successful call must be paired with end_frame().
     */
    [[nodiscard]] Result begin_frame(Device* device, FrameInfo* out_frame);

    /**
     * @brief Closes the command list, submits it, and presents the image.
     * @param device The device that opened the frame.
     * @return Result::Success, or Result::OutOfDate when the swapchain needs a rebuild.
     */
    [[nodiscard]] Result end_frame(Device* device);

    /**
     * @brief Opens dynamic rendering into the current swapchain image.
     *
     * The image loads with a clear to @p clear_color and stores at the end. There
     * is no render pass object and no framebuffer, per DESIGN.md section 2.
     *
     * @param commands The command list from begin_frame().
     * @param clear_color The linear color to clear to.
     */
    void cmd_begin_rendering(CommandList* commands, const ColorRGBA& clear_color);

    /**
     * @brief Closes the rendering scope that cmd_begin_rendering() opened.
     * @param commands The command list from begin_frame().
     */
    void cmd_end_rendering(CommandList* commands);

} // namespace engine::gfx
