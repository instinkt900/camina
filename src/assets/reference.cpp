#include "assets/reference.h"

#include "core/log.h"

#include <charconv>
#include <system_error>

namespace engine::assets {

    namespace {

        /**
         * Whether a reference path stays inside the content tree.
         *
         * Resolving a path that leaves it would read a file the content tree
         * does not own, and writing a sidecar would put a file next to it. A
         * cook runs on a build machine over content that arrives from
         * somewhere else, so this is a refusal and not a warning.
         *
         * A root name catches the Windows drive-relative form, `C:file`, which
         * is not absolute and does not stay where it looks like it does.
         */
        [[nodiscard]] bool inside_content_tree(const std::filesystem::path& path,
                                               std::string_view text) {
            if (path.is_absolute() || path.has_root_name() || path.has_root_directory()) {
                ENGINE_LOG_ERROR("'{}' names an absolute path. A reference names a file "
                                 "inside the content tree, written relative to its root.",
                                 text);
                return false;
            }
            for (const std::filesystem::path& part : path) {
                if (part == "..") {
                    ENGINE_LOG_ERROR("'{}' steps outside the content tree with '..'. A "
                                     "reference names a file inside it.",
                                     text);
                    return false;
                }
            }
            return true;
        }

    } // namespace

    bool parse_reference(std::string_view text, AssetReference& out) {
        if (!text.starts_with(kAssetPrefix)) {
            return false;
        }
        const std::string_view body = text.substr(kAssetPrefix.size());

        const std::size_t marker = body.find(kPartSeparator);
        const std::string_view path = body.substr(0, marker);
        if (path.empty()) {
            ENGINE_LOG_ERROR("'{}' names no file. Write asset:<path> or "
                             "asset:<path>#<kind>:<index>.",
                             text);
            return false;
        }
        if (!inside_content_tree(std::filesystem::path{ path }, text)) {
            return false;
        }

        if (marker == std::string_view::npos) {
            out = AssetReference{ .source = std::filesystem::path{ path },
                                  .kind = {},
                                  .index = 0 };
            return true;
        }

        // A part reference. Both halves have to be there, because a kind with
        // no index and an index with no kind each name nothing.
        const std::string_view part = body.substr(marker + 1);
        const std::size_t colon = part.find(':');
        if (colon == std::string_view::npos) {
            ENGINE_LOG_ERROR("'{}' names a part with no kind or no index. Write "
                             "asset:<path>#<kind>:<index>, as in asset:a.gltf#mesh:0.",
                             text);
            return false;
        }

        const std::string_view kind = part.substr(0, colon);
        const std::string_view number = part.substr(colon + 1);
        if (kind.empty() || number.empty()) {
            ENGINE_LOG_ERROR("'{}' names a part with no kind or no index. Write "
                             "asset:<path>#<kind>:<index>, as in asset:a.gltf#mesh:0.",
                             text);
            return false;
        }

        std::uint32_t index = 0;
        const auto* const end = number.data() + number.size();
        const auto read = std::from_chars(number.data(), end, index);
        if (read.ec != std::errc{} || read.ptr != end) {
            ENGINE_LOG_ERROR("'{}' has '{}' where the part index goes, and that is not a "
                             "whole number.",
                             text, number);
            return false;
        }

        out = AssetReference{ .source = std::filesystem::path{ path },
                              .kind = std::string{ kind },
                              .index = index };
        return true;
    }

    std::string format_reference(const AssetReference& reference) {
        std::string text{ kAssetPrefix };
        text += reference.source.generic_string();
        if (!reference.kind.empty()) {
            text += kPartSeparator;
            text += reference.kind;
            text += ':';
            text += std::to_string(reference.index);
        }
        return text;
    }

    std::string reference_for(const Manifest& manifest, Guid guid) {
        if (!guid.valid()) {
            return {};
        }

        for (const ManifestEntry& entry : manifest.entries) {
            bool holds = false;
            for (const ManifestOutput& output : entry.outputs) {
                if (output.guid == guid) {
                    holds = true;
                    break;
                }
            }
            if (!holds) {
                continue;
            }

            // The whole file. Its one output goes by the source's own identity.
            if (entry.guid == guid) {
                return format_reference(AssetReference{ .source = std::filesystem::path{ entry.source },
                                                        .kind = {},
                                                        .index = 0 });
            }

            // A part of it. The identity was derived from the source identity,
            // so it is derived again here until it matches. A part index is
            // never past the number of outputs, because every part is one.
            for (const char* kind : kPartKinds) {
                for (std::size_t at = 0; at < entry.outputs.size(); ++at) {
                    const auto index = static_cast<std::uint32_t>(at);
                    if (Guid::derive(entry.guid, kind, index) == guid) {
                        return format_reference(
                            AssetReference{ .source = std::filesystem::path{ entry.source },
                                            .kind = kind,
                                            .index = index });
                    }
                }
            }
        }
        return {};
    }

    std::size_t restore_references(nlohmann::json& document, const Manifest& manifest) {
        if (document.is_object() || document.is_array()) {
            std::size_t count = 0;
            for (auto& child : document) {
                count += restore_references(child, manifest);
            }
            return count;
        }
        if (!document.is_string()) {
            return 0;
        }

        Guid guid;
        if (!Guid::parse(document.get<std::string>(), guid)) {
            return 0;
        }
        std::string named = reference_for(manifest, guid);
        if (named.empty()) {
            // An identity nothing in this manifest produced. Leaving it alone
            // keeps a document readable rather than dropping what it held.
            return 0;
        }
        document = std::move(named);
        return 1;
    }

} // namespace engine::assets
