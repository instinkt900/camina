#include "assets/meta.h"

#include "core/log.h"
#include "reflect/json.h"

#include <system_error>

namespace engine::assets {

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

    bool meta_for(const std::filesystem::path& source, AssetMeta& out) {
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

        const AssetMeta fresh{ .guid = Guid::generate() };
        if (!save_meta(source, fresh)) {
            return false;
        }
        ENGINE_LOG_INFO("{} is now {}.", source.string(), fresh.guid.to_text());
        out = fresh;
        return true;
    }

} // namespace engine::assets
