#pragma once

/**
 * @file
 * @brief Logging for the whole engine.
 *
 * Every subsystem logs through the ENGINE_LOG_* macros rather than calling spdlog
 * directly. The macros record the source location, and they give one place to
 * change the backend later.
 */

#include <spdlog/spdlog.h>

/// @brief Logging setup and configuration.
namespace engine::log {

    /**
     * @brief Starts the logging system.
     *
     * Call this before any other engine subsystem. Logging before this call goes
     * to the default spdlog logger, which has no engine formatting.
     */
    void init();

    /// @brief Flushes and releases the loggers.
    void shutdown();

    /**
     * @brief Sets the minimum level that reaches the sinks.
     * @param level Messages below this level are discarded.
     */
    void set_level(spdlog::level::level_enum level);

} // namespace engine::log

/// @brief Logs at trace level. Compiled out unless SPDLOG_ACTIVE_LEVEL allows it.
#define ENGINE_LOG_TRACE(...) SPDLOG_TRACE(__VA_ARGS__)
/// @brief Logs at debug level.
#define ENGINE_LOG_DEBUG(...) SPDLOG_DEBUG(__VA_ARGS__)
/// @brief Logs at info level.
#define ENGINE_LOG_INFO(...) SPDLOG_INFO(__VA_ARGS__)
/// @brief Logs at warning level.
#define ENGINE_LOG_WARN(...) SPDLOG_WARN(__VA_ARGS__)
/// @brief Logs at error level.
#define ENGINE_LOG_ERROR(...) SPDLOG_ERROR(__VA_ARGS__)
/// @brief Logs at critical level. The sinks flush on this level.
#define ENGINE_LOG_CRITICAL(...) SPDLOG_CRITICAL(__VA_ARGS__)
