#include "import/source_assets.h"

#include "assets/manifest.h"
#include "assets/meta.h"
#include "assets/reference.h"
#include "assets/irradiance.h"
#include "assets/texture.h"
#include "core/log.h"
#include "import/mesh.h"
#include "import/rules.h"

#include <algorithm>
#include <set>
#include <system_error>

namespace engine::import {

    namespace {

        namespace as = engine::assets;

        /// One asset that keeps the identity of its source file.
        [[nodiscard]] as::AssetRecord whole_file(const std::filesystem::path& relative, Rule rule,
                                                 Guid guid, std::uint32_t part) {
            return as::AssetRecord{ .guid = guid,
                                    .source = as::manifest_path(relative),
                                    .name = as::manifest_path(cooked_name(relative, rule, part)) };
        }

    } // namespace

    bool SourceAssets::index_one(const std::filesystem::path& relative) {
        const std::optional<Rule> rule = rule_for(relative);
        if (!rule) {
            return true;
        }

        const std::filesystem::path source = root_ / relative;

        // The sidecar is where an identity comes from, and a file without one
        // gets one written. That is what the cooker does, so both sides give a
        // file the same identity.
        as::AssetMeta meta;
        if (!as::meta_for(source, meta)) {
            ENGINE_LOG_ERROR("{}: it has no readable sidecar, so it has no identity and the "
                             "project cannot name it.",
                             source.string());
            return false;
        }

        identity_of_.emplace(as::manifest_path(relative), meta.guid);

        switch (*rule) {
        case Rule::Shader: {
            // The base form keeps the identity of the source, so a reference to
            // the shader itself still resolves. Every other variant derives one.
            const std::size_t forms = std::max<std::size_t>(meta.shader.variants.size(), 1);
            for (std::size_t at = 0; at < forms; ++at) {
                const auto part = static_cast<std::uint32_t>(at);
                const Guid guid =
                    at == 0 ? meta.guid : Guid::derive(meta.guid, as::kShaderPartKind, part);
                records_.push_back(whole_file(relative, *rule, guid, part));
            }
            return true;
        }
        case Rule::Environment: {
            // The cubemap keeps the source identity and the irradiance derives
            // one beside it. Neither is numbered, so neither uses part_record.
            records_.push_back(as::AssetRecord{
                .guid = meta.guid,
                .source = as::manifest_path(relative),
                .name = as::manifest_path(
                    std::filesystem::path(relative).concat(as::kTextureExtension)) });
            records_.push_back(as::AssetRecord{
                .guid = Guid::derive(meta.guid, as::kIrradiancePartKind, 0),
                .source = as::manifest_path(relative),
                .name = as::manifest_path(
                    std::filesystem::path(relative).concat(as::kIrradianceExtension)) });
            return true;
        }
        case Rule::Mesh:
            return gltf_parts(source, relative, meta.guid, records_);
        case Rule::Texture:
        case Rule::Brdf:
        case Rule::Document:
        case Rule::Script:
        case Rule::Font:
        case Rule::Layout:
            break;
        }

        records_.push_back(whole_file(relative, *rule, meta.guid, 0));
        return true;
    }

    bool SourceAssets::open(const std::filesystem::path& content_root) {
        root_ = content_root;
        records_.clear();
        by_source_.clear();
        by_guid_.clear();
        identity_of_.clear();
        failed_ = 0;

        std::error_code error;
        const std::filesystem::recursive_directory_iterator walk(root_, error);
        if (error) {
            ENGINE_LOG_ERROR("Could not read the source content at {}. {}", root_.string(),
                             error.message());
            return false;
        }

        // Sorted, so two opens of one tree agree on the order and so the order
        // matches a cook of the same tree.
        std::vector<std::filesystem::path> sources;
        for (const auto& item : walk) {
            if (!item.is_regular_file()) {
                continue;
            }
            const std::filesystem::path relative =
                std::filesystem::relative(item.path(), root_, error);
            if (error) {
                continue;
            }
            // A sidecar describes an asset. It is not one.
            if (relative.extension() == as::kMetaExtension) {
                continue;
            }
            if (!rule_for(relative)) {
                continue;
            }
            sources.push_back(relative);
        }
        std::ranges::sort(sources);

        // A glTF names its buffers, and a buffer is payload rather than an
        // asset. The cooker keeps them out of the tree for the same reason.
        std::set<std::filesystem::path> buffers;
        for (const std::filesystem::path& relative : sources) {
            if (rule_for(relative) != Rule::Mesh) {
                continue;
            }
            GltfReferences named;
            (void)gltf_references(root_ / relative, relative, named);
            for (const std::filesystem::path& path : named.buffers) {
                buffers.insert(path);
            }
        }

        for (const std::filesystem::path& relative : sources) {
            if (buffers.contains(relative)) {
                continue;
            }
            if (!index_one(relative)) {
                ++failed_;
            }
        }

        for (std::size_t at = 0; at < records_.size(); ++at) {
            by_source_[records_[at].source].push_back(at);
            by_guid_.emplace(records_[at].guid, at);
        }

        ENGINE_LOG_INFO("Opened the source content at {} with {} assets, {} unreadable.",
                        root_.string(), records_.size(), failed_);
        return true;
    }

    bool SourceAssets::assets_for(std::string_view source,
                                  std::vector<as::AssetRecord>& out) const {
        out.clear();
        const auto found = by_source_.find(source);
        if (found == by_source_.end()) {
            ENGINE_LOG_ERROR("{} is not in the source content at {}.", source, root_.string());
            return false;
        }
        out.reserve(found->second.size());
        for (const std::size_t at : found->second) {
            out.push_back(records_[at]);
        }
        return true;
    }

    bool SourceAssets::assets_of_kind(std::string_view suffix,
                                      std::vector<as::AssetRecord>& out) const {
        out.clear();
        for (const as::AssetRecord& record : records_) {
            if (std::string_view{ record.name }.ends_with(suffix)) {
                out.push_back(record);
            }
        }
        return true;
    }

    bool SourceAssets::read(Guid guid, std::vector<std::byte>& out) const {
        (void)out;
        ENGINE_LOG_ERROR("The source content cannot import {} yet, so nothing reads it. That is "
                         "issue #363.",
                         guid.to_text());
        return false;
    }

    bool SourceAssets::resolve(std::string_view reference, Guid& out) const {
        as::AssetReference parts;
        if (!as::parse_reference(reference, parts)) {
            return false;
        }

        // The identity of the named file comes from its sidecar, and every part
        // of it derives from that. So this needs nothing a cook wrote.
        //
        // It reads the sidecar identity rather than the first record of that
        // path, because a glTF names only derived parts and none of them is the
        // file. Taking the first record would resolve every reference to that
        // file against its first mesh.
        const auto found = identity_of_.find(as::manifest_path(parts.source));
        if (found == identity_of_.end()) {
            ENGINE_LOG_ERROR("{} names {}, which the source content does not hold.", reference,
                             parts.source.string());
            return false;
        }

        out = parts.kind.empty() ? found->second
                                 : Guid::derive(found->second, parts.kind, parts.index);
        return true;
    }

} // namespace engine::import
