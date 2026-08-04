#include "document.h"

#include "assets/meta.h"
#include "core/log.h"

#include <nlohmann/json.hpp>

#include <fstream>

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

        /// Replaces every reference under a node with the identity it names.
        [[nodiscard]] bool resolve_in_place(nlohmann::json& node,
                                            const std::filesystem::path& content_root,
                                            std::string_view where) {
            if (node.is_object() || node.is_array()) {
                for (auto& child : node) {
                    if (!resolve_in_place(child, content_root, where)) {
                        return false;
                    }
                }
                return true;
            }
            if (!node.is_string()) {
                return true;
            }

            const auto text = node.get<std::string>();
            if (!std::string_view{ text }.starts_with(kAssetPrefix)) {
                // Every other string in the file, a name and a component type
                // among them. A GUID already written out lands here too, which
                // is what keeps a document written before this still readable.
                return true;
            }

            AssetReference reference;
            if (!parse_reference(text, reference)) {
                // It meant to be a reference and it will not read.
                // parse_reference has already said what is wrong with it.
                return false;
            }

            engine::Guid guid;
            if (!resolve(reference, content_root, where, guid)) {
                return false;
            }
            node = guid.to_text();
            return true;
        }

        /// Collects every reference under a node, whole, and reports nothing.
        void gather_references(const nlohmann::json& node, std::vector<AssetReference>& out) {
            if (node.is_object() || node.is_array()) {
                for (const auto& child : node) {
                    gather_references(child, out);
                }
                return;
            }
            if (!node.is_string()) {
                return;
            }
            AssetReference reference;
            if (parse_reference(node.get<std::string>(), reference)) {
                out.push_back(std::move(reference));
            }
        }

        /// Reads a document and gives back every reference in it.
        [[nodiscard]] std::vector<AssetReference> references_of(const nlohmann::json& document) {
            std::vector<AssetReference> out;
            gather_references(document, out);
            return out;
        }

    } // namespace

    void document_references(const std::filesystem::path& source,
                             std::vector<std::filesystem::path>& out) {
        std::ifstream file(source, std::ios::binary);
        if (!file) {
            return;
        }
        const nlohmann::json document = nlohmann::json::parse(file, nullptr, false);
        if (document.is_discarded()) {
            return;
        }
        for (const AssetReference& reference : references_of(document)) {
            out.push_back(reference.source);
        }
    }

    bool validate_references(const std::filesystem::path& source,
                             const std::filesystem::path& content_root,
                             const as::Manifest& manifest) {
        std::ifstream file(source, std::ios::binary);
        if (!file) {
            return true;
        }
        const nlohmann::json document = nlohmann::json::parse(file, nullptr, false);
        if (document.is_discarded()) {
            // The rule reports a document that will not parse. Saying it twice
            // would put the same failure in the log under two headings.
            return true;
        }

        // The references are read again rather than remembered from the cook,
        // because a document the cooker skipped was never resolved this run and
        // its references still have to hold.
        bool sound = true;
        for (const AssetReference& reference : references_of(document)) {
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
                       const std::filesystem::path& content_root) {
        nlohmann::json document;
        if (!read_json(source, document)) {
            return false;
        }
        if (!resolve_in_place(document, content_root, source.string())) {
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
