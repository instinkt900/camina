#include "import/writer.h"

#include "assets/manifest.h"
#include "core/log.h"

#include <fstream>
#include <system_error>

namespace engine::import {

    bool FileWriter::write(const std::filesystem::path& cooked, std::span<const std::byte> bytes) {
        const std::filesystem::path destination = root_ / cooked;

        std::error_code error;
        std::filesystem::create_directories(destination.parent_path(), error);

        std::ofstream file(destination, std::ios::binary | std::ios::trunc);
        if (!file) {
            ENGINE_LOG_ERROR("{}: could not open it for writing.", destination.string());
            return false;
        }
        file.write(reinterpret_cast<const char*>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()));
        if (!file) {
            ENGINE_LOG_ERROR("{}: the write failed part way through.", destination.string());
            return false;
        }
        return true;
    }

    bool MemoryWriter::write(const std::filesystem::path& cooked,
                             std::span<const std::byte> bytes) {
        files_[assets::manifest_path(cooked)].assign(bytes.begin(), bytes.end());
        return true;
    }

} // namespace engine::import
