#include "assets/content.h"

#include "core/log.h"

#include <cstring>
#include <fstream>

namespace engine::assets {

    bool Content::open(const std::filesystem::path& cooked_root) {
        root_ = cooked_root;
        manifest_ = Manifest{};
        if (!load_manifest(root_, manifest_)) {
            ENGINE_LOG_ERROR("{} holds no readable {}. Run the cooker over the source content.",
                             root_.string(), kManifestFile);
            return false;
        }
        ENGINE_LOG_INFO("Opened {} with {} cooked assets.", root_.string(),
                        manifest_.entries.size());
        return true;
    }

    const ManifestEntry* Content::find(std::string_view source) const {
        return find_by_source(manifest_, source);
    }

    bool Content::read_bytes(const ManifestOutput& output, std::vector<std::byte>& out) const {
        const std::filesystem::path path = root_ / output.cooked;
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file) {
            ENGINE_LOG_ERROR("Could not open {}.", path.string());
            return false;
        }

        const std::streamoff size = file.tellg();
        if (size < 0) {
            ENGINE_LOG_ERROR("Could not measure {}.", path.string());
            return false;
        }
        file.seekg(0);

        out.resize(static_cast<std::size_t>(size));
        if (!out.empty()) {
            // std::byte has no read overload, so go through char and copy no bytes.
            file.read(reinterpret_cast<char*>(out.data()),
                      static_cast<std::streamsize>(out.size()));
            if (!file) {
                ENGINE_LOG_ERROR("Could not read {}.", path.string());
                return false;
            }
        }
        return true;
    }

    bool Content::read_bytes(Guid guid, std::vector<std::byte>& out) const {
        const ManifestOutput* output = find_by_guid(manifest_, guid);
        if (output == nullptr) {
            ENGINE_LOG_ERROR("{} is not in the manifest. Nothing cooked that identity.",
                             guid.to_text());
            return false;
        }
        return read_bytes(*output, out);
    }

    const ManifestOutput* Content::sole_output(std::string_view source) const {
        const ManifestEntry* entry = find(source);
        if (entry == nullptr) {
            ENGINE_LOG_ERROR("{} is not in the manifest. The cooker did not make it.", source);
            return nullptr;
        }
        if (entry->outputs.size() != 1) {
            ENGINE_LOG_ERROR("{} cooked into {} files, so its path does not name one. Ask for "
                             "the part you want by its GUID.",
                             source, entry->outputs.size());
            return nullptr;
        }
        return &entry->outputs.front();
    }

    bool Content::read_bytes(std::string_view source, std::vector<std::byte>& out) const {
        const ManifestOutput* output = sole_output(source);
        return output != nullptr && read_bytes(*output, out);
    }

    bool Content::read_words(std::string_view source, std::vector<std::uint32_t>& out) const {
        const ManifestOutput* output = sole_output(source);
        if (output == nullptr) {
            return false;
        }

        std::vector<std::byte> bytes;
        if (!read_bytes(*output, bytes)) {
            return false;
        }

        constexpr std::size_t kWordSize = sizeof(std::uint32_t);
        if (bytes.empty() || bytes.size() % kWordSize != 0) {
            ENGINE_LOG_ERROR("{} is {} bytes, which is not a whole number of 32-bit words.",
                             output->cooked, bytes.size());
            return false;
        }

        // Copy rather than cast. A vector<byte> has no guarantee that its data
        // is aligned for a 32-bit read, and a cast would be undefined.
        out.resize(bytes.size() / kWordSize);
        std::memcpy(out.data(), bytes.data(), bytes.size());
        return true;
    }

} // namespace engine::assets
