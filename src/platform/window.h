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
         * Call this once per frame. Escape and the window close button both request
         * a quit.
         *
         * @return False when the user asks to quit, true to keep running.
         */
        [[nodiscard]] bool poll();

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
        IVec2 size_{ 0, 0 };
        bool minimized_ = false;
        bool running_ = true;
    };

} // namespace engine::platform
