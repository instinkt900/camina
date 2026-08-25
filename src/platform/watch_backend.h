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
        /// The backend lost track of the tree. @p out holds what it can name,
        /// and the caller has to check everything it already knows as well.
        Resync,
        Failed, ///< The backend looked and the tree would not read.
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
         * @warning Resync is not a failure. A backend fed by the operating
         * system loses events when its queue overflows, and a directory moved
         * out of the tree takes its files with it and reports nothing about
         * them. Both mean the caller can no longer trust what it knows.
         *
         * @param out Receives the names, relative to the root, with forward
         * slashes. It is cleared first.
         * @return What the backend did. Read @p out after Collected or Resync.
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
     * @brief Reads every file under a directory.
     *
     * A backend uses this for its first look, and an event-driven one uses it
     * again when it loses events and has to start over. A directory is not an
     * entry: it holds no bytes to cook, and its contents arrive as files of
     * their own.
     *
     * @param root The directory to read.
     * @param out Receives one entry for each file, keyed by its path relative
     * to @p root with forward slashes. It is cleared first.
     * @return True when the tree was read. False leaves @p out incomplete.
     */
    [[nodiscard]] bool walk_tree(const std::filesystem::path& root,
                                 std::unordered_map<std::string, FileState>& out);

    /// @brief Which backend a watcher should use.
    enum class WatchBackendChoice : std::uint8_t {
        /// The best backend this build and this machine can give. It asks the
        /// operating system for changes where it can, and walks where it cannot.
        Automatic,
        /// The walk, always. A network drive reports no events, so it needs this.
        Polling,
    };

    /**
     * @brief Makes the backend a choice asks for.
     *
     * @warning Automatic can still come back as the polling backend, and it
     * says so in the log when it does. A caller that must have the native one
     * cannot ask this.
     *
     * @param choice Which backend to make.
     * @return A backend that has not started yet.
     */
    [[nodiscard]] std::unique_ptr<WatchBackend> make_watch_backend(WatchBackendChoice choice);

#if defined(__linux__)
    /**
     * @brief Makes the backend the kernel feeds through inotify.
     *
     * @warning This can fail to start where the polling backend would work. A
     * watch descriptor is a per-user resource, so a large tree can exhaust
     * `max_user_watches` for every program the person is running.
     *
     * @return A backend that has not started yet.
     */
    [[nodiscard]] std::unique_ptr<WatchBackend> make_inotify_watch_backend();
#endif

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
