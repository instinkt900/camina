#include "assets/content.h"

#include "core/log.h"

#include <cstring>
#include <fstream>
#include <map>
#include <utility>

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

    namespace {

        /// What a manifest says about one identity, flattened out of the entries.
        struct Cooked {
            std::string path;       ///< The cooked file, relative to the root.
            std::uint64_t hash = 0; ///< The hash of every input the entry was built from.

            [[nodiscard]] bool operator==(const Cooked& other) const = default;
        };

        /// Turns a manifest into one record for each identity it names.
        [[nodiscard]] std::map<Guid, Cooked> by_identity(const Manifest& manifest) {
            std::map<Guid, Cooked> out;
            for (const ManifestEntry& entry : manifest.entries) {
                for (const ManifestOutput& output : entry.outputs) {
                    const auto [at, added] = out.emplace(
                        output.guid, Cooked{ .path = output.cooked, .hash = entry.hash });
                    if (!added) {
                        // The cooker should have refused the tree before it
                        // wrote this manifest. Reaching here means either the
                        // manifest was edited by hand, or the cooker check in
                        // cook_all did not run. Either way, only the first of
                        // the two is ever reloaded.
                        ENGINE_LOG_WARN("{} is the identity of {} and of {}. The cooker "
                                        "should have refused this. Only the first one "
                                        "reloads.",
                                        output.guid.to_text(), at->second.path, output.cooked);
                    }
                }
            }
            return out;
        }

    } // namespace

    bool Content::reload(std::vector<AssetChange>& changed) {
        changed.clear();

        Manifest fresh;
        if (!load_manifest(root_, fresh)) {
            ENGINE_LOG_ERROR("{}: the manifest will not read, so nothing is reloaded. The "
                             "assets already loaded stay as they are.",
                             root_.string());
            return false;
        }

        const std::map<Guid, Cooked> before = by_identity(manifest_);
        const std::map<Guid, Cooked> after = by_identity(fresh);

        // The hash covers every input of the entry, so an asset the cooker
        // skipped keeps the hash it had. Comparing it therefore names the
        // assets that were rebuilt, and nothing else. The cooked path is
        // compared as well, because a rule that starts writing somewhere else
        // leaves the hash alone.
        for (const auto& [guid, cooked] : after) {
            const auto was = before.find(guid);
            if (was == before.end() || was->second != cooked) {
                changed.push_back(
                    AssetChange{ .guid = guid, .cooked = cooked.path, .gone = false });
            }
        }
        // An identity that is gone has to be reported too. A cache holding it
        // would otherwise keep drawing an asset the content no longer has. The
        // path comes from the manifest being replaced, because that is the last
        // record there is of what the asset was.
        for (const auto& [guid, cooked] : before) {
            if (!after.contains(guid)) {
                changed.push_back(
                    AssetChange{ .guid = guid, .cooked = cooked.path, .gone = true });
            }
        }

        manifest_ = std::move(fresh);
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
        // Zero and many are two different problems, and the advice for many
        // does not work for zero. No rule writes an entry with no outputs, so
        // this reports a manifest somebody edited or a write that was cut off.
        if (entry->outputs.empty()) {
            ENGINE_LOG_ERROR("{} has no cooked files in the manifest. Cook the content tree "
                             "again.",
                             source);
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
