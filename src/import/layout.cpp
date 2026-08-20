#include "import/layout.h"

#include "assets/manifest.h"
#include "assets/meta.h"
#include "core/log.h"

#include <nlohmann/json.hpp>

#include <fstream>
#include <string>

namespace engine::import {

    namespace {

        namespace as = engine::assets;

        /// What a moth_ui image entity calls the image it draws.
        constexpr const char* kImagePathKey = "imagePath";

        /// Reads a layout, or says why it could not.
        [[nodiscard]] bool read_layout(const std::filesystem::path& source,
                                       nlohmann::json& out) {
            std::ifstream file(source);
            if (!file) {
                ENGINE_LOG_ERROR("{}: the layout could not be opened.", source.string());
                return false;
            }
            try {
                file >> out;
            } catch (const nlohmann::json::exception& error) {
                ENGINE_LOG_ERROR("{}: the layout is not readable JSON. {}", source.string(),
                                 error.what());
                return false;
            }
            return true;
        }

        /**
         * Turns the path a layout stored into one relative to the content root.
         *
         * moth_ui stores an image relative to the directory holding the layout,
         * so `ui/main.mothui` naming `panel.png` means `ui/panel.png`. The
         * manifest is keyed on the second form.
         *
         * An empty result means the path climbs out of the content tree, which
         * names no asset the cooker will ever write.
         */
        [[nodiscard]] std::filesystem::path source_of(const std::filesystem::path& relative,
                                                      const std::string& stored) {
            if (stored.empty()) {
                return {};
            }
            const std::filesystem::path joined =
                (relative.parent_path() / stored).lexically_normal();
            if (!joined.empty() && *joined.begin() == std::filesystem::path{ ".." }) {
                return {};
            }
            return joined;
        }

        /**
         * Calls @p visit for every image entity in the tree.
         *
         * A layout nests: a group holds children and so does the layout itself.
         * So this walks everything rather than the top level, and it keys on
         * the field rather than on the entity type. A field named `imagePath`
         * is an image path whatever entity carries it, and a moth_ui release
         * that adds an entity kind with one keeps working.
         */
        template <typename Visit>
        void for_each_image(nlohmann::json& node, const Visit& visit) {
            if (node.is_array()) {
                for (nlohmann::json& child : node) {
                    for_each_image(child, visit);
                }
                return;
            }
            if (!node.is_object()) {
                return;
            }
            if (const auto found = node.find(kImagePathKey);
                found != node.end() && found->is_string()) {
                visit(*found);
            }
            for (const auto& [key, child] : node.items()) {
                if (key != kImagePathKey) {
                    for_each_image(child, visit);
                }
            }
        }

    } // namespace

    void layout_references(const std::filesystem::path& source,
                           const std::filesystem::path& relative,
                           std::vector<std::filesystem::path>& out) {
        nlohmann::json document;
        if (!read_layout(source, document)) {
            return;
        }
        for_each_image(document, [&](const nlohmann::json& value) {
            const std::filesystem::path named =
                source_of(relative, value.get<std::string>());
            if (!named.empty()) {
                out.push_back(named);
            }
        });
    }

    bool cook_layout(const std::filesystem::path& source, Writer& writer,
                     const std::filesystem::path& cooked,
                     const std::filesystem::path& relative,
                     const std::filesystem::path& content_root) {
        nlohmann::json document;
        if (!read_layout(source, document)) {
            return false;
        }

        bool sound = true;
        for_each_image(document, [&](nlohmann::json& value) {
            const std::string stored = value.get<std::string>();

            // An image entity nobody has assigned yet holds nothing. That is a
            // layout part way through being authored rather than a fault, and
            // moth_ui already draws no image for it.
            if (stored.empty()) {
                return;
            }

            const std::filesystem::path named = source_of(relative, stored);
            if (named.empty()) {
                ENGINE_LOG_ERROR("{}: it names the image '{}', which is outside the content "
                                 "tree. A layout names an image relative to its own "
                                 "directory.",
                                 source.string(), stored);
                sound = false;
                return;
            }

            // The sidecar is what holds the identity, and meta_for writes one
            // when the file has none. It has to: the sources cook in name
            // order, so a layout can reach an image before the texture rule
            // has run, and both have to come out with the same answer.
            as::AssetMeta meta;
            if (!as::meta_for(content_root / named, meta)) {
                // meta_for has already said what is wrong with the file it was
                // handed, one that is not there included. What it cannot say is
                // which layout asked for it, and that is the half a person
                // needs to fix it.
                ENGINE_LOG_ERROR("{}: it names {}, and that has no identity to give.",
                                 source.string(), named.generic_string());
                sound = false;
                return;
            }
            value = meta.guid.to_text();
        });

        if (!sound) {
            return false;
        }

        const std::string text = document.dump(2);
        return writer.write(cooked, std::as_bytes(std::span(text.data(), text.size())));
    }

} // namespace engine::import
