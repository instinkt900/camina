#include "core/log.h"

#include <spdlog/sinks/stdout_color_sinks.h>

#include <memory>

namespace engine::log {

    void init() {
        auto console = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
        console->set_pattern("[%H:%M:%S.%e] [%^%l%$] [%t] %v (%s:%#)");

        auto logger = std::make_shared<spdlog::logger>("engine", console);
        logger->set_level(spdlog::level::trace);
        logger->flush_on(spdlog::level::warn);

        spdlog::set_default_logger(logger);
    }

    void shutdown() {
        spdlog::shutdown();
    }

    void set_level(spdlog::level::level_enum level) {
        spdlog::set_level(level);
    }

} // namespace engine::log
