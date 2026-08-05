#include "render/texture_cache.h"

#include "assets/texture.h"
#include "core/assert.h"
#include "core/log.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace engine::render {

    namespace {

        /// The gfx format that matches a cooked format and a cooked color space.
        [[nodiscard]] gfx::TextureFormat to_gfx_format(assets::TextureFormat format,
                                                       assets::ColorSpace space) {
            // A half float carries values above 1 and an sRGB transfer function
            // is defined only from 0 to 1, so this one format ignores the color
            // space rather than choosing on it. The cooker writes Linear for
            // every one it makes.
            if (format == assets::TextureFormat::RGBA16F) {
                return gfx::TextureFormat::RGBA16F;
            }
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
        ENGINE_ASSERT(!fallback_cube_.valid(),
                      "TextureCache::destroy was not called, so the fallback cubemap leaked.");
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

        // One grey texel for each of the six faces, in a linear format because
        // an environment is radiance and not a color to convert. A quarter is
        // dim enough to read as a room with no lamp in it, and bright enough
        // that a metal shows its shape rather than reading black.
        constexpr std::uint8_t kGrey = 64;    // 64 / 255 is about 0.25 linear.
        constexpr std::uint8_t kOpaque = 255; // The alpha of every face texel.
        constexpr std::size_t kFaceTexels = 4;
        constexpr std::size_t kAlphaOffset = 3;
        std::array<std::uint8_t, assets::kCubeFaceCount * kFaceTexels> grey_faces{};
        grey_faces.fill(kGrey);
        for (std::size_t face = 0; face < assets::kCubeFaceCount; ++face) {
            grey_faces[(face * kFaceTexels) + kAlphaOffset] = kOpaque;
        }

        const gfx::TextureDesc cube_desc{
            .pixels = grey_faces.data(),
            .size = grey_faces.size(),
            .width = 1,
            .height = 1,
            .mip_count = 1,
            .face_count = assets::kCubeFaceCount,
            .format = gfx::TextureFormat::RGBA8Unorm,
            .sampler = { .filter = gfx::Filter::Linear,
                         .address = gfx::AddressMode::ClampToEdge },
        };

        const gfx::Result cube_result = gfx::create_texture(device, cube_desc, &fallback_cube_);
        if (!gfx::succeeded(cube_result)) {
            ENGINE_LOG_ERROR("The fallback cubemap failed: {}", gfx::result_name(cube_result));
            return false;
        }
        return true;
    }

    gfx::TextureHandle TextureCache::get(gfx::Device* device, const assets::Content& content,
                                         Guid guid) {
        return load(device, content, guid, 1);
    }

    gfx::TextureHandle TextureCache::get_cube(gfx::Device* device,
                                              const assets::Content& content, Guid guid) {
        return load(device, content, guid, assets::kCubeFaceCount);
    }

    gfx::TextureHandle TextureCache::load(gfx::Device* device, const assets::Content& content,
                                          Guid guid, std::uint32_t faces) {
        const bool cube = faces == assets::kCubeFaceCount;
        const gfx::TextureHandle fallback = cube ? fallback_cube_ : fallback_;

        if (!guid.valid()) {
            return fallback;
        }

        // The shape is part of what was asked for, so it is part of the key.
        // Answering a cubemap request from a flat entry would hand back a
        // texture the binding cannot read.
        const Request request{ guid, faces };

        if (const auto found = loaded_.find(request); found != loaded_.end()) {
            return found->second;
        }
        // A material that names a texture it does not have would otherwise
        // report on every frame, and the log would say nothing else.
        if (failed_.contains(request)) {
            return fallback;
        }

        std::vector<std::byte> bytes;
        assets::TextureView view;
        if (!content.read_bytes(guid, bytes) ||
            !assets::read_texture(bytes, view, guid.to_text())) {
            failed_.emplace(request, true);
            return fallback;
        }

        // The caller asked for one shape and the file holds the other. The
        // driver would take the upload and the binding would then be undefined,
        // because a set layout declares a `sampler2D` or a `samplerCube` and
        // the two are not interchangeable.
        if (view.face_count != faces) {
            ENGINE_LOG_ERROR("{} holds {} faces and the binding needs {}. A cubemap comes "
                             "from an environment rule and a flat texture from the texture "
                             "rule, so check which one the scene named.",
                             guid.to_text(), view.face_count, faces);
            failed_.emplace(request, true);
            return fallback;
        }

        const gfx::TextureDesc desc{
            .pixels = view.payload.data(),
            .size = view.payload.size(),
            .width = view.width,
            .height = view.height,
            .mip_count = view.mip_count,
            .face_count = view.face_count,
            .format = to_gfx_format(view.format, view.color_space),
            // A cube clamps. Repeat is meaningless across a face edge, and the
            // hardware filters across the seam for a cube whatever this says.
            .sampler = { .filter = gfx::Filter::Linear,
                         .address = cube ? gfx::AddressMode::ClampToEdge
                                         : gfx::AddressMode::Repeat },
        };

        gfx::TextureHandle texture;
        const gfx::Result result = gfx::create_texture(device, desc, &texture);
        if (!gfx::succeeded(result)) {
            ENGINE_LOG_ERROR("{} would not upload: {}", guid.to_text(),
                             gfx::result_name(result));
            failed_.emplace(request, true);
            return fallback;
        }

        ENGINE_LOG_INFO("Uploaded {} {}, {} by {} with {} levels.",
                        cube ? "cubemap" : "texture", guid.to_text(), view.width, view.height,
                        view.mip_count);
        loaded_.emplace(request, texture);
        return texture;
    }

    void TextureCache::drop(gfx::Device* device, Guid guid) {
        // Both shapes. The caller has one identity and does not know which way
        // it was asked for, and a reload that freed only one of them would
        // leave the other pointing at a texture the device no longer has.
        for (const std::uint32_t faces : { 1U, assets::kCubeFaceCount }) {
            const Request request{ guid, faces };
            failed_.erase(request);

            const auto found = loaded_.find(request);
            if (found == loaded_.end()) {
                continue;
            }
            if (device != nullptr) {
                gfx::destroy_texture(device, found->second);
            }
            loaded_.erase(found);
        }
    }

    void TextureCache::destroy(gfx::Device* device) {
        if (device != nullptr) {
            for (const auto& [request, texture] : loaded_) {
                gfx::destroy_texture(device, texture);
            }
            gfx::destroy_texture(device, fallback_);
            gfx::destroy_texture(device, fallback_cube_);
        }
        loaded_.clear();
        failed_.clear();
        fallback_ = gfx::TextureHandle{};
        fallback_cube_ = gfx::TextureHandle{};
    }

} // namespace engine::render
