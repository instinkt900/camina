#include "assets/meta.h"

#include "core/log.h"
#include "reflect/json.h"

#include <nlohmann/json.hpp>

#include <system_error>

namespace engine::assets {

    namespace {

        /// Which import block belongs to the asset this extension names.
        /// Returns an empty string when the extension is unknown.
        [[nodiscard]] const char* import_key_for(std::string_view extension) {
            if (extension == ".vert" || extension == ".frag" || extension == ".comp") {
                return "shader";
            }
            if (extension == ".png" || extension == ".jpg" || extension == ".jpeg" ||
                extension == ".tga" || extension == ".bmp" || extension == ".psd") {
                return "texture";
            }
            if (extension == ".hdr") {
                return "environment";
            }
            if (extension == ".brdf") {
                return "brdf";
            }
            return {};
        }

        /// Writes a fresh sidecar with only the relevant import block.
        [[nodiscard]] bool save_fresh_meta(const std::filesystem::path& source,
                                           const AssetMeta& meta) {
            const std::filesystem::path sidecar = meta_path(source);

            nlohmann::json doc;
            doc["guid"] = meta.guid.to_text();

            const std::string ext = source.extension().string();
            const char* import_key = import_key_for(ext);
            if (import_key != nullptr && import_key[0] != '\0') {
                if (import_key == std::string_view{ "texture" }) {
                    doc[import_key] = reflect::to_json(meta.texture);
                } else if (import_key == std::string_view{ "shader" }) {
                    doc[import_key] = reflect::to_json(meta.shader);
                } else if (import_key == std::string_view{ "environment" }) {
                    doc[import_key] = reflect::to_json(meta.environment);
                } else if (import_key == std::string_view{ "brdf" }) {
                    doc[import_key] = reflect::to_json(meta.brdf);
                }
            }

            std::ofstream file(sidecar, std::ios::binary | std::ios::trunc);
            if (!file) {
                ENGINE_LOG_ERROR("{}: could not open it to write the identity.", sidecar.string());
                return false;
            }
            file << doc.dump(2) << '\n';
            if (!file) {
                ENGINE_LOG_ERROR("{}: the sidecar did not write.", sidecar.string());
                return false;
            }
            return true;
        }

    } // namespace

    std::filesystem::path meta_path(const std::filesystem::path& source) {
        std::filesystem::path path = source;
        path += kMetaExtension;
        return path;
    }

    bool load_meta(const std::filesystem::path& source, AssetMeta& out) {
        return reflect::load_json(meta_path(source), out);
    }

    bool save_meta(const std::filesystem::path& source, const AssetMeta& meta) {
        return reflect::save_json(meta_path(source), meta);
    }

    bool meta_for(const std::filesystem::path& source, AssetMeta& out, bool* created) {
        if (created != nullptr) {
            *created = false;
        }

        std::error_code error;
        if (!std::filesystem::is_regular_file(source, error)) {
            ENGINE_LOG_ERROR("{} is not a file, so it gets no asset identity.",
                             source.string());
            return false;
        }

        // Ask before reading. A source file with no sidecar yet is the normal
        // case on a first cook, and load_meta would report it as an error.
        const std::filesystem::path sidecar = meta_path(source);
        if (std::filesystem::is_regular_file(sidecar, error)) {
            AssetMeta existing;
            if (load_meta(source, existing) && existing.guid.valid()) {
                out = existing;
                return true;
            }
            ENGINE_LOG_WARN("{} holds no usable identity. Writing a new one.",
                            sidecar.string());
        }

        AssetMeta fresh;
        fresh.guid = Guid::generate();
        if (!save_fresh_meta(source, fresh)) {
            return false;
        }
        ENGINE_LOG_INFO("{} is now {}.", source.string(), fresh.guid.to_text());
        out = fresh;
        if (created != nullptr) {
            *created = true;
        }
        return true;
    }

} // namespace engine::assets
