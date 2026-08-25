// The front end of the watcher. It owns the debounce and the meaning of a
// change, and it asks a backend only which files are worth another look.
//
// Every backend needs the debounce, because a native event arrives while the
// file is still being written, which is the same problem polling has. So it
// lives here and a backend never repeats it.

#include "platform/watch.h"

#include <utility>

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

    DirectoryWatcher::DirectoryWatcher()
        : DirectoryWatcher(make_polling_watch_backend()) {}

    DirectoryWatcher::DirectoryWatcher(std::unique_ptr<WatchBackend> backend)
        : backend_(std::move(backend)) {
        backend_->set_interval(kDefaultInterval);
    }

    DirectoryWatcher::~DirectoryWatcher() = default;

    DirectoryWatcher::DirectoryWatcher(DirectoryWatcher&&) = default;

    DirectoryWatcher& DirectoryWatcher::operator=(DirectoryWatcher&&) = default;

    void DirectoryWatcher::set_interval(std::chrono::milliseconds interval) {
        backend_->set_interval(interval);
    }

    bool DirectoryWatcher::start(const std::filesystem::path& root) {
        root_ = root;
        known_.clear();
        pending_.clear();
        started_ = false;

        if (!backend_->start(root, known_)) {
            return false;
        }
        started_ = true;
        return true;
    }

    bool DirectoryWatcher::poll(std::vector<WatchEvent>& out) {
        out.clear();
        if (!started_) {
            return false;
        }

        std::vector<std::string> candidates;
        if (backend_->collect(candidates) != CollectResult::Collected) {
            // The backend did not look, or the tree would not read. Either way
            // nothing new is known, so nothing here moves.
            return false;
        }

        // A name the backend already gave keeps the clock it started on. The
        // state it is waiting on is read again below whatever happens here.
        for (std::string& name : candidates) {
            pending_.try_emplace(std::move(name));
        }

        const auto now = std::chrono::steady_clock::now();
        std::vector<std::string> finished;
        for (auto& [name, waiting] : pending_) {
            if (settle(name, waiting, now, out)) {
                finished.push_back(name);
            }
        }
        for (const std::string& name : finished) {
            pending_.erase(name);
        }
        return !out.empty();
    }

    bool DirectoryWatcher::settle(const std::string& name, Pending& waiting,
                                  std::chrono::steady_clock::time_point now,
                                  std::vector<WatchEvent>& out) {
        const FileState seen = read_file_state(root_ / name);

        const auto was = known_.find(name);
        const FileState before = was != known_.end() ? was->second : FileState{};
        if (seen == before) {
            // This is where a file whose write time and size came back to what
            // was already reported lands. Dropping it is what stops a change
            // that undid itself from being announced.
            return true;
        }

        if (!waiting.seen_once || waiting.state != seen) {
            // The first look at this state only starts the clock. A file still
            // being written changes again before the next look, and this is
            // where that gets caught.
            waiting.state = seen;
            waiting.first_seen = now;
            waiting.seen_once = true;
            return false;
        }
        if (now - waiting.first_seen < settle_) {
            return false;
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
        return true;
    }

} // namespace engine::platform
