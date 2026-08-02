#pragma once

/**
 * @file
 * @brief Assertion macros.
 *
 * There are two levels. ENGINE_CHECK is always active and guards conditions that
 * must hold in a shipping build. ENGINE_ASSERT compiles out in a Release build and
 * guards programmer errors.
 *
 * Both log at critical level, flush the log, and then trap. The flush matters:
 * without it the message that explains the crash is still sitting in a buffer.
 */

#include "core/log.h"

#include <cstdlib>

#if defined(_MSC_VER)
/// @brief Stops in the debugger, or ends the process when none is attached.
#define ENGINE_DEBUG_BREAK() __debugbreak()
#elif defined(__clang__) || defined(__GNUC__)
/// @brief Stops in the debugger, or ends the process when none is attached.
#define ENGINE_DEBUG_BREAK() __builtin_trap()
#else
/// @brief Stops in the debugger, or ends the process when none is attached.
#define ENGINE_DEBUG_BREAK() std::abort()
#endif

/// @brief Implementation details. Nothing here is part of the engine interface.
namespace engine::detail {

    /**
     * @brief Reports a failed assertion and ends the process.
     *
     * Call this only through ENGINE_CHECK or ENGINE_ASSERT.
     *
     * @param expression The source text of the condition that failed.
     * @param file The source file of the failing check.
     * @param line The line of the failing check.
     * @param message The explanation supplied at the call site.
     */
    [[noreturn]] inline void assert_failed(const char* expression, const char* file, int line,
                                           const char* message) {
        ENGINE_LOG_CRITICAL("Assertion failed: {} at {}:{}. {}", expression, file, line, message);
        spdlog::default_logger()->flush();
        ENGINE_DEBUG_BREAK();
        std::abort();
    }

} // namespace engine::detail

/**
 * @brief Checks a condition in every build, including Release.
 *
 * Use this for conditions that must hold in a shipping build, such as a failed
 * device creation or a corrupt asset header.
 *
 * @param condition The expression that must be true.
 * @param message Text explaining what the condition protects.
 */
#define ENGINE_CHECK(condition, message)                                                \
    do {                                                                                \
        if (!(condition)) {                                                             \
            ::engine::detail::assert_failed(#condition, __FILE__, __LINE__, (message)); \
        }                                                                               \
    } while (false)

/**
 * @brief Checks a condition in every build except Release.
 *
 * Use this for programmer errors, such as an index that is out of range or a state
 * machine in an impossible state. The condition is not evaluated in a Release
 * build, so it must have no side effects.
 *
 * @param condition The expression that must be true.
 * @param message Text explaining what the condition protects.
 */
#if defined(ENGINE_ENABLE_ASSERTS)
#define ENGINE_ASSERT(condition, message) ENGINE_CHECK(condition, message)
#else
#define ENGINE_ASSERT(condition, message) ((void)0)
#endif
