#pragma once

/**
 * @file
 * @brief volk, VMA, and the error helpers that the backend shares.
 *
 * This header includes Vulkan. Rule 4.1 in DESIGN.md allows that only under
 * `src/gfx/vulkan/`. Nothing above this directory may include it.
 *
 * volk loads every Vulkan entry point, so no translation unit links against the
 * loader directly. VMA takes its function pointers from volk, which is why both
 * VMA_STATIC_VULKAN_FUNCTIONS and the static loader stay off.
 */

#include "core/log.h"
#include "gfx/types.h"

#include <cstdint>

#include <volk.h>

/// @brief Stops VMA from linking Vulkan entry points directly. volk owns them.
#define VMA_STATIC_VULKAN_FUNCTIONS 0
/// @brief Lets VMA resolve the rest of its entry points from the two loader hooks.
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1
#include <vk_mem_alloc.h>

/// @brief The Vulkan backend. Nothing outside this namespace sees a Vulkan type.
namespace engine::gfx::vk {

    /**
     * @brief Maps a VkResult onto the public Result values.
     * @param result The value the driver returned.
     * @return The closest public value. Anything unmapped becomes Result::ErrorUnknown.
     */
    [[nodiscard]] Result to_result(VkResult result);

    /**
     * @brief A short name for a VkResult.
     * @param result The value to name.
     * @return A static string. The caller must not free it.
     */
    [[nodiscard]] const char* vk_result_name(VkResult result);

} // namespace engine::gfx::vk

/**
 * @brief Logs and returns when a Vulkan call fails.
 *
 * Use this for a call whose failure ends the operation. The macro evaluates
 * @p expr once, and on failure it logs the call text and returns the mapped
 * Result to the caller.
 *
 * @param expr A call that returns VkResult.
 *
 * @code
 * ENGINE_VK_TRY(vkCreateInstance(&info, nullptr, &instance));
 * @endcode
 */
#define ENGINE_VK_TRY(expr)                                                       \
    do {                                                                          \
        const VkResult engine_vk_result = (expr);                                 \
        if (engine_vk_result != VK_SUCCESS) {                                     \
            ENGINE_LOG_ERROR("{} failed with {} ({})", #expr,                     \
                             ::engine::gfx::vk::vk_result_name(engine_vk_result), \
                             static_cast<std::int32_t>(engine_vk_result));        \
            return ::engine::gfx::vk::to_result(engine_vk_result);                \
        }                                                                         \
    } while (false)
