// The Linux backend. The kernel reports each change, so nothing here walks the
// tree on a timer and a save costs no work at all on the frames between saves.
//
// Four things about inotify decide the shape of this file.
//
// It does not recurse. Each directory needs a watch descriptor of its own, and
// a directory that appears during a session needs one added while the program
// runs. A tree that grows a folder stops reporting otherwise.
//
// It names an event by directory and entry, not by path. So each watch
// descriptor carries the directory path it stands for, relative to the root.
//
// It reports a save by rename as IN_MOVED_TO rather than IN_MODIFY. Several
// editors save that way, which is why the mask below is long.
//
// It can lose track of the tree, in two ways a walk never does. Its queue can
// overflow, and a directory moved out of the tree takes its files with it and
// reports nothing about any of them. Both answer CollectResult::Resync, which
// tells the front end to check everything it already knows.

#include "platform/watch_backend.h"

#include "core/log.h"

#include <poll.h>
#include <sys/inotify.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstring>
#include <string>
#include <system_error>
#include <unordered_map>
#include <vector>

namespace engine::platform {

    namespace {

        /// Everything that can mean "a file under this directory is different".
        ///
        /// IN_CLOSE_WRITE and IN_MODIFY both arrive for an ordinary save, and
        /// the debounce in the front end is what turns the pair into one
        /// change. IN_ATTRIB is here because a write time set by hand, which
        /// is what a build tool does, moves nothing else.
        constexpr std::uint32_t kMask = IN_CREATE | IN_MODIFY | IN_CLOSE_WRITE | IN_MOVED_FROM |
                                        IN_MOVED_TO | IN_DELETE | IN_ATTRIB | IN_DELETE_SELF |
                                        IN_MOVE_SELF | IN_EXCL_UNLINK;

        /// One read of the event queue. inotify refuses a buffer that cannot
        /// hold one event, and a name can be as long as a path component.
        constexpr std::size_t kReadBytes = 64 * 1024;

        /// Reports what the kernel said, in a form a log line can carry.
        std::string errno_text() {
            return std::error_code{ errno, std::generic_category() }.message();
        }

        /// Watches a tree through inotify.
        class InotifyWatchBackend final : public WatchBackend {
        public:
            ~InotifyWatchBackend() override { close_queue(); }

            [[nodiscard]] bool start(const std::filesystem::path& root,
                                     std::unordered_map<std::string, FileState>& initial)
                override {
                close_queue();
                root_ = root;

                std::error_code error;
                if (!std::filesystem::is_directory(root_, error) || error) {
                    ENGINE_LOG_WARN("{}: it is not a directory, so nothing under it is watched",
                                    root_.string());
                    return false;
                }

                queue_ = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
                if (queue_ < 0) {
                    ENGINE_LOG_WARN("inotify will not start: {}", errno_text());
                    return false;
                }

                // The watches go on before the walk. A file written between the
                // two is then reported by an event, where the other order would
                // miss it: the walk would not see it and no watch existed yet.
                if (!watch_tree(root_)) {
                    close_queue();
                    return false;
                }
                if (!walk_tree(root_, initial)) {
                    close_queue();
                    return false;
                }
                return true;
            }

            [[nodiscard]] CollectResult collect(std::vector<std::string>& out) override {
                out.clear();
                if (queue_ < 0) {
                    return CollectResult::Failed;
                }

                bool lost_track = false;
                for (;;) {
                    alignas(inotify_event) std::array<char, kReadBytes> buffer{};
                    const ssize_t got = ::read(queue_, buffer.data(), buffer.size());
                    if (got <= 0) {
                        if (got < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                            ENGINE_LOG_WARN("the inotify queue will not read: {}", errno_text());
                            return CollectResult::Failed;
                        }
                        break;
                    }
                    read_events(buffer.data(), static_cast<std::size_t>(got), out, lost_track);
                }

                if (lost_track) {
                    // Whatever went unseen, a file that is there now is worth
                    // another look. The names that are gone are the front end's
                    // to find, because it is the half that knows what was there.
                    offer_whole_tree(out);
                    return CollectResult::Resync;
                }
                return CollectResult::Collected;
            }

            void wait(std::chrono::milliseconds timeout) override {
                if (queue_ < 0) {
                    return;
                }
                pollfd waiting{ .fd = queue_, .events = POLLIN, .revents = 0 };
                ::poll(&waiting, 1, static_cast<int>(timeout.count()));
            }

            void set_interval(std::chrono::milliseconds /*interval*/) override {
                // The kernel decides when this backend has something to say.
            }

        private:
            /// Puts a watch on @p directory and on every directory under it.
            [[nodiscard]] bool watch_tree(const std::filesystem::path& directory);

            /// Puts a watch on one directory and records what it stands for.
            [[nodiscard]] bool watch_one(const std::filesystem::path& directory);

            /// Turns one read of the queue into candidate names.
            void read_events(const char* bytes, std::size_t size, std::vector<std::string>& out,
                             bool& lost_track);

            /// Handles one event, which may add watches or drop them.
            void read_one(const inotify_event& event, const std::string& name,
                          std::vector<std::string>& out, bool& lost_track);

            /// Offers every file under the root as a candidate.
            void offer_whole_tree(std::vector<std::string>& out);

            void close_queue() {
                if (queue_ >= 0) {
                    ::close(queue_);
                    queue_ = -1;
                }
                directories_.clear();
            }

            std::filesystem::path root_;
            int queue_ = -1;
            /// Each watch descriptor, and the directory it stands for relative
            /// to the root. The root itself carries an empty string.
            std::unordered_map<int, std::string> directories_;
        };

        bool InotifyWatchBackend::watch_one(const std::filesystem::path& directory) {
            const int watch = inotify_add_watch(queue_, directory.c_str(), kMask);
            if (watch < 0) {
                // A dropped watch is a directory that reports nothing, and
                // nothing else would ever say so. ENOSPC is the common one: a
                // watch descriptor is a per-user resource, so a large tree can
                // exhaust it for every program the person is running.
                ENGINE_LOG_WARN("{}: it will not be watched: {}", directory.string(),
                                errno_text());
                return false;
            }
            // The root against itself gives ".", not an empty path. Left as it
            // is, every name under the root comes out as "./a.txt" and matches
            // nothing the manifest or the front end holds.
            std::string relative = directory.lexically_relative(root_).generic_string();
            if (relative == ".") {
                relative.clear();
            }
            directories_[watch] = std::move(relative);
            return true;
        }

        bool InotifyWatchBackend::watch_tree(const std::filesystem::path& directory) {
            if (!watch_one(directory)) {
                return false;
            }

            std::error_code error;
            std::filesystem::recursive_directory_iterator entry{
                directory, std::filesystem::directory_options::skip_permission_denied, error
            };
            if (error) {
                ENGINE_LOG_WARN("{}: it will not read, so nothing under it is watched: {}",
                                directory.string(), error.message());
                return false;
            }

            const std::filesystem::recursive_directory_iterator end;
            for (; entry != end; entry.increment(error)) {
                if (error) {
                    ENGINE_LOG_WARN("{}: the walk stopped part way through: {}",
                                    directory.string(), error.message());
                    return false;
                }
                if (!entry->is_directory(error) || error) {
                    error.clear();
                    continue;
                }
                if (!watch_one(entry->path())) {
                    return false;
                }
            }
            return true;
        }

        void InotifyWatchBackend::read_events(const char* bytes, std::size_t size,
                                              std::vector<std::string>& out, bool& lost_track) {
            std::size_t at = 0;
            while (at + sizeof(inotify_event) <= size) {
                // The kernel packs events end to end, each one followed by its
                // name. Copying the header out is what keeps this defined: the
                // buffer is bytes, and the next event can sit at any offset.
                inotify_event event{};
                std::memcpy(&event, bytes + at, sizeof(event));

                const std::size_t whole = sizeof(inotify_event) + event.len;
                if (at + whole > size) {
                    break;
                }

                std::string name;
                if (event.len > 0) {
                    // event.len counts the padding after the name, so the text
                    // ends at the first zero rather than at the end.
                    const char* text = bytes + at + sizeof(inotify_event);
                    name.assign(text, ::strnlen(text, event.len));
                }
                read_one(event, name, out, lost_track);

                at += whole;
            }
        }

        void InotifyWatchBackend::read_one(const inotify_event& event, const std::string& name,
                                           std::vector<std::string>& out, bool& lost_track) {
            if ((event.mask & IN_Q_OVERFLOW) != 0) {
                lost_track = true;
                return;
            }

            const auto directory = directories_.find(event.wd);
            if (directory == directories_.end()) {
                return;
            }

            if ((event.mask & IN_IGNORED) != 0) {
                // The kernel dropped this watch, which is what a deleted or
                // moved directory looks like from here.
                directories_.erase(event.wd);
                return;
            }

            const std::string& parent = directory->second;
            if (name.empty()) {
                // An event about the watched directory itself rather than
                // about something in it. The root going away is the one that
                // matters, and IN_IGNORED above has already dropped the watch.
                return;
            }
            const std::string relative = parent.empty() ? name : parent + "/" + name;

            if ((event.mask & IN_ISDIR) == 0) {
                out.push_back(relative);
                return;
            }

            if ((event.mask & (IN_CREATE | IN_MOVED_TO)) != 0) {
                // A directory that appeared needs a watch of its own, and the
                // files already inside it arrived before that watch existed.
                // So they are offered as candidates rather than waited for.
                const std::filesystem::path added = root_ / relative;
                if (!watch_tree(added)) {
                    return;
                }
                std::unordered_map<std::string, FileState> inside;
                if (walk_tree(added, inside)) {
                    for (const auto& [inner, state] : inside) {
                        out.push_back(relative + "/" + inner);
                    }
                }
                return;
            }

            if ((event.mask & (IN_DELETE | IN_MOVED_FROM)) != 0) {
                // A directory that went away takes its files with it, and a
                // move reports nothing at all about them. Neither this half
                // nor the kernel can name them, so the front end has to look
                // through what it already knows.
                lost_track = true;
            }
        }

        void InotifyWatchBackend::offer_whole_tree(std::vector<std::string>& out) {
            std::unordered_map<std::string, FileState> everything;
            if (!walk_tree(root_, everything)) {
                return;
            }
            for (const auto& [name, state] : everything) {
                out.push_back(name);
            }
        }

    } // namespace

    std::unique_ptr<WatchBackend> make_inotify_watch_backend() {
        return std::make_unique<InotifyWatchBackend>();
    }

} // namespace engine::platform
