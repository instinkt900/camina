#include "platform/watch.h"

#include "core/log.h"

namespace engine::platform {

    const char* file_change_name(FileChange change) {
        switch (change) {
        case FileChange::Added:
            return "added";
        case FileChange::Modified:
            return "modified";
        case FileChange::Removed:
            return "removed";
        }
        return "unknown";
    }

    bool DirectoryWatcher::start(const std::filesystem::path& root) {
        root_ = root;
        known_.clear();
        pending_.clear();
        started_ = false;

        if (!walk(known_)) {
            return false;
        }

        // The next poll() must walk rather than wait out an interval that
        // started here. A person who edits a file the moment the program opens
        // would otherwise wait twice as long for it.
        last_walk_ = std::chrono::steady_clock::time_point{};
        started_ = true;
        return true;
    }

    bool DirectoryWatcher::poll(std::vector<WatchEvent>& out) {
        out.clear();
        if (!started_) {
            return false;
        }

        const auto now = std::chrono::steady_clock::now();
        if (last_walk_ != std::chrono::steady_clock::time_point{} &&
            now - last_walk_ < interval_) {
            return false;
        }
        last_walk_ = now;

        std::unordered_map<std::string, State> current;
        if (!walk(current)) {
            // The root went away, or it will not read. Report nothing and keep
            // what is known, because a directory that comes back should not
            // arrive as one removal for every file in it.
            return false;
        }

        // A name in either map may have changed, so both are walked.
        for (const auto& [name, state] : current) {
            settle(name, state, now, out);
        }

        // The names to check for removal are collected before they are
        // checked, because settle() writes to known_ and this reads it.
        std::vector<std::string> gone;
        for (const auto& [name, state] : known_) {
            if (!current.contains(name)) {
                gone.push_back(name);
            }
        }
        for (const std::string& name : gone) {
            settle(name, State{}, now, out);
        }
        return !out.empty();
    }

    void DirectoryWatcher::settle(const std::string& name, const State& seen,
                                  std::chrono::steady_clock::time_point now,
                                  std::vector<WatchEvent>& out) {
        const auto was = known_.find(name);
        const State before = was != known_.end() ? was->second : State{};
        if (seen == before) {
            // Almost every file on almost every walk lands here. It is also
            // where a file whose write time and size came back to what was
            // already reported lands, and dropping the pending entry is what
            // stops a change that undid itself from being announced.
            pending_.erase(name);
            return;
        }

        const auto waiting = pending_.find(name);
        if (waiting == pending_.end() || waiting->second.state != seen) {
            // The first walk to see this state only starts the clock. A file
            // still being written changes again before the next walk, and this
            // is where that gets caught.
            pending_[name] = Pending{ .state = seen, .first_seen = now };
            return;
        }
        if (now - waiting->second.first_seen < settle_) {
            return;
        }

        FileChange change = FileChange::Modified;
        if (!seen.exists) {
            change = FileChange::Removed;
        } else if (!before.exists) {
            change = FileChange::Added;
        }
        out.push_back(WatchEvent{ .relative = name, .change = change });

        if (seen.exists) {
            known_[name] = seen;
        } else {
            known_.erase(name);
        }
        pending_.erase(name);
    }

    bool DirectoryWatcher::walk(std::unordered_map<std::string, State>& out) const {
        std::error_code error;
        std::filesystem::recursive_directory_iterator entry{
            root_, std::filesystem::directory_options::skip_permission_denied, error
        };
        if (error) {
            ENGINE_LOG_WARN("{}: it will not read, so nothing under it is watched: {}",
                            root_.string(), error.message());
            return false;
        }

        const std::filesystem::recursive_directory_iterator end;
        for (; entry != end; entry.increment(error)) {
            if (error) {
                ENGINE_LOG_WARN("{}: the walk stopped part way through: {}", root_.string(),
                                error.message());
                return false;
            }

            // A directory holds no bytes to cook. Its contents arrive as files
            // of their own, so a new directory reports as the files inside it.
            if (!entry->is_regular_file(error) || error) {
                error.clear();
                continue;
            }

            const auto write_time = entry->last_write_time(error);
            if (error) {
                // The file went away between the directory read and this call.
                // Leaving it out makes it absent, and the next walk either
                // finds it again or reports the removal.
                error.clear();
                continue;
            }
            const auto size = entry->file_size(error);
            if (error) {
                error.clear();
                continue;
            }

            out.emplace(entry->path().lexically_relative(root_).generic_string(),
                        State{ .write_time = write_time.time_since_epoch().count(),
                               .size = size,
                               .exists = true });
        }
        return true;
    }

} // namespace engine::platform
