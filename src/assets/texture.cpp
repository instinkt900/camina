#include "assets/texture.h"

#include "core/log.h"

#include <algorithm>
#include <cstring>

namespace engine::assets {

    namespace {

        /// Matches two words without regard to letter case.
        [[nodiscard]] bool same_word(std::string_view a, std::string_view b) {
            return std::ranges::equal(a, b, [](char left, char right) {
                const auto lower = [](char c) {
                    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
                };
                return lower(left) == lower(right);
            });
        }

    } // namespace

    std::uint32_t mip_extent(std::uint32_t base, std::uint32_t level) {
        const std::uint32_t shifted = level >= 32U ? 0U : base >> level;
        return std::max(shifted, 1U);
    }

    std::uint32_t mip_count_for(std::uint32_t width, std::uint32_t height) {
        if (width == 0 || height == 0) {
            return 0;
        }
        std::uint32_t levels = 1;
        std::uint32_t size = std::max(width, height);
        while (size > 1) {
            size /= 2;
            ++levels;
        }
        return levels;
    }

    std::size_t level_bytes(TextureFormat format, std::uint32_t width, std::uint32_t height) {
        if (format == TextureFormat::BC7) {
            // A block covers 4 by 4 texels whether or not the level fills it, so
            // a 2 by 2 level still costs one whole block.
            const std::size_t blocks_across = (width + kBlockSize - 1) / kBlockSize;
            const std::size_t blocks_down = (height + kBlockSize - 1) / kBlockSize;
            constexpr std::size_t kBytesPerBlock = 16;
            return blocks_across * blocks_down * kBytesPerBlock;
        }
        constexpr std::size_t kBytesPerTexel = 4;
        return static_cast<std::size_t>(width) * height * kBytesPerTexel;
    }

    std::size_t chain_bytes(TextureFormat format, std::uint32_t width, std::uint32_t height,
                            std::uint32_t mip_count) {
        std::size_t total = 0;
        for (std::uint32_t level = 0; level < mip_count; ++level) {
            total += level_bytes(format, mip_extent(width, level), mip_extent(height, level));
        }
        return total;
    }

    bool read_texture(std::span<const std::byte> bytes, TextureView& out, std::string_view where) {
        if (bytes.size() < sizeof(TextureHeader)) {
            ENGINE_LOG_ERROR("{}: too short to be a cooked texture. It holds {} bytes.", where,
                             bytes.size());
            return false;
        }

        // A copy, not a cast. The file may sit at any alignment in the caller's
        // buffer, and reading a struct through a misaligned pointer is undefined.
        TextureHeader header{};
        std::memcpy(&header, bytes.data(), sizeof(header));

        if (header.magic != kTextureMagic) {
            ENGINE_LOG_ERROR("{}: not a cooked texture. Cook the content tree again.", where);
            return false;
        }
        if (header.version != kTextureVersion) {
            ENGINE_LOG_ERROR("{}: written by version {} and this build reads version {}. "
                             "Cook the content tree again.",
                             where, header.version, kTextureVersion);
            return false;
        }
        if (header.width == 0 || header.height == 0 || header.mip_count == 0) {
            ENGINE_LOG_ERROR("{}: the header holds no texels. It says {} by {} with {} levels.",
                             where, header.width, header.height, header.mip_count);
            return false;
        }
        if (header.format > static_cast<std::uint32_t>(TextureFormat::BC7) ||
            header.color_space > static_cast<std::uint32_t>(ColorSpace::Linear)) {
            ENGINE_LOG_ERROR("{}: the header names a format or a color space this build does "
                             "not have.",
                             where);
            return false;
        }
        if (header.mip_count > mip_count_for(header.width, header.height)) {
            ENGINE_LOG_ERROR("{}: {} levels is more than {} by {} texels can hold.", where,
                             header.mip_count, header.width, header.height);
            return false;
        }

        const auto format = static_cast<TextureFormat>(header.format);
        const std::size_t wanted =
            chain_bytes(format, header.width, header.height, header.mip_count);
        const std::size_t have = bytes.size() - sizeof(TextureHeader);

        // Both directions matter. Too few bytes makes the device read past the
        // end of the buffer, and too many means the file is not what the header
        // describes.
        if (header.payload_size != wanted || have != wanted) {
            ENGINE_LOG_ERROR("{}: the header calls for {} bytes of texels, it says it holds {}, "
                             "and the file holds {}.",
                             where, wanted, header.payload_size, have);
            return false;
        }

        out.format = format;
        out.color_space = static_cast<ColorSpace>(header.color_space);
        out.width = header.width;
        out.height = header.height;
        out.mip_count = header.mip_count;
        out.payload = bytes.subspan(sizeof(TextureHeader));
        return true;
    }

    std::string to_text(const ColorSpace& value) {
        return value == ColorSpace::Linear ? "Linear" : "sRGB";
    }

    bool from_text(std::string_view text, ColorSpace& out) {
        if (same_word(text, "sRGB")) {
            out = ColorSpace::Srgb;
            return true;
        }
        if (same_word(text, "Linear")) {
            out = ColorSpace::Linear;
            return true;
        }
        return false;
    }

} // namespace engine::assets
