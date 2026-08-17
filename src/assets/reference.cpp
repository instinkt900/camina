#include "assets/reference.h"

#include "assets/material.h"
#include "assets/mesh.h"
#include "assets/texture.h"
#include "core/log.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <system_error>
#include <utility>

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

        /**
         * Reads the kind and the index out of a cooked file name.
         *
         * The cooker writes one part of a source as `<source>.<index><suffix>`,
         * and the suffix says what kind of part it is. The source path itself
         * is not read here, because the manifest already holds it.
         */
        [[nodiscard]] bool part_of_cooked_name(std::string_view cooked, AssetReference& out) {
            static constexpr std::array<std::pair<std::string_view, const char*>, 4> kSuffixes{
                { { kMeshExtension, kMeshPartKind },
                  { kTextureExtension, kTexturePartKind },
                  { kMaterialExtension, kMaterialPartKind },
                  { kPrefabExtension, kPrefabPartKind } }
            };

            for (const auto& [suffix, kind] : kSuffixes) {
                if (!cooked.ends_with(suffix)) {
                    continue;
                }
                const std::string_view stem = cooked.substr(0, cooked.size() - suffix.size());
                const std::size_t dot = stem.rfind('.');
                if (dot == std::string_view::npos) {
                    return false;
                }

                const std::string_view digits = stem.substr(dot + 1);
                std::uint32_t index = 0;
                const auto* const end = digits.data() + digits.size();
                const auto read = std::from_chars(digits.data(), end, index);
                if (digits.empty() || read.ec != std::errc{} || read.ptr != end) {
                    return false;
                }
                out.kind = kind;
                out.index = index;
                return true;
            }
            return false;
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

    std::string prefab_name(std::string_view source, std::string_view cooked) {
        // A prefab the cooker copied rather than wrote keeps the source path,
        // extension and all, and there is no scene index to read. This is
        // compared before the extension comes off, because the source path of a
        // copied prefab already ends in it.
        if (cooked == source) {
            return std::string{ source };
        }

        std::string_view stem = cooked;
        if (stem.ends_with(engine::assets::kPrefabExtension)) {
            stem.remove_suffix(std::string_view{ engine::assets::kPrefabExtension }.size());
        }

        // A glTF writes "<source>.<scene>.prefab". The index is read from the
        // path rather than counted, because the identity of the prefab is
        // derived from that same scene index. Counting would be a second
        // source of truth, and the two would disagree the moment the manifest
        // held the outputs of one source in another order.
        if (stem.size() > source.size() + 1 && stem.starts_with(source) &&
            stem[source.size()] == '.') {
            const std::string_view digits = stem.substr(source.size() + 1);
            if (!digits.empty() && std::ranges::all_of(digits, [](char c) {
                    return c >= '0' && c <= '9';
                })) {
                return digits == "0" ? std::string{ source }
                                     : std::string{ source } + "#" + std::string{ digits };
            }
        }

        // A shape nobody planned for. The cooked path is still unique, so this
        // names the prefab rather than colliding with the source path.
        return std::string{ stem };
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

            // A part of it. The cooked name says which part, because the
            // cooker writes `<source>.<index><suffix>` for one. Counting the
            // outputs instead would miss a sparse index, and those happen: a
            // glTF where only image 7 of 8 sits inside the file cooks one
            // texture and derives it at index 7.
            //
            // The name only suggests the answer. Deriving from the source
            // identity is what confirms it, so a naming rule that changes
            // gives no answer rather than a wrong one.
            for (const ManifestOutput& output : entry.outputs) {
                if (output.guid != guid) {
                    continue;
                }
                AssetReference part;
                if (!part_of_cooked_name(output.cooked, part) ||
                    Guid::derive(entry.guid, part.kind, part.index) != guid) {
                    break;
                }
                part.source = std::filesystem::path{ entry.source };
                return format_reference(part);
            }
        }
        return {};
    }

} // namespace engine::assets
