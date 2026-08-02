#pragma once

/**
 * @file
 * @brief Plain data types shared by the whole gfx interface.
 *
 * Rule 4.2 in DESIGN.md keeps this header C-compatible. There is no
 * `std::string`, no `std::vector`, no virtual function, and no exception. The
 * cost today is nothing. The gain later is that the plugin ABI becomes a header
 * rename instead of a rewrite.
 */

#include <cstdint>

/// @brief The public render interface. The backend behind it stays hidden.
namespace engine::gfx {

    /// @brief The outcome of a gfx call.
    enum class Result : std::uint32_t {
        Success = 0,      ///< The call did what it says.
        OutOfDate,        ///< The swapchain no longer matches the window. Resize, then retry.
        ErrorInit,        ///< The loader or the instance failed to start.
        ErrorNoDevice,    ///< No physical device meets the requirements.
        ErrorSurface,     ///< The window surface failed, or the driver lost it.
        ErrorDeviceLost,  ///< The driver reset the device or removed it.
        ErrorOutOfMemory, ///< Host memory or device memory ran out.
        ErrorUnknown,     ///< The backend failed and no other value fits.
    };

    /**
     * @brief A short name for a Result.
     * @param result The value to name.
     * @return A static string. The caller must not free it.
     */
    [[nodiscard]] const char* result_name(Result result);

    /**
     * @brief Whether a Result reports success.
     * @param result The value to test.
     * @return True only for Result::Success. Result::OutOfDate is not a success.
     */
    [[nodiscard]] constexpr bool succeeded(Result result) {
        return result == Result::Success;
    }

    /// @brief A width and a height in pixels.
    struct Extent2D {
        std::uint32_t width = 0;  ///< Width in pixels.
        std::uint32_t height = 0; ///< Height in pixels.
    };

    /**
     * @brief A color in the linear working space, with straight alpha.
     *
     * DESIGN.md section 3 sets linear as the working space. The backend converts
     * to sRGB at the final write, so do not pre-convert a value you put here.
     */
    struct ColorRGBA {
        float r = 0.0F; ///< Red, linear.
        float g = 0.0F; ///< Green, linear.
        float b = 0.0F; ///< Blue, linear.
        float a = 1.0F; ///< Alpha, straight.
    };

    /**
     * @brief How many frames the device records before it waits for the oldest.
     *
     * Two lets the CPU record frame N+1 while the GPU runs frame N. Three adds
     * latency for little gain at this stage.
     */
    inline constexpr std::uint32_t kFramesInFlight = 2;

    /// @brief Settings for create_device().
    struct DeviceDesc {
        /// @brief The `SDL_Window` to draw into, as an opaque pointer. Required.
        void* window = nullptr;
        /// @brief The application name reported to the driver.
        const char* app_name = "camina";
        /// @brief Whether to turn on the Vulkan validation layer and the debug messenger.
        bool enable_validation = false;
        /// @brief Whether to wait for vertical blank. False selects the lowest latency mode.
        bool vsync = true;
    };

} // namespace engine::gfx
