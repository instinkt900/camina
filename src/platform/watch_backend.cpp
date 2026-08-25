// The parts every watcher backend shares: reading one file, reading a whole
// tree, and picking which backend a program gets.
//
// The choice lives here rather than in the watcher, because it is the one
// place that knows which backends this build carries.

#include "platform/watch_backend.h"

#include "core/log.h"

#include <system_error>

namespace engine::platform {

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

    bool walk_tree(const std::filesystem::path& root,
                   std::unordered_map<std::string, FileState>& out) {
        out.clear();

        std::error_code error;
        std::filesystem::recursive_directory_iterator entry{
            root, std::filesystem::directory_options::skip_permission_denied, error
        };
        if (error) {
            ENGINE_LOG_WARN("{}: it will not read, so nothing under it is watched: {}",
                            root.string(), error.message());
            return false;
        }

        const std::filesystem::recursive_directory_iterator end;
        for (; entry != end; entry.increment(error)) {
            if (error) {
                ENGINE_LOG_WARN("{}: the walk stopped part way through: {}", root.string(),
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
                // Leaving it out makes it absent, and the next look either
                // finds it again or reports the removal.
                error.clear();
                continue;
            }
            const auto size = entry->file_size(error);
            if (error) {
                error.clear();
                continue;
            }

            out.emplace(entry->path().lexically_relative(root).generic_string(),
                        FileState{ .write_time = write_time.time_since_epoch().count(),
                                   .size = size,
                                   .exists = true });
        }
        return true;
    }

    std::unique_ptr<WatchBackend> make_watch_backend(WatchBackendChoice choice) {
        if (choice == WatchBackendChoice::Polling) {
            return make_polling_watch_backend();
        }
#if defined(__linux__)
        return make_inotify_watch_backend();
#else
        // No native backend on this platform yet. Issue #482 adds the Windows
        // one, and until then Automatic and Polling are the same thing here.
        return make_polling_watch_backend();
#endif
    }

} // namespace engine::platform
