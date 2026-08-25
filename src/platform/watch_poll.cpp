// The polling backend. It walks the whole tree on a timer and compares what it
// sees against what the last walk saw.
//
// It reports a name only when the state moved, which keeps the candidate list
// short. The front end holds a name it has not settled yet, so a file that is
// still being written does not have to be reported on every walk.

#include "platform/watch_backend.h"

#include "core/log.h"

#include <algorithm>
#include <system_error>
#include <thread>
#include <unordered_map>

namespace engine::platform {

    namespace {

        /// Walks a tree on a timer and reports the files whose state moved.
        class PollingWatchBackend final : public WatchBackend {
        public:
            [[nodiscard]] bool start(const std::filesystem::path& root,
                                     std::unordered_map<std::string, FileState>& initial)
                override {
                initial.clear();
                root_ = root;
                seen_.clear();

                if (!walk(seen_)) {
                    return false;
                }
                initial = seen_;

                // The next collect() must walk rather than wait out an interval
                // that started here. A person who edits a file the moment the
                // program opens would otherwise wait twice as long for it.
                last_walk_ = std::chrono::steady_clock::time_point{};
                return true;
            }

            [[nodiscard]] CollectResult collect(std::vector<std::string>& out) override {
                out.clear();

                const auto now = std::chrono::steady_clock::now();
                if (last_walk_ != std::chrono::steady_clock::time_point{} &&
                    now - last_walk_ < interval_) {
                    return CollectResult::Idle;
                }
                last_walk_ = now;

                std::unordered_map<std::string, FileState> current;
                if (!walk(current)) {
                    // The root went away, or it will not read. Keep what is
                    // known, because a directory that comes back should not
                    // arrive as one candidate for every file in it.
                    return CollectResult::Failed;
                }

                for (const auto& [name, state] : current) {
                    const auto was = seen_.find(name);
                    if (was == seen_.end() || was->second != state) {
                        out.push_back(name);
                    }
                }
                for (const auto& [name, state] : seen_) {
                    if (!current.contains(name)) {
                        out.push_back(name);
                    }
                }

                seen_ = std::move(current);
                return CollectResult::Collected;
            }

            void wait(std::chrono::milliseconds timeout) override {
                if (last_walk_ == std::chrono::steady_clock::time_point{}) {
                    // The next collect() walks, so there is nothing to wait for.
                    return;
                }
                const auto due = last_walk_ + interval_;
                const auto now = std::chrono::steady_clock::now();
                if (due <= now) {
                    return;
                }
                const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(due - now);
                std::this_thread::sleep_for(std::min(timeout, left));
            }

            void set_interval(std::chrono::milliseconds interval) override {
                interval_ = interval;
            }

        private:
            /// Reads every file under the root into a map of relative path to state.
            [[nodiscard]] bool walk(std::unordered_map<std::string, FileState>& out) const;

            std::filesystem::path root_;
            /// What the last walk saw. The next walk reports what differs from it.
            std::unordered_map<std::string, FileState> seen_;
            std::chrono::steady_clock::time_point last_walk_;
            std::chrono::milliseconds interval_{ 0 };
        };

        bool PollingWatchBackend::walk(std::unordered_map<std::string, FileState>& out) const {
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

                // A directory holds no bytes to cook. Its contents arrive as
                // files of their own, so a new directory reports as the files
                // inside it.
                if (!entry->is_regular_file(error) || error) {
                    error.clear();
                    continue;
                }

                const auto write_time = entry->last_write_time(error);
                if (error) {
                    // The file went away between the directory read and this
                    // call. Leaving it out makes it absent, and the next walk
                    // either finds it again or reports the removal.
                    error.clear();
                    continue;
                }
                const auto size = entry->file_size(error);
                if (error) {
                    error.clear();
                    continue;
                }

                out.emplace(entry->path().lexically_relative(root_).generic_string(),
                            FileState{ .write_time = write_time.time_since_epoch().count(),
                                       .size = size,
                                       .exists = true });
            }
            return true;
        }

    } // namespace

    FileState read_file_state(const std::filesystem::path& path) {
        std::error_code error;
        const auto status = std::filesystem::status(path, error);
        if (error || !std::filesystem::is_regular_file(status)) {
            return FileState{};
        }

        const auto write_time = std::filesystem::last_write_time(path, error);
        if (error) {
            return FileState{};
        }
        const auto size = std::filesystem::file_size(path, error);
        if (error) {
            return FileState{};
        }
        return FileState{ .write_time = write_time.time_since_epoch().count(),
                          .size = size,
                          .exists = true };
    }

    std::unique_ptr<WatchBackend> make_polling_watch_backend() {
        return std::make_unique<PollingWatchBackend>();
    }

} // namespace engine::platform
