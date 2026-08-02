#pragma once

/**
 * @file
 * @brief The ImGui overlay, drawn through the same device as everything else.
 *
 * ImGui itself is not a Vulkan library, so a caller draws its windows with
 * plain ImGui calls. Only the part that talks to the GPU lives behind this
 * header, because the ImGui Vulkan backend includes vulkan.h and rule 4.1 keeps
 * that under src/gfx/vulkan/.
 *
 * The order in one frame is:
 *
 * 1. imgui_process_event() for each event the window drains.
 * 2. imgui_new_frame() after begin_frame().
 * 3. Your ImGui window calls.
 * 4. imgui_render() between cmd_begin_rendering() and cmd_end_rendering().
 */

#include "gfx/types.h"

namespace engine::gfx {

    struct Device;
    struct CommandList;

    /**
     * @brief Starts ImGui and attaches it to the window and the device.
     *
     * The overlay draws into the swapchain image with dynamic rendering, so it
     * needs no render pass of its own. It declares the same color and depth
     * formats the rest of the frame uses.
     *
     * @param device The device that owns the swapchain.
     * @param sdl_window The SDL window handle, from platform::Window::native().
     * @return Result::Success, or the reason the overlay did not start.
     *
     * @warning Call this after create_device() and destroy it with
     * imgui_shutdown() before destroy_device().
     */
    [[nodiscard]] Result imgui_init(Device* device, void* sdl_window);

    /**
     * @brief Releases everything imgui_init() built.
     * @param device The device the overlay was attached to. A null pointer does nothing.
     */
    void imgui_shutdown(Device* device);

    /**
     * @brief Hands one window event to ImGui.
     *
     * Call this for every event, before the application acts on it. ImGui then
     * reports through imgui_wants_input() whether it took the event.
     *
     * @param sdl_event A pointer to the SDL_Event.
     */
    void imgui_process_event(const void* sdl_event);

    /**
     * @brief Whether ImGui is using the keyboard or the mouse this frame.
     *
     * A game reads this before it moves a camera, so a click on a window does
     * not also turn the view.
     *
     * @param mouse Receives whether ImGui wants the mouse. May be null.
     * @param keyboard Receives whether ImGui wants the keyboard. May be null.
     */
    void imgui_wants_input(bool* mouse, bool* keyboard);

    /// @brief Opens an ImGui frame. Call it once, after begin_frame().
    void imgui_new_frame();

    /**
     * @brief Records the ImGui draw lists into the open frame.
     *
     * Call this inside the rendering scope, so the overlay lands on top of what
     * the passes drew.
     *
     * @param commands The command list from begin_frame().
     */
    void imgui_render(CommandList* commands);

} // namespace engine::gfx
