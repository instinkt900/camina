#include "render/texture_cache.h"

#include "assets/texture.h"
#include "core/assert.h"
#include "core/log.h"

#include <array>
#include <cstdint>
#include <vector>

namespace engine::render {

    namespace {

        /// The gfx format that matches a cooked format and a cooked color space.
        [[nodiscard]] gfx::TextureFormat to_gfx_format(assets::TextureFormat format,
                                                       assets::ColorSpace space) {
            const bool srgb = space == assets::ColorSpace::Srgb;
            if (format == assets::TextureFormat::BC7) {
                return srgb ? gfx::TextureFormat::BC7Srgb : gfx::TextureFormat::BC7Unorm;
            }
            return srgb ? gfx::TextureFormat::RGBA8Srgb : gfx::TextureFormat::RGBA8Unorm;
        }

    } // namespace

    TextureCache::~TextureCache() {
        ENGINE_ASSERT(loaded_.empty(),
                      "TextureCache::destroy was not called, so GPU textures leaked.");
        ENGINE_ASSERT(!fallback_.valid(),
                      "TextureCache::destroy was not called, so the fallback texture leaked.");
    }

    bool TextureCache::create(gfx::Device* device) {
        // One white texel, in sRGB because that is what a base color texture
        // is. White is the identity, so a material that binds this shows its
        // own color and nothing else.
        constexpr std::array<std::uint8_t, 4> kWhite{ 255, 255, 255, 255 };
        const gfx::TextureDesc desc{
            .pixels = kWhite.data(),
            .size = kWhite.size(),
            .width = 1,
            .height = 1,
            .mip_count = 1,
            .format = gfx::TextureFormat::RGBA8Srgb,
            .sampler = { .filter = gfx::Filter::Linear },
        };

        const gfx::Result result = gfx::create_texture(device, desc, &fallback_);
        if (!gfx::succeeded(result)) {
            ENGINE_LOG_ERROR("The fallback texture failed: {}", gfx::result_name(result));
            return false;
        }
        return true;
    }

    gfx::TextureHandle TextureCache::get(gfx::Device* device, const assets::Content& content,
                                         Guid guid) {
        if (!guid.valid()) {
            return fallback_;
        }
        if (const auto found = loaded_.find(guid); found != loaded_.end()) {
            return found->second;
        }
        // A material that names a texture it does not have would otherwise
        // report on every frame, and the log would say nothing else.
        if (failed_.contains(guid)) {
            return fallback_;
        }

        std::vector<std::byte> bytes;
        assets::TextureView view;
        if (!content.read_bytes(guid, bytes) ||
            !assets::read_texture(bytes, view, guid.to_text())) {
            failed_.emplace(guid, true);
            return fallback_;
        }

        const gfx::TextureDesc desc{
            .pixels = view.payload.data(),
            .size = view.payload.size(),
            .width = view.width,
            .height = view.height,
            .mip_count = view.mip_count,
            .format = to_gfx_format(view.format, view.color_space),
            .sampler = { .filter = gfx::Filter::Linear },
        };

        gfx::TextureHandle texture;
        const gfx::Result result = gfx::create_texture(device, desc, &texture);
        if (!gfx::succeeded(result)) {
            ENGINE_LOG_ERROR("{} would not upload: {}", guid.to_text(),
                             gfx::result_name(result));
            failed_.emplace(guid, true);
            return fallback_;
        }

        ENGINE_LOG_INFO("Uploaded texture {}, {} by {} with {} levels.", guid.to_text(),
                        view.width, view.height, view.mip_count);
        loaded_.emplace(guid, texture);
        return texture;
    }

    void TextureCache::drop(gfx::Device* device, Guid guid) {
        failed_.erase(guid);

        const auto found = loaded_.find(guid);
        if (found == loaded_.end()) {
            return;
        }
        if (device != nullptr) {
            gfx::destroy_texture(device, found->second);
        }
        loaded_.erase(found);
    }

    void TextureCache::destroy(gfx::Device* device) {
        if (device != nullptr) {
            for (const auto& [guid, texture] : loaded_) {
                gfx::destroy_texture(device, texture);
            }
            gfx::destroy_texture(device, fallback_);
        }
        loaded_.clear();
        failed_.clear();
        fallback_ = gfx::TextureHandle{};
    }

} // namespace engine::render
