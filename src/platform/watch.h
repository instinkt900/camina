#pragma once

/**
 * @file
 * @brief Reports a source file that changed under a directory.
 *
 * M4.5 needs this so a person edits an asset and sees the result without a
 * restart. The watcher says what moved. The caller decides what to do about it.
 *
 * The watcher finds its changes through a `WatchBackend`. The one backend
 * today walks the tree on a timer, so one implementation serves both platforms
 * and a test drives it with no event plumbing. The cost is the walk, which
 * suits a content tree of the size `sandbox/` carries today. Issue #57 holds
 * the native backends that go behind the same seam, and the traps that come
 * with them.
 */

#include "platform/watch_backend.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace engine::platform {

    /// @brief What happened to one file.
    enum class FileChange : std::uint8_t {
        Added,    ///< The file was not there on the last walk.
        Modified, ///< The file is there and its write time or its size moved.
        Removed,  ///< The file was there on the last walk and it is gone.
    };

    /**
     * @brief Turns a change into text for a log line.
     * @param change The change to name.
     * @return A static string.
     */
    [[nodiscard]] const char* file_change_name(FileChange change);

    /// @brief One file that changed under the watched directory.
    struct WatchEvent {
        /// @brief The path, relative to the watched root, with forward slashes.
        std::string relative;
        /// @brief What happened to it.
        FileChange change = FileChange::Modified;
    };

    /**
     * @brief Watches a directory tree and reports the files that change.
     *
     * A change has to hold still before it is reported. An editor that saves
     * by writing a temporary file and renaming it over the original shows up
     * as several changes in a few milliseconds, and a large file is readable
     * before it is complete. Reporting either one early gives the caller a
     * half-written file.
     *
     * @warning The walk reads the write time and the size. Two writes inside
     * one write-time tick that leave the size the same look like no change.
     * The next real change reports both, so this delays a reload and never
     * loses one. Windows updates the write time coarsely enough for this to
     * be reachable, and a tool that rewrites many files at once, such as a
     * branch checkout, is what reaches it. That is a limit of the polling
     * backend rather than of this class. Issue #57 removes it by asking the
     * operating system for the change instead.
     *
     * @code
     * engine::platform::DirectoryWatcher watcher;
     * if (watcher.start(content)) {
     *     std::vector<engine::platform::WatchEvent> changes;
     *     if (watcher.poll(changes)) {
     *         // React to changes.
     *     }
     * }
     * @endcode
     */
    class DirectoryWatcher {
    public:
        /// @brief How often the tree is walked, when nothing else is set.
        static constexpr std::chrono::milliseconds kDefaultInterval{ 250 };

        /// @brief How long a new state must hold, when nothing else is set.
        static constexpr std::chrono::milliseconds kDefaultSettle{ 250 };

        /// @brief Makes a watcher over the polling backend.
        DirectoryWatcher();

        /**
         * @brief Makes a watcher over a backend the caller chose.
         * @param backend Where the candidate names come from. It must not be null.
         */
        explicit DirectoryWatcher(std::unique_ptr<WatchBackend> backend);

        /// @brief Drops the backend.
        ~DirectoryWatcher();

        DirectoryWatcher(const DirectoryWatcher&) = delete;
        DirectoryWatcher& operator=(const DirectoryWatcher&) = delete;
        /// @brief Takes the backend and the state of another watcher.
        /// @param other The watcher to take from.
        DirectoryWatcher(DirectoryWatcher&& other);

        /// @brief Takes the backend and the state of another watcher.
        /// @param other The watcher to take from.
        /// @return This watcher.
        DirectoryWatcher& operator=(DirectoryWatcher&& other);

        /**
         * @brief Takes the first snapshot of a directory tree.
         *
         * The files already there are the starting point, so the first poll()
         * reports nothing. Calling this again on a live watcher starts over on
         * the new root.
         *
         * @param root The directory to watch. It must exist.
         * @return True when the directory was there and it was read.
         */
        [[nodiscard]] bool start(const std::filesystem::path& root);

        /**
         * @brief Reports every file that changed and then settled.
         *
         * Call this as often as you like. The backend decides how often it
         * looks, so calling this once a frame costs almost nothing on the
         * frames between two looks.
         *
         * @param out Receives the changes. It is cleared first.
         * @return True when @p out holds at least one change.
         */
        [[nodiscard]] bool poll(std::vector<WatchEvent>& out);

        /**
         * @brief Sets how often the backend looks for changes.
         *
         * A backend the operating system feeds is free to ignore this.
         *
         * @param interval The wait between two looks. Zero looks on every poll().
         */
        void set_interval(std::chrono::milliseconds interval);

        /**
         * @brief Sets how long a new state must hold before it is reported.
         *
         * A change is never reported on the walk that first sees it, whatever
         * this is set to. Zero therefore means "report it on the next walk"
         * and not "report it at once".
         *
         * @param settle The wait.
         */
        void set_settle(std::chrono::milliseconds settle) { settle_ = settle; }

        /// @brief The directory this watches.
        /// @return The root, or an empty path before start().
        [[nodiscard]] const std::filesystem::path& root() const { return root_; }

        /// @brief How many files the watcher is tracking.
        /// @return The count, which is what the last settled walk saw.
        [[nodiscard]] std::size_t size() const { return known_.size(); }

    private:
        /// A candidate the backend named, waiting to hold still.
        struct Pending {
            FileState state;                                  ///< The state the last look saw.
            std::chrono::steady_clock::time_point first_seen; ///< When that state arrived.
            bool seen_once = false;                           ///< False until the first look.
        };

        /// Moves one candidate toward a reported change, and reports it once it holds.
        /// Returns true when the candidate is finished with, either way.
        [[nodiscard]] bool settle(const std::string& name, Pending& waiting,
                                  std::chrono::steady_clock::time_point now,
                                  std::vector<WatchEvent>& out);

        std::unique_ptr<WatchBackend> backend_;
        std::filesystem::path root_;
        /// The last state reported for each file. A file with no entry is absent.
        std::unordered_map<std::string, FileState> known_;
        /// The candidates the backend named, which have not settled yet.
        std::unordered_map<std::string, Pending> pending_;
        std::chrono::milliseconds settle_ = kDefaultSettle;
        bool started_ = false;
    };

} // namespace engine::platform
