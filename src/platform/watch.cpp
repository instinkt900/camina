// The front end of the watcher. It owns the debounce and the meaning of a
// change, and it asks a backend only which files are worth another look.
//
// Every backend needs the debounce, because a native event arrives while the
// file is still being written, which is the same problem polling has. So it
// lives here and a backend never repeats it.

#include "platform/watch.h"

#include "core/log.h"

#include <algorithm>
#include <system_error>
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
        : DirectoryWatcher(WatchBackendChoice::Automatic) {}

    DirectoryWatcher::DirectoryWatcher(WatchBackendChoice choice)
        : DirectoryWatcher(make_watch_backend(choice)) {
        // Only a watcher that asked for the best available backend may quietly
        // take a worse one. A caller that named a backend gets that backend.
        may_fall_back_ = choice == WatchBackendChoice::Automatic;
    }

    DirectoryWatcher::DirectoryWatcher(std::unique_ptr<WatchBackend> backend)
        : backend_(std::move(backend)) {
        backend_->set_interval(interval_);
    }

    DirectoryWatcher::~DirectoryWatcher() = default;

    DirectoryWatcher::DirectoryWatcher(DirectoryWatcher&&) = default;

    DirectoryWatcher& DirectoryWatcher::operator=(DirectoryWatcher&&) = default;

    void DirectoryWatcher::set_interval(std::chrono::milliseconds interval) {
        // The front end keeps this as well as handing it over, because a
        // backend swapped in by the fallback below has to be told the same.
        interval_ = interval;
        backend_->set_interval(interval);
    }

    bool DirectoryWatcher::start(const std::filesystem::path& root) {
        root_ = root;
        known_.clear();
        pending_.clear();
        started_ = false;

        if (backend_->start(root, known_)) {
            started_ = true;
            return true;
        }

        // A backend that will not start is worth saying out loud. Falling back
        // without a word hides a broken build, and it brings back the walk on
        // the machine least able to afford it.
        std::error_code error;
        if (!may_fall_back_ || !std::filesystem::is_directory(root, error) || error) {
            return false;
        }

        ENGINE_LOG_WARN("{}: the native watcher would not start, so the tree is walked on a "
                        "timer instead. Hot reload still works and it costs a walk every {} ms.",
                        root.string(), interval_.count());
        backend_ = make_polling_watch_backend();
        backend_->set_interval(interval_);
        known_.clear();
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
        const CollectResult result = backend_->collect(candidates);
        if (result == CollectResult::Failed) {
            // The tree would not read. Every candidate would look absent, so
            // reading them now would report a removal for each one.
            return false;
        }

        // A name the backend already gave keeps the clock it started on. The
        // state it is waiting on is read again below whatever happens here.
        for (std::string& name : candidates) {
            pending_.try_emplace(std::move(name));
        }
        if (result == CollectResult::Resync) {
            // The backend lost track. It can name what is there and not what
            // is gone, so every file this end knows about goes back on the
            // list. A file that still holds what was reported drops out again
            // on the first look, which is what settle() does with it.
            for (const auto& [name, state] : known_) {
                pending_.try_emplace(name);
            }
        }
        if (pending_.empty()) {
            return false;
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

    bool DirectoryWatcher::wait(std::vector<WatchEvent>& out,
                                std::chrono::milliseconds timeout) {
        out.clear();
        if (!started_) {
            return false;
        }

        const auto deadline = std::chrono::steady_clock::now() + timeout;
        for (;;) {
            if (poll(out)) {
                return true;
            }

            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline) {
                return false;
            }

            auto slice = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
            // A candidate that is waiting out the settle time has to wake this
            // loop when it is due. Nothing reaches the backend for it, because
            // the file that is settling is the file that stopped changing.
            if (const auto due = next_settle_due(now)) {
                slice = std::min(slice, *due);
            }
            backend_->wait(slice);
        }
    }

    std::optional<std::chrono::milliseconds>
    DirectoryWatcher::next_settle_due(std::chrono::steady_clock::time_point now) const {
        std::optional<std::chrono::milliseconds> soonest;
        for (const auto& [name, waiting] : pending_) {
            std::chrono::milliseconds left{ 0 };
            if (waiting.seen_once) {
                const auto due = waiting.first_seen + settle_;
                if (due > now) {
                    left = std::chrono::duration_cast<std::chrono::milliseconds>(due - now);
                }
            }
            if (!soonest || left < *soonest) {
                soonest = left;
            }
        }
        return soonest;
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
