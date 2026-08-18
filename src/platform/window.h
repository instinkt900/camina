#pragma once

/**
 * @file
 * @brief The application window, backed by SDL3.
 */

#include "math/conventions.h"

struct SDL_Window;

/// @brief Windowing, input, and filesystem access.
namespace engine::platform {

    /// @brief Default window width in pixels.
    inline constexpr int kDefaultWindowWidth = 1280;
    /// @brief Default window height in pixels.
    inline constexpr int kDefaultWindowHeight = 720;

    /// @brief Settings for Window::create().
    struct WindowDesc {
        /// @brief Text shown in the title bar.
        const char* title = "camina";
        /// @brief Requested client width in pixels. The window manager may override it.
        int width = kDefaultWindowWidth;
        /// @brief Requested client height in pixels. The window manager may override it.
        int height = kDefaultWindowHeight;
        /// @brief Whether the user can resize the window.
        bool resizable = true;
        /**
         * @brief Whether Escape asks the window to close.
         *
         * A per-application choice, the way docking is for `gfx::ImGuiDesc`.
         * A game runtime quits on it. **The editor does not**, because Escape
         * there clears the selection, and a key that throws away unsaved work
         * when somebody meant to deselect something is the worst kind of
         * shortcut.
         */
        bool quit_on_escape = true;
    };

    /**
     * @brief An SDL3 window with the Vulkan flag set.
     *
     * The header keeps SDL types out of the engine interface, so only window.cpp
     * includes SDL. A window owns a live connection to the video driver, so it
     * cannot be copied or moved.
     */
    class Window {
    public:
        /**
         * @brief Called for each event the window drains, before it acts on it.
         *
         * The event arrives as a pointer to an SDL_Event, so the header stays
         * free of SDL types. An overlay that wants the raw input uses this.
         *
         * @param event A pointer to the SDL_Event.
         * @param user Whatever the caller passed to set_event_hook().
         */
        using EventHook = void (*)(const void* event, void* user);

        Window() = default;
        ~Window();

        Window(const Window&) = delete;
        Window& operator=(const Window&) = delete;
        Window(Window&&) = delete;
        Window& operator=(Window&&) = delete;

        /**
         * @brief Starts SDL video and opens the window.
         * @param desc The requested title, size, and flags.
         * @return True on success. On failure the reason is logged at critical level.
         */
        [[nodiscard]] bool create(const WindowDesc& desc);

        /// @brief Closes the window and shuts down SDL video. Safe to call twice.
        void destroy();

        /**
         * @brief Drains the event queue.
         *
         * Call this once per frame. The window close button requests a quit, and
         * so does Escape when `WindowDesc::quit_on_escape` is set.
         *
         * @return False when the user asks to quit, true to keep running.
         */
        [[nodiscard]] bool poll();

        /**
         * @brief Sends every event to a second reader before the window uses it.
         *
         * There is one hook. Setting a second one replaces the first.
         *
         * @param hook The function to call, or nullptr to stop calling one.
         * @param user Passed back to @p hook unchanged. May be null.
         */
        void set_event_hook(EventHook hook, void* user);

        /// @brief The current client size in pixels.
        /// @return Width and height. Both are 0 before create() succeeds.
        [[nodiscard]] IVec2 size() const { return size_; }

        /// @brief Whether the window is currently minimized.
        /// @return True while minimized. Skip rendering in that state.
        [[nodiscard]] bool minimized() const { return minimized_; }

        /**
         * @brief The raw SDL handle.
         *
         * The Vulkan backend needs this to create a surface. No code outside
         * src/gfx/vulkan/ should call it.
         *
         * @return The SDL window pointer, or nullptr before create() succeeds.
         */
        [[nodiscard]] SDL_Window* native() const { return window_; }

    private:
        SDL_Window* window_ = nullptr;
        EventHook event_hook_ = nullptr;
        void* event_hook_user_ = nullptr;
        IVec2 size_{ 0, 0 };
        bool minimized_ = false;
        bool running_ = true;
        /// @brief What WindowDesc::quit_on_escape asked for, kept for poll().
        bool quit_on_escape_ = true;
    };

} // namespace engine::platform
