#include "render/material_cache.h"

#include "core/log.h"

#include <cstddef>
#include <utility>
#include <vector>

namespace engine::render {

    const GpuMaterial& MaterialCache::get(gfx::Device* device, const assets::Content& content,
                                          TextureCache& textures, Guid guid) {
        // The fallback follows whatever the texture cache calls its fallback,
        // so a caller that binds this before create() ran gets a null handle
        // rather than a stale one.
        fallback_.base_color = textures.fallback();

        if (!guid.valid()) {
            return fallback_;
        }
        if (const auto found = loaded_.find(guid); found != loaded_.end()) {
            return found->second;
        }
        // A submesh that names a material it does not have would otherwise
        // report on every frame, and the log would say nothing else.
        if (failed_.contains(guid)) {
            return fallback_;
        }

        std::vector<std::byte> bytes;
        assets::Material material;
        if (!content.read_bytes(guid, bytes) ||
            !assets::read_material(bytes, material, guid.to_text())) {
            failed_.emplace(guid, true);
            return fallback_;
        }

        GpuMaterial built{
            .source = material,
            .base_color = textures.get(device, content, material.base_color),
        };

        ENGINE_LOG_INFO("Read material {}.", guid.to_text());
        return loaded_.emplace(guid, std::move(built)).first->second;
    }

    void MaterialCache::destroy() {
        loaded_.clear();
        failed_.clear();
        fallback_ = GpuMaterial{};
    }

} // namespace engine::render
