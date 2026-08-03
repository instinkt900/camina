#include "assets/manifest.h"

#include "core/hash.h"
#include "reflect/json.h"

#include <array>
#include <fstream>
#include <system_error>

namespace engine::assets {

    namespace {

        /// Big enough that a large file costs few reads, small enough to sit on the stack.
        constexpr std::size_t kReadBlock = 64 * 1024;

    } // namespace

    bool hash_file(const std::filesystem::path& path, std::uint64_t& out) {
        std::ifstream file(path, std::ios::binary);
        if (!file) {
            return false;
        }

        // Block by block, so the cooker does not hold a whole texture in memory
        // only to fold it into eight bytes.
        std::array<char, kReadBlock> block{};
        std::uint64_t value = kHashOffsetBasis;
        while (file) {
            file.read(block.data(), static_cast<std::streamsize>(block.size()));
            const auto read = static_cast<std::size_t>(file.gcount());
            if (read == 0) {
                break;
            }
            value = hash_bytes(
                std::as_bytes(std::span<const char>{ block.data(), read }), value);
        }

        if (file.bad()) {
            return false;
        }
        out = value;
        return true;
    }

    bool hash_inputs(const std::filesystem::path& root, const std::vector<std::string>& inputs,
                     std::uint64_t& out) {
        std::uint64_t value = kHashOffsetBasis;
        for (const std::string& input : inputs) {
            std::uint64_t one = 0;
            if (!hash_file(root / input, one)) {
                return false;
            }
            // Fold the name in as well as the bytes. Two files that hold the
            // same bytes are still two inputs, and a rename is a change.
            value = hash_text(input, value);
            const auto bytes = std::as_bytes(std::span<const std::uint64_t>{ &one, 1 });
            value = hash_bytes(bytes, value);
        }
        out = value;
        return true;
    }

    const ManifestEntry* find_by_source(const Manifest& manifest, std::string_view source) {
        for (const ManifestEntry& entry : manifest.entries) {
            if (entry.source == source) {
                return &entry;
            }
        }
        return nullptr;
    }

    const ManifestEntry* find_by_guid(const Manifest& manifest, Guid guid) {
        for (const ManifestEntry& entry : manifest.entries) {
            if (entry.guid == guid) {
                return &entry;
            }
        }
        return nullptr;
    }

    bool is_fresh(const ManifestEntry& entry, const std::filesystem::path& source_root,
                  const std::filesystem::path& cooked_root) {
        std::error_code error;
        if (!std::filesystem::is_regular_file(cooked_root / entry.cooked, error)) {
            return false;
        }

        std::uint64_t hash = 0;
        if (!hash_inputs(source_root, entry.inputs, hash)) {
            // An input that will not read counts as changed. The cooker then
            // tries again and reports the real reason.
            return false;
        }
        return hash == entry.hash;
    }

    std::string manifest_path(const std::filesystem::path& path) {
        return path.generic_string();
    }

    bool load_manifest(const std::filesystem::path& cooked_root, Manifest& out) {
        const std::filesystem::path path = cooked_root / kManifestFile;
        std::error_code error;
        if (!std::filesystem::is_regular_file(path, error)) {
            // A first cook has no manifest, and that is not a failure.
            out = Manifest{};
            return false;
        }
        return reflect::load_json(path, out);
    }

    bool save_manifest(const std::filesystem::path& cooked_root, const Manifest& manifest) {
        return reflect::save_json(cooked_root / kManifestFile, manifest);
    }

} // namespace engine::assets
