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

#include <cstdint>

namespace engine::gfx {

    struct Device;
    struct CommandList;

    /**
     * @brief What ImGui calls a texture, as a number the caller passes back.
     *
     * ImGui names its own type for this and rule 4.1 keeps the backend out of
     * every header above `gfx::`. So a caller takes one of these from
     * imgui_texture_id() and hands it to ImGui::Image(), and nothing in
     * between has to know what is inside it.
     */
    using ImGuiTextureId = std::uint64_t;

    /// @brief The value imgui_texture_id() returns when it could not bind one.
    inline constexpr ImGuiTextureId kInvalidImGuiTexture = 0;

    /**
     * @brief Settings for imgui_init(), chosen by the application.
     *
     * Docking and the saved layout are the two settings the editor and the
     * runtime answer differently, so neither is decided here. The editor wants
     * both. The runtime debug overlay wants neither, because its windows open
     * at fixed places and a run always starts from the same layout. See
     * DESIGN.md section 10, M9.
     */
    struct ImGuiDesc {
        /// @brief The SDL window handle, from platform::Window::native().
        void* sdl_window = nullptr;

        /// @brief Whether a window can dock into another one.
        ///
        /// The dockspace itself belongs to the application, because only the
        /// application knows what else is on screen. This flag is what lets it
        /// build one.
        bool docking = false;

        /**
         * @brief Whether a panel dragged off the window becomes an OS window.
         *
         * ImGui calls this multi-viewport. The backends do most of it: the SDL3
         * one creates the extra windows and the Vulkan one creates their
         * surfaces, their swapchains, and the pipeline they draw with. Nothing
         * above `gfx::` learns that a second swapchain exists.
         *
         * The editor asks for it. The runtime debug overlay does not, because
         * its windows open at fixed places and a run has to look the same every
         * time.
         *
         * @warning An offscreen run has no window at all, so this does nothing
         * there and an offscreen capture stays what it was.
         */
        bool viewports = false;

        /**
         * @brief Whether a floating panel inside the main window stays in it.
         *
         * True is what a person expects: a panel floats over the window until
         * they drag it off, and only then does it become an OS window.
         *
         * False puts every floating panel in a window of its own straight away.
         * That is how the multi-viewport path is checked without a hand on the
         * mouse, because nothing else can drag a panel out. `editor
         * --own-windows` asks for it.
         *
         * It does nothing unless `viewports` is on.
         */
        bool merge_viewports = true;

        /**
         * @brief Where ImGui saves the window layout, or null to save none.
         *
         * @warning ImGui keeps the pointer rather than a copy of the text, so
         * the string must outlive imgui_shutdown().
         */
        const char* ini_path = nullptr;
    };

    /**
     * @brief Starts ImGui and attaches it to the window and the device.
     *
     * The overlay draws into the swapchain image with dynamic rendering, so it
     * needs no render pass of its own. It declares the same color and depth
     * formats the rest of the frame uses.
     *
     * @param device The device that owns the swapchain.
     * @param desc The window and the per-application settings.
     * @return Result::Success, or the reason the overlay did not start.
     *
     * @warning Call this after create_device() and destroy it with
     * imgui_shutdown() before destroy_device().
     */
    [[nodiscard]] Result imgui_init(Device* device, const ImGuiDesc& desc);

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

    /**
     * @brief Draws and presents the panels that live in their own OS windows.
     *
     * Call it once for each frame, **after the frame that holds the main window
     * has been presented**. The extra windows have swapchains of their own, and
     * this submits and presents each of them.
     *
     * It does nothing unless `ImGuiDesc::viewports` asked for it, and nothing on
     * a frame where every panel is docked.
     */
    void imgui_render_platform_windows();

    /**
     * @brief Binds a texture so an ImGui window can draw it.
     *
     * The editor renders the scene into an image of its own and shows that
     * image in a panel, which is the one thing ImGui cannot do with a handle
     * from `gfx::`. This turns the handle into something ImGui::Image() takes.
     *
     * @param device The device that owns the texture.
     * @param texture The image to bind, from create_color_target().
     * @return The id, or kInvalidImGuiTexture when the handle was stale or the
     * overlay is not running.
     *
     * @warning The texture must be in ResourceState::ShaderRead by the time
     * imgui_render() records, and it must stay alive until then. The caller
     * issues that barrier.
     *
     * @warning Release it with imgui_release_texture() before the texture goes.
     * The overlay holds a small pool of these, so an image rebuilt on every
     * resize runs the pool out in a few seconds of dragging a panel edge.
     */
    [[nodiscard]] ImGuiTextureId imgui_texture_id(Device* device, TextureHandle texture);

    /**
     * @brief Releases what imgui_texture_id() bound.
     *
     * @param id The id to release. kInvalidImGuiTexture does nothing.
     *
     * @warning No frame may still be reading it. The caller waits for the
     * device first, the same way it does before it frees the image itself.
     */
    void imgui_release_texture(ImGuiTextureId id);

} // namespace engine::gfx
