#include "document.h"

#include "assets/meta.h"
#include "core/log.h"
#include "scene/references.h"

#include <nlohmann/json.hpp>

#include <fstream>
#include <string>
#include <string_view>
#include <utility>

namespace cooker {

    namespace {

        namespace as = engine::assets;

        using as::AssetReference;
        using as::kAssetPrefix;
        using as::parse_reference;

        /// How a cooked document is written. It is read by machine, not by hand.
        constexpr int kIndent = 2;

        /// Reads a JSON file, or reports why it could not.
        [[nodiscard]] bool read_json(const std::filesystem::path& path, nlohmann::json& out) {
            std::ifstream file(path, std::ios::binary);
            if (!file) {
                ENGINE_LOG_ERROR("{}: could not open it.", path.string());
                return false;
            }
            // Parsing with exceptions off, because the rest of the cooker
            // reports a bad file rather than throwing through it.
            out = nlohmann::json::parse(file, nullptr, false);
            if (out.is_discarded()) {
                ENGINE_LOG_ERROR("{}: it is not valid JSON.", path.string());
                return false;
            }
            return true;
        }

        /**
         * Turns one reference into the identity it names.
         *
         * The sidecar of the named file is what holds the parent identity, and
         * meta_for() writes one when the file has none. It has to. The sources
         * cook in name order, so a scene can reach a model before the model
         * rule has run, and both have to come out with the same answer.
         */
        [[nodiscard]] bool resolve(const AssetReference& reference,
                                   const std::filesystem::path& content_root,
                                   std::string_view where, engine::Guid& out) {
            const std::filesystem::path source = content_root / reference.source;

            as::AssetMeta meta;
            if (!as::meta_for(source, meta)) {
                // meta_for has already said what is wrong with the file it was
                // handed, a file that is not there included. What it cannot
                // say is which document asked for it, and that is the half a
                // person needs to fix it.
                ENGINE_LOG_ERROR("{}: it names {}, and that has no identity to give.", where,
                                 reference.source.generic_string());
                return false;
            }
            if (reference.kind.empty()) {
                out = meta.guid;
                return true;
            }
            out = engine::Guid::derive(meta.guid, reference.kind, reference.index);
            return true;
        }

        /// Replaces the reference in every field that names an asset.
        [[nodiscard]] bool resolve_in_place(nlohmann::json& document,
                                            const engine::scene::ComponentRegistry& types,
                                            const std::filesystem::path& content_root,
                                            std::string_view where) {
            bool sound = true;
            engine::scene::for_each_reference_field(
                document, types, [&](nlohmann::json& value) {
                    const auto text = value.get<std::string>();
                    if (!std::string_view{ text }.starts_with(kAssetPrefix)) {
                        // A GUID already written out. That is what keeps a
                        // document written before references existed readable,
                        // and what carries an identity the save side could not
                        // name back through unchanged.
                        return true;
                    }

                    AssetReference reference;
                    if (!parse_reference(text, reference)) {
                        // It meant to be a reference and it will not read.
                        // parse_reference has already said what is wrong.
                        sound = false;
                        return false;
                    }

                    engine::Guid guid;
                    if (!resolve(reference, content_root, where, guid)) {
                        sound = false;
                        return false;
                    }
                    value = guid.to_text();
                    return true;
                });
            return sound;
        }

        /// Collects every string under a node that reads as a reference.
        void gather_strays(const nlohmann::json& node, std::vector<std::string>& out) {
            if (node.is_object() || node.is_array()) {
                for (const auto& child : node) {
                    gather_strays(child, out);
                }
                return;
            }
            if (!node.is_string()) {
                return;
            }
            auto text = node.get<std::string>();
            if (std::string_view{ text }.starts_with(kAssetPrefix)) {
                out.push_back(std::move(text));
            }
        }

        /**
         * Reports a reference sitting where nothing will ever resolve it.
         *
         * Only a field marked `reflect::AssetRef` is resolved, so a reference
         * anywhere else reaches the cooked tree as text and the runtime reads
         * it as a GUID that will not parse. That used to work, because the
         * resolve read the text of every string, and it is the granularity
         * issue #81 took out.
         *
         * The document arrives by value, and the fields that do name an asset
         * come out of the copy first. What is left is every place a resolve
         * never reaches, whether the caller has resolved already or not.
         */
        [[nodiscard]] bool no_stray_references(nlohmann::json document,
                                               const engine::scene::ComponentRegistry& types,
                                               std::string_view where) {
            engine::scene::for_each_reference_field(document, types,
                                                    [](nlohmann::json& value) {
                                                        value = nullptr;
                                                        return true;
                                                    });

            std::vector<std::string> strays;
            gather_strays(document, strays);
            for (const std::string& text : strays) {
                ENGINE_LOG_ERROR("{}: '{}' reads as a reference, and nothing resolves it "
                                 "there. A reference goes in a field marked "
                                 "reflect::AssetRef, on a component the cooker registers.",
                                 where, text);
            }
            return strays.empty();
        }

        /// Reads a document and gives back the reference in every tagged field.
        [[nodiscard]] std::vector<AssetReference> references_of(
            nlohmann::json& document, const engine::scene::ComponentRegistry& types) {
            std::vector<AssetReference> out;
            engine::scene::for_each_reference_field(document, types,
                                                    [&](nlohmann::json& value) {
                                                        AssetReference reference;
                                                        if (parse_reference(
                                                                value.get<std::string>(),
                                                                reference)) {
                                                            out.push_back(std::move(reference));
                                                        }
                                                        return true;
                                                    });
            return out;
        }

    } // namespace

    void document_references(const std::filesystem::path& source,
                             const engine::scene::ComponentRegistry& types,
                             std::vector<std::filesystem::path>& out) {
        std::ifstream file(source, std::ios::binary);
        if (!file) {
            return;
        }
        nlohmann::json document = nlohmann::json::parse(file, nullptr, false);
        if (document.is_discarded()) {
            return;
        }
        for (const AssetReference& reference : references_of(document, types)) {
            out.push_back(reference.source);
        }
    }

    bool validate_references(const std::filesystem::path& source,
                             const std::filesystem::path& content_root,
                             const as::Manifest& manifest,
                             const engine::scene::ComponentRegistry& types) {
        std::ifstream file(source, std::ios::binary);
        if (!file) {
            return true;
        }
        nlohmann::json document = nlohmann::json::parse(file, nullptr, false);
        if (document.is_discarded()) {
            // The rule reports a document that will not parse. Saying it twice
            // would put the same failure in the log under two headings.
            return true;
        }

        // A document the freshness check skipped never reached cook_document
        // this run, so this is the only pass that reads it. A stray therefore
        // has to be caught here as well as there.
        bool sound = no_stray_references(document, types, source.string());

        // The references are read again rather than remembered from the cook,
        // because a document the cooker skipped was never resolved this run and
        // its references still have to hold.
        for (const AssetReference& reference : references_of(document, types)) {
            engine::Guid guid;
            if (!resolve(reference, content_root, source.string(), guid)) {
                sound = false;
                continue;
            }
            if (as::find_by_guid(manifest, guid) != nullptr) {
                continue;
            }
            if (reference.kind.empty()) {
                ENGINE_LOG_ERROR("{}: it names {}, and nothing cooked that file.",
                                 source.string(), reference.source.generic_string());
            } else {
                ENGINE_LOG_ERROR("{}: it names {} of {}, and that file has no such part. "
                                 "Check how many the file holds and count from zero.",
                                 source.string(),
                                 reference.kind + " " + std::to_string(reference.index),
                                 reference.source.generic_string());
            }
            sound = false;
        }
        return sound;
    }

    bool cook_document(const std::filesystem::path& source,
                       const std::filesystem::path& destination,
                       const std::filesystem::path& content_root,
                       const engine::scene::ComponentRegistry& types) {
        nlohmann::json document;
        if (!read_json(source, document)) {
            return false;
        }
        if (!resolve_in_place(document, types, content_root, source.string())) {
            return false;
        }
        // Before the write, so a document holding one produces no output. The
        // runtime would read that text as a GUID and get nothing.
        if (!no_stray_references(document, types, source.string())) {
            return false;
        }

        std::ofstream out(destination, std::ios::trunc);
        if (!out) {
            ENGINE_LOG_ERROR("{}: could not open it for writing.", destination.string());
            return false;
        }
        out << document.dump(kIndent) << '\n';
        if (!out) {
            ENGINE_LOG_ERROR("{}: the write failed part way through.", destination.string());
            return false;
        }
        return true;
    }

} // namespace cooker
