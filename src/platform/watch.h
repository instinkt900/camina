#pragma once

/**
 * @file
 * @brief Reports a source file that changed under a directory.
 *
 * M4.5 needs this so a person edits an asset and sees the result without a
 * restart. The watcher says what moved. The caller decides what to do about it.
 *
 * This walks the tree on a timer rather than asking the operating system for
 * events. One implementation then serves both platforms, and a test drives it
 * with no event plumbing. The cost is the walk, which suits a content tree of
 * the size `sandbox/` carries today. Issue #57 holds the reasons to put a
 * native backend behind this interface, and the traps that come with one.
 */

#include <chrono>
#include <cstdint>
#include <filesystem>
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
     * loses one.
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
         * Call this as often as you like. The walk happens only when the poll
         * interval has passed, so calling it once a frame costs almost nothing
         * on the frames between walks.
         *
         * @param out Receives the changes. It is cleared first.
         * @return True when @p out holds at least one change.
         */
        [[nodiscard]] bool poll(std::vector<WatchEvent>& out);

        /**
         * @brief Sets how often the tree is walked.
         * @param interval The wait between two walks. Zero walks on every poll().
         */
        void set_interval(std::chrono::milliseconds interval) { interval_ = interval; }

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
        /// What one walk saw of one file. An absent file carries exists false.
        struct State {
            std::int64_t write_time = 0;
            std::uintmax_t size = 0;
            bool exists = false;

            [[nodiscard]] bool operator==(const State& other) const = default;
        };

        /// A state seen once, waiting to be seen again and to settle.
        struct Pending {
            State state;
            std::chrono::steady_clock::time_point first_seen;
        };

        /// Reads every file under the root into a map of relative path to state.
        [[nodiscard]] bool walk(std::unordered_map<std::string, State>& out) const;

        /// Moves one file toward a reported change, and reports it once it holds.
        void settle(const std::string& name, const State& seen,
                    std::chrono::steady_clock::time_point now, std::vector<WatchEvent>& out);

        std::filesystem::path root_;
        /// The last state reported for each file. A file with no entry is absent.
        std::unordered_map<std::string, State> known_;
        /// The states seen since, which have not settled yet.
        std::unordered_map<std::string, Pending> pending_;
        std::chrono::steady_clock::time_point last_walk_;
        std::chrono::milliseconds interval_ = kDefaultInterval;
        std::chrono::milliseconds settle_ = kDefaultSettle;
        bool started_ = false;
    };

} // namespace engine::platform
