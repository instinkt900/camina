#pragma once

/**
 * @file
 * @brief The seam a file watcher finds its changes through.
 *
 * `DirectoryWatcher` decides what a change means. A backend decides only which
 * files are worth another look. Those are two different jobs: the first is the
 * same on every platform, and the second is where inotify differs from
 * ReadDirectoryChangesW and from a walk on a timer.
 *
 * A backend never reports a change. It names a file, and the front end reads
 * the file and applies the debounce. So a backend that over-reports costs a
 * little work and cannot produce a wrong event. Issue #57 holds the native
 * backends this seam exists for.
 */

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace engine::platform {

    /// @brief What one look at one file saw. An absent file carries exists false.
    struct FileState {
        std::int64_t write_time = 0; ///< The write time, in clock ticks since the epoch.
        std::uintmax_t size = 0;     ///< The size in bytes.
        bool exists = false;         ///< False when the file was not there.

        /// @brief Compares every field.
        /// @param other The state to compare against.
        /// @return True when the two states are the same.
        [[nodiscard]] bool operator==(const FileState& other) const = default;
    };

    /**
     * @brief Reads the state of one file.
     *
     * A file that is not there, and a file that will not read, both come back
     * as an absent state. The caller cannot act on the difference.
     *
     * @param path The file to read.
     * @return What the file looks like now.
     */
    [[nodiscard]] FileState read_file_state(const std::filesystem::path& path);

    /// @brief What one call to WatchBackend::collect() did.
    enum class CollectResult : std::uint8_t {
        Collected, ///< The backend looked. @p out holds everything it found, which may be nothing.
        Idle,      ///< The backend did not look this time, so nothing is known.
        Failed,    ///< The backend looked and the tree would not read.
    };

    /**
     * @brief Names the files that may have changed under a directory.
     *
     * @warning A name this reports is a candidate and not a change. The file
     * may hold what the caller already knows about, and it may not be there at
     * all. The caller reads it and decides.
     */
    class WatchBackend {
    public:
        WatchBackend() = default;
        virtual ~WatchBackend() = default;

        WatchBackend(const WatchBackend&) = delete;
        WatchBackend& operator=(const WatchBackend&) = delete;
        WatchBackend(WatchBackend&&) = delete;
        WatchBackend& operator=(WatchBackend&&) = delete;

        /**
         * @brief Takes up the given root and lists what is already under it.
         *
         * The files in @p initial are the starting point, so nothing under the
         * root reports as new. Calling this again starts over on the new root.
         *
         * @param root The directory to watch. It must exist.
         * @param initial Receives every file under @p root, keyed by its path
         * relative to @p root with forward slashes. It is cleared first.
         * @return True when the directory was there and it was read.
         */
        [[nodiscard]] virtual bool start(const std::filesystem::path& root,
                                         std::unordered_map<std::string, FileState>& initial) = 0;

        /**
         * @brief Names the files that may have changed since the last call.
         *
         * @param out Receives the names, relative to the root, with forward
         * slashes. It is cleared first.
         * @return What the backend did. Read @p out only after Collected.
         */
        [[nodiscard]] virtual CollectResult collect(std::vector<std::string>& out) = 0;

        /**
         * @brief Waits until the backend may have something new.
         *
         * A backend that looks on a timer waits out the rest of its interval.
         * A backend the operating system feeds waits on its handle. Either one
         * may come back with nothing, so the caller collects and checks.
         *
         * @warning This blocks the calling thread. Nothing on a frame calls it.
         *
         * @param timeout The longest to wait. Zero returns at once.
         */
        virtual void wait(std::chrono::milliseconds timeout) = 0;

        /**
         * @brief Sets how often the backend looks for changes.
         *
         * A backend the operating system feeds is free to ignore this.
         *
         * @param interval The wait between two looks. Zero looks on every collect().
         */
        virtual void set_interval(std::chrono::milliseconds interval) = 0;
    };

    /**
     * @brief Makes the backend that walks the tree on a timer.
     *
     * This one serves both platforms and needs nothing from the operating
     * system, so it is the fallback wherever a native backend cannot work. A
     * network drive is the case that needs it.
     *
     * @return A backend that has not started yet.
     */
    [[nodiscard]] std::unique_ptr<WatchBackend> make_polling_watch_backend();

} // namespace engine::platform
