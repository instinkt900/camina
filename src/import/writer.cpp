#include "import/writer.h"

#include "assets/manifest.h"
#include "core/log.h"

#include <fstream>
#include <system_error>
#include <utility>

namespace engine::import {

    bool FileWriter::write(const std::filesystem::path& cooked, std::span<const std::byte> bytes) {
        const std::filesystem::path destination = root_ / cooked;

        std::error_code error;
        std::filesystem::create_directories(destination.parent_path(), error);

        // Beside the destination rather than in a temporary directory, so the
        // rename in commit() stays inside one directory and therefore inside
        // one file system. A rename across file systems is a copy, and it is
        // not atomic.
        std::filesystem::path temporary = destination;
        temporary += ".cooking" + std::to_string(next_++);

        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        if (!file) {
            ENGINE_LOG_ERROR("{}: could not open it for writing.", temporary.string());
            return false;
        }
        file.write(reinterpret_cast<const char*>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()));
        if (!file) {
            ENGINE_LOG_ERROR("{}: the write failed part way through.", temporary.string());
            // Close before removing, because Windows refuses to delete a file
            // another handle holds open.
            file.close();
            std::filesystem::remove(temporary, error);
            return false;
        }
        file.close();

        staged_.push_back(Staged{ .temporary = std::move(temporary), .final = destination });
        return true;
    }

    bool FileWriter::commit() {
        bool ok = true;
        for (const Staged& entry : staged_) {
            std::error_code error;
            // rename() over an existing file replaces it on POSIX. Windows
            // refuses, so the old one goes first. That leaves a window with no
            // file at that path, which is the best a rename can do there.
            std::filesystem::rename(entry.temporary, entry.final, error);
            if (error) {
                std::filesystem::remove(entry.final, error);
                std::filesystem::rename(entry.temporary, entry.final, error);
            }
            if (error) {
                ENGINE_LOG_ERROR("{}: could not be published: {}", entry.final.string(),
                                 error.message());
                std::filesystem::remove(entry.temporary, error);
                ok = false;
            }
        }
        staged_.clear();
        next_ = 0;
        return ok;
    }

    void FileWriter::discard() {
        for (const Staged& entry : staged_) {
            std::error_code error;
            std::filesystem::remove(entry.temporary, error);
        }
        staged_.clear();
        next_ = 0;
    }

    bool MemoryWriter::write(const std::filesystem::path& cooked,
                             std::span<const std::byte> bytes) {
        staged_[assets::manifest_path(cooked)].assign(bytes.begin(), bytes.end());
        return true;
    }

    bool MemoryWriter::commit() {
        for (auto& [path, bytes] : staged_) {
            files_[path] = std::move(bytes);
        }
        staged_.clear();
        return true;
    }

    void MemoryWriter::discard() { staged_.clear(); }

} // namespace engine::import
