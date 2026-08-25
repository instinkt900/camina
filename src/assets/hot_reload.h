#pragma once

/**
 * @file
 * @brief Cooks a source asset that changed, and says what to load again.
 *
 * This is M4.5, and it is what makes the pipeline usable. A person edits a
 * source file and sees the result, with no restart and no cook by hand.
 *
 * The work splits in two. This half watches the source tree, runs the cooker,
 * and reads the manifest again. It names no GPU type, so it stays testable
 * with no device. The other half is `render::MeshPass::reload()`, which frees
 * what the identities here name.
 *
 * DESIGN.md section 6 keeps the cooker a separate executable, so this runs it
 * rather than linking it. No importer therefore reaches a shipping build.
 */

#include "assets/content.h"
#include "core/guid.h"
#include "platform/watch.h"

#include <filesystem>
#include <string>
#include <vector>

namespace engine::assets {

    /// @brief What hot reload needs to know before it can run.
    struct HotReloadDesc {
        /// @brief The source content tree to watch. This is what a person edits.
        std::filesystem::path source;

        /// @brief The cooker executable, which normally sits beside this program.
        std::filesystem::path cooker;

        /// @brief How the source tree is watched.
        ///
        /// Automatic asks the operating system for changes where it can. A
        /// tree on a network drive reports no events, so that one needs
        /// Polling named outright.
        platform::WatchBackendChoice watching = platform::WatchBackendChoice::Automatic;
    };

    /**
     * @brief Watches a source tree, cooks what changed, and reports what moved.
     *
     * @code
     * engine::assets::HotReload reload;
     * if (reload.start(desc, content)) {
     *     std::vector<engine::assets::AssetChange> changed;
     *     if (reload.poll(content, changed)) {
     *         // Pull the identities out for the render caches.
     *     }
     * }
     * @endcode
     */
    class HotReload {
    public:
        /**
         * @brief Checks what it needs and starts watching.
         *
         * A missing source tree or a missing cooker is not a failure of the
         * program. It means this machine cannot cook, which is what a build
         * with no source tree beside it looks like. The log then says which
         * one was missing, and the caller runs on without hot reload.
         *
         * @param desc The source tree and the cooker.
         * @return True when it will watch. False when it will not, with the
         * reason in the log.
         */
        [[nodiscard]] bool start(const HotReloadDesc& desc);

        /**
         * @brief Cooks anything that changed, and reports what to load again.
         *
         * Call this between frames. It does nothing on almost every call,
         * because the watcher walks the tree on a timer and a change has to
         * settle before it counts.
         *
         * A cook that fails changes nothing. The assets already loaded stay
         * loaded, the log says what went wrong, and the next change tries
         * again. Nothing here ends the process.
         *
         * @param content The cooked content, which this cooks into and reads
         * the manifest of again.
         * @param changed Receives every identity to load again, with what it
         * was. It is cleared first.
         * @return True when @p changed holds at least one identity.
         *
         * @warning This blocks while the cooker runs. A frame therefore takes
         * as long as the cook, which a person sees as one long frame after a
         * save.
         */
        [[nodiscard]] bool poll(Content& content, std::vector<AssetChange>& changed);

        /// @brief Whether start() found what it needed.
        /// @return True when poll() can do anything.
        [[nodiscard]] bool active() const { return active_; }

        /// @brief The watcher, so an application can set its timing.
        /// @return The watcher.
        [[nodiscard]] platform::DirectoryWatcher& watcher() { return watcher_; }

        /// @brief How many cooks have run since start().
        /// @return The count, a failed cook included.
        [[nodiscard]] std::size_t cooks() const { return cooks_; }

    private:
        /// Runs the cooker over the whole source tree, and reports its exit.
        [[nodiscard]] bool cook(const Content& content);

        platform::DirectoryWatcher watcher_;
        std::filesystem::path source_;
        std::filesystem::path cooker_;
        std::size_t cooks_ = 0;
        bool active_ = false;
    };

} // namespace engine::assets
