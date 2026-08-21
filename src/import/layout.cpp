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

        /// What a moth_ui reference entity calls the layout it stands for.
        constexpr const char* kLayoutPathKey = "layoutPath";

        /**
         * Reads a layout, and says why it could not when @p report is true.
         *
         * The input scan runs before anything cooks and reads every layout to
         * find the images it names. It stays quiet, because the rule reads the
         * same file a moment later and reports there. Both reporting doubles
         * every message a broken layout produces.
         */
        [[nodiscard]] bool read_layout(const std::filesystem::path& source,
                                       nlohmann::json& out, bool report) {
            std::ifstream file(source);
            if (!file) {
                if (report) {
                    ENGINE_LOG_ERROR("{}: the layout could not be opened.", source.string());
                }
                return false;
            }
            try {
                file >> out;
            } catch (const nlohmann::json::exception& error) {
                if (report) {
                    ENGINE_LOG_ERROR("{}: the layout is not readable JSON. {}",
                                     source.string(), error.what());
                }
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
         * Calls @p visit for every string field named @p key in the tree.
         *
         * A layout nests: a group holds children and so does the layout itself.
         * So this walks everything rather than the top level, and it keys on
         * the field rather than on the entity type. A field named `imagePath`
         * is an image path whatever entity carries it, and a moth_ui release
         * that adds an entity kind with one keeps working. The same holds for
         * `layoutPath` and a reference.
         */
        template <typename Visit>
        void for_each_reference(nlohmann::json& node, const char* key, const Visit& visit) {
            if (node.is_array()) {
                for (nlohmann::json& child : node) {
                    for_each_reference(child, key, visit);
                }
                return;
            }
            if (!node.is_object()) {
                return;
            }
            if (const auto found = node.find(key);
                found != node.end() && found->is_string()) {
                visit(*found);
            }
            for (const auto& [name, child] : node.items()) {
                if (name != key) {
                    for_each_reference(child, key, visit);
                }
            }
        }

    } // namespace

    void layout_references(const std::filesystem::path& source,
                           const std::filesystem::path& relative,
                           std::vector<std::filesystem::path>& out) {
        nlohmann::json document;
        if (!read_layout(source, document, false)) {
            return;
        }
        // A referenced layout is an input as much as an image is, so editing a
        // button re-cooks every menu that stands one up.
        const auto collect = [&](const nlohmann::json& value) {
            const std::filesystem::path named =
                source_of(relative, value.get<std::string>());
            if (!named.empty()) {
                out.push_back(named);
            }
        };
        for_each_reference(document, kImagePathKey, collect);
        for_each_reference(document, kLayoutPathKey, collect);
    }

    bool cook_layout(const std::filesystem::path& source, Writer& writer,
                     const std::filesystem::path& cooked,
                     const std::filesystem::path& relative,
                     const std::filesystem::path& content_root) {
        nlohmann::json document;
        if (!read_layout(source, document, true)) {
            return false;
        }

        bool sound = true;

        /**
         * Turns one stored reference into the identity of what it names.
         *
         * The same job for an image and for a sub-layout, because moth_ui stores
         * both the same way: a path relative to the directory the layout sits in.
         * `kind` is only there so a message names what was being looked for.
         */
        const auto resolve = [&](nlohmann::json& value, const char* kind) {
            const std::string stored = value.get<std::string>();

            // An entity nobody has assigned yet holds nothing. That is a layout
            // part way through being authored rather than a fault, and moth_ui
            // draws nothing for it either way.
            if (stored.empty()) {
                return;
            }

            const std::filesystem::path named = source_of(relative, stored);
            if (named.empty()) {
                ENGINE_LOG_ERROR("{}: it names the {} '{}', which is outside the content "
                                 "tree. A layout names one relative to its own directory.",
                                 source.string(), kind, stored);
                sound = false;
                return;
            }

            // The sidecar is what holds the identity, and meta_for writes one
            // when the file has none. It has to: the sources cook in name
            // order, so a layout can reach an image or another layout before
            // the rule for it has run, and both have to come out with the same
            // answer.
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
        };

        for_each_reference(document, kImagePathKey,
                           [&](nlohmann::json& value) { resolve(value, "image"); });
        for_each_reference(document, kLayoutPathKey,
                           [&](nlohmann::json& value) { resolve(value, "layout"); });

        if (!sound) {
            return false;
        }

        const std::string text = document.dump(2);
        return writer.write(cooked, std::as_bytes(std::span(text.data(), text.size())));
    }

} // namespace engine::import
