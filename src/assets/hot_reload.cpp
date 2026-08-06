#include "assets/hot_reload.h"

#include "core/log.h"
#include "platform/process.h"

namespace engine::assets {

    namespace {

        /// How many changed files a log line names before it stops listing them.
        constexpr std::size_t kMostToName = 4;

        void report(const std::vector<platform::WatchEvent>& events) {
            for (std::size_t at = 0; at < events.size() && at < kMostToName; ++at) {
                ENGINE_LOG_INFO("Content {}: {}", platform::file_change_name(events[at].change),
                                events[at].relative);
            }
            if (events.size() > kMostToName) {
                ENGINE_LOG_INFO("...and {} more files.", events.size() - kMostToName);
            }
        }

    } // namespace

    bool HotReload::start(const HotReloadDesc& desc) {
        active_ = false;
        source_ = desc.source;
        cooker_ = desc.cooker;

        std::error_code error;
        if (!std::filesystem::is_directory(source_, error)) {
            ENGINE_LOG_INFO("{}: there is no source content tree here, so hot reload is off. "
                            "This is what a build away from its source tree looks like.",
                            source_.string());
            return false;
        }
        if (!std::filesystem::is_regular_file(cooker_, error)) {
            ENGINE_LOG_INFO("{}: the cooker is not here, so hot reload is off.",
                            cooker_.string());
            return false;
        }
        if (!watcher_.start(source_)) {
            return false;
        }

        ENGINE_LOG_INFO("Hot reload is watching {} ({} files).", source_.string(),
                        watcher_.size());
        active_ = true;
        return true;
    }

    bool HotReload::cook(const Content& content) {
        ++cooks_;

        // The cooker walks the whole tree and skips what the manifest already
        // has, so it is told about the tree rather than about one file. That
        // is also what picks up a dependent, because an entry recooks when any
        // of its inputs changed and the entry lists them all.
        std::vector<std::string> arguments{ "--content", source_.string(), "--out",
                                            content.root().string() };

        const platform::ProcessResult result = platform::run_process(cooker_, arguments);
        if (!result.ran) {
            ENGINE_LOG_ERROR("The cooker would not run, so nothing was reloaded.");
            return false;
        }
        if (result.exit_code != 0) {
            // The cooker already wrote which asset failed and why. Saying it
            // again here would only push that line further up the log.
            ENGINE_LOG_ERROR("The cook failed and returned {}. What is loaded stays loaded, "
                             "and the next change tries again.",
                             result.exit_code);
            return false;
        }
        return true;
    }

    bool HotReload::poll(Content& content, std::vector<AssetChange>& changed) {
        changed.clear();
        if (!active_) {
            return false;
        }

        std::vector<platform::WatchEvent> events;
        if (!watcher_.poll(events)) {
            return false;
        }
        report(events);

        if (!cook(content)) {
            return false;
        }
        if (!content.reload(changed)) {
            return false;
        }
        if (changed.empty()) {
            // The tree changed and no cooked output did. A sidecar the cooker
            // wrote itself does this, and so does a file it has no rule for
            // and copies through unchanged.
            ENGINE_LOG_INFO("The cook produced nothing new, so nothing is reloaded.");
            return false;
        }

        ENGINE_LOG_INFO("Reloading {} assets.", changed.size());
        return true;
    }

} // namespace engine::assets
