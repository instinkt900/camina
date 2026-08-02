#include "platform/window.h"

#include "core/log.h"
#include "core/profile.h"

#include <SDL3/SDL.h>

namespace engine::platform {

    Window::~Window() {
        destroy();
    }

    bool Window::create(const WindowDesc& desc) {
        if (!SDL_Init(SDL_INIT_VIDEO)) {
            ENGINE_LOG_CRITICAL("SDL_Init failed. {}", SDL_GetError());
            return false;
        }

        SDL_WindowFlags flags = SDL_WINDOW_VULKAN;
        if (desc.resizable) {
            flags |= SDL_WINDOW_RESIZABLE;
        }

        window_ = SDL_CreateWindow(desc.title, desc.width, desc.height, flags);
        if (window_ == nullptr) {
            ENGINE_LOG_CRITICAL("SDL_CreateWindow failed. {}", SDL_GetError());
            SDL_Quit();
            return false;
        }

        int width = 0;
        int height = 0;
        SDL_GetWindowSizeInPixels(window_, &width, &height);
        size_ = IVec2{ width, height };

        ENGINE_LOG_INFO("Window created at {}x{} on the {} video driver.", size_.x, size_.y,
                        SDL_GetCurrentVideoDriver());
        return true;
    }

    void Window::destroy() {
        if (window_ != nullptr) {
            SDL_DestroyWindow(window_);
            window_ = nullptr;
            SDL_Quit();
            ENGINE_LOG_INFO("Window destroyed.");
        }
    }

    bool Window::poll() {
        ENGINE_PROFILE_ZONE_N("Window::poll");

        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            switch (event.type) {
            case SDL_EVENT_QUIT:
                running_ = false;
                break;

            case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
                running_ = false;
                break;

            case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
                size_ = IVec2{ event.window.data1, event.window.data2 };
                ENGINE_LOG_DEBUG("Window resized to {}x{}.", size_.x, size_.y);
                break;

            case SDL_EVENT_WINDOW_MINIMIZED:
                minimized_ = true;
                break;

            case SDL_EVENT_WINDOW_RESTORED:
                minimized_ = false;
                break;

            case SDL_EVENT_KEY_DOWN:
                if (event.key.key == SDLK_ESCAPE) {
                    running_ = false;
                }
                break;

            default:
                break;
            }
        }

        return running_;
    }

} // namespace engine::platform
