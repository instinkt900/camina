#include "import/source_assets.h"

#include "assets/manifest.h"
#include "assets/meta.h"
#include "assets/reference.h"
#include "assets/irradiance.h"
#include "assets/texture.h"
#include "core/log.h"
#include "import/cook.h"
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
        // asset_meta() rather than meta_for(), so a sidecar this index has to
        // write carries the same guess the cooker would have made. With
        // meta_for() an image the editor reached first recorded sRGB whatever
        // its name said, and a sidecar decides forever after it is written.
        as::AssetMeta meta;
        if (!asset_meta(source, meta)) {
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
        case Rule::Sound:
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

    void SourceAssets::build_manifest(const std::vector<std::filesystem::path>& sources) {
        // One entry for each source, in the order the tree walk found them, so
        // it reads like a cook of the same tree.
        manifest_ = as::Manifest{};
        manifest_.cooker = as::kCookerVersion;
        for (const std::filesystem::path& relative : sources) {
            const std::string source = as::manifest_path(relative);
            const auto found = by_source_.find(source);
            if (found == by_source_.end()) {
                continue;
            }
            as::ManifestEntry entry;
            entry.source = source;
            const auto identity = identity_of_.find(source);
            entry.guid = identity != identity_of_.end() ? identity->second : Guid{};
            for (const std::size_t at : found->second) {
                entry.outputs.push_back(
                    as::ManifestOutput{ .cooked = records_[at].name, .guid = records_[at].guid });
            }
            manifest_.entries.push_back(std::move(entry));
        }
    }

    bool SourceAssets::open(const std::filesystem::path& content_root) {
        root_ = content_root;
        records_.clear();
        by_source_.clear();
        by_guid_.clear();
        identity_of_.clear();
        failed_ = 0;
        manifest_ = as::Manifest{};

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

        build_manifest(sources);

        ENGINE_LOG_INFO("Opened the source content at {} with {} assets, {} unreadable.",
                        root_.string(), records_.size(), failed_);
        return true;
    }

    bool SourceAssets::reload(const std::vector<std::filesystem::path>& sources,
                              std::vector<as::AssetChange>& out) {
        out.clear();

        // What those sources named before the tree is walked again, so an asset
        // that goes away can be reported. Nothing can be looked up about it
        // afterwards.
        std::set<std::string> touched;
        std::map<Guid, std::string> before;
        for (const std::filesystem::path& relative : sources) {
            const std::string source = as::manifest_path(relative);
            touched.insert(source);
            const auto found = by_source_.find(source);
            if (found == by_source_.end()) {
                continue;
            }
            for (const std::size_t at : found->second) {
                before.emplace(records_[at].guid, records_[at].name);
            }
        }

        // Drop what those sources produced. The next read imports it again.
        for (const auto& [guid, name] : before) {
            cache_.erase(name);
        }
        for (const std::string& source : touched) {
            imported_.erase(source);
        }

        // The whole tree again, because a change can add an asset or take one
        // away rather than only replace its bytes.
        const std::filesystem::path root = root_;
        if (!open(root)) {
            return false;
        }

        for (const auto& [guid, name] : before) {
            const bool gone = by_guid_.find(guid) == by_guid_.end();
            out.push_back(as::AssetChange{ .guid = guid, .cooked = name, .gone = gone });
        }

        // Anything those sources name now that they did not name before, which
        // is a glTF that gained a mesh or a shader that gained a variant.
        for (const std::string& source : touched) {
            const auto found = by_source_.find(source);
            if (found == by_source_.end()) {
                continue;
            }
            for (const std::size_t at : found->second) {
                if (!before.contains(records_[at].guid)) {
                    out.push_back(as::AssetChange{
                        .guid = records_[at].guid, .cooked = records_[at].name, .gone = false });
                }
            }
        }

        ENGINE_LOG_INFO("{} source file(s) changed, so {} asset(s) load again.", sources.size(),
                        out.size());
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
        const auto record = by_guid_.find(guid);
        if (record == by_guid_.end()) {
            ENGINE_LOG_ERROR("{} is not an asset the source content at {} holds.",
                             guid.to_text(), root_.string());
            return false;
        }
        const as::AssetRecord& wanted = records_[record->second];

        // A rule produces every part of its source at once, so one glTF is
        // imported once however many of its meshes are asked for.
        if (!imported_.contains(wanted.source)) {
            imported_.insert(wanted.source);
            ++imports_;

            MemoryWriter writer;
            std::vector<as::ManifestOutput> produced;
            static const scene::ComponentRegistry kEngineOnly = engine_components();
            const scene::ComponentRegistry& types =
                components_ != nullptr ? *components_ : kEngineOnly;

            if (!import_one(root_, wanted.source, writer, types, produced)) {
                // The source stays in imported_, so a file that will not import
                // is not retried on every frame that asks for it. The editor
                // keeps running with the asset missing, the same way a missing
                // cooked asset behaves.
                ENGINE_LOG_ERROR("{} did not import, so nothing it holds can be drawn.",
                                 wanted.source);
                return false;
            }

            for (const auto& [name, bytes] : writer.files()) {
                cache_[name] = bytes;
            }
        }

        const auto found = cache_.find(wanted.name);
        if (found == cache_.end()) {
            ENGINE_LOG_ERROR("{} imported and produced no {}, so the index and the rule "
                             "disagree about what it holds.",
                             wanted.source, wanted.name);
            return false;
        }

        out = found->second;
        return true;
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
