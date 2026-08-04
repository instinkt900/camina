#pragma once

/**
 * @file
 * @brief The cooked texture format, shared by the cooker and the runtime.
 *
 * A cooked texture is one file: a header, then every mip level, largest first,
 * packed with no padding between them. The runtime reads the file into one
 * block of memory and hands the payload straight to the device. There is no
 * decode step and no second copy.
 *
 * The cooker writes this file. Nothing else does. `tools/cooker/texture.cpp`
 * reads a PNG, builds the mip chain, and compresses. This header holds only
 * what both sides must agree on, so a change here is a format change and it
 * moves the format version below.
 */

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace engine::assets {

    /// @brief The name a cooked texture file carries after the source name.
    inline constexpr const char* kTextureExtension = ".tex";

    /**
     * @brief The first four bytes of a cooked texture file.
     *
     * The value spells "CTEX" when a person opens the file in a hex viewer.
     * Reading it first turns a wrong file into a named error rather than into a
     * driver crash over nonsense dimensions.
     */
    inline constexpr std::uint32_t kTextureMagic = 0x58455443U;

    /// @brief The format version this build writes and reads.
    ///
    /// Version 2 added the face count and the half float format, which is what
    /// an HDR environment cubemap needs.
    inline constexpr std::uint32_t kTextureVersion = 2;

    /// @brief The width and the height of one block-compressed block, in texels.
    inline constexpr std::uint32_t kBlockSize = 4;

    /**
     * @brief How a shader must read the texels of a texture.
     *
     * This is the field that DESIGN.md section 3 turns on. The working space is
     * linear, and a texture that holds color arrives encoded as sRGB. Getting it
     * wrong looks almost right, so it is recorded once, at cook time, and both
     * the mip filter and the device format read it.
     */
    enum class ColorSpace : std::uint32_t {
        /// @brief The texels hold color, encoded as sRGB. Base color and emissive.
        Srgb = 0,
        /// @brief The texels hold numbers. Normal, roughness, metallic, occlusion.
        Linear,
    };

    /// @brief How the texels are stored in the cooked file.
    enum class TextureFormat : std::uint32_t {
        /// @brief Four 8-bit channels in RGBA order. 4 bytes for each texel.
        RGBA8 = 0,
        /// @brief BC7 blocks. 16 bytes for each 4 by 4 block of texels.
        BC7,
        /**
         * @brief Four 16-bit half floats in RGBA order. 8 bytes for each texel.
         *
         * This is what an HDR environment needs. An 8-bit channel holds values
         * from 0 to 1, and a sky is brighter than 1 by a wide margin. A sun in
         * an equirectangular capture reaches into the thousands, and clamping it
         * to 1 removes the very thing that lights the scene.
         *
         * Half rather than full float, because a full float chain is twice the
         * memory for range and precision a sky does not use. BC7 is not an
         * option at all, because it is an 8-bit format.
         */
        RGBA16F,
    };

    /// @brief The largest ::TextureFormat value, so a reader can reject the rest.
    inline constexpr std::uint32_t kTextureFormatMax = static_cast<std::uint32_t>(
        TextureFormat::RGBA16F);

    /**
     * @brief How many faces a cubemap holds.
     *
     * The order is the Vulkan one, which is also the glTF and the D3D one: +X,
     * −X, +Y, −Y, +Z, −Z. A cooked cubemap stores every level of face 0, then
     * every level of face 1, and so on. That is the order a device upload wants.
     */
    inline constexpr std::uint32_t kCubeFaceCount = 6;

    /**
     * @brief The fixed-size header at the start of a cooked texture file.
     *
     * Every field is a 32-bit unsigned integer, so the layout is the same on
     * both platforms and the struct needs no packing attribute.
     */
    struct TextureHeader {
        std::uint32_t magic = kTextureMagic;     ///< ::kTextureMagic. Checked first.
        std::uint32_t version = kTextureVersion; ///< ::kTextureVersion when written.
        std::uint32_t format = 0;                ///< A ::TextureFormat value.
        std::uint32_t color_space = 0;           ///< A ::ColorSpace value.
        std::uint32_t width = 0;                 ///< Width of mip level 0, in texels.
        std::uint32_t height = 0;                ///< Height of mip level 0, in texels.
        std::uint32_t mip_count = 0;             ///< How many levels follow. At least 1.
        std::uint32_t payload_size = 0;          ///< Bytes of texel data after this header.
        /// @brief 1 for a flat texture, or ::kCubeFaceCount for a cubemap.
        ///
        /// A cubemap is a texture with six faces rather than an asset kind of
        /// its own. The mip rules, the color space, and the reader are all the
        /// same, and only the count of faces and the view type differ.
        std::uint32_t face_count = 1;
    };

    /**
     * @brief A cooked texture that a caller already holds in memory.
     *
     * The bytes belong to the caller. This only points into them, so it stays
     * valid exactly as long as the buffer behind it does.
     */
    struct TextureView {
        TextureFormat format = TextureFormat::RGBA8; ///< How the texels are stored.
        ColorSpace color_space = ColorSpace::Srgb;   ///< How a shader must read them.
        std::uint32_t width = 0;                     ///< Width of mip level 0, in texels.
        std::uint32_t height = 0;                    ///< Height of mip level 0, in texels.
        std::uint32_t mip_count = 0;                 ///< How many levels the payload holds.
        std::uint32_t face_count = 1;                ///< 1, or ::kCubeFaceCount for a cubemap.
        std::span<const std::byte> payload;          ///< Every level of every face, face by face.
    };

    /**
     * @brief The size of one mip level, in texels, never smaller than one.
     * @param base The size of mip level 0 on this axis.
     * @param level The level to measure. Level 0 gives @p base back.
     * @return The size of that level on that axis.
     */
    [[nodiscard]] std::uint32_t mip_extent(std::uint32_t base, std::uint32_t level);

    /**
     * @brief How many mip levels a texture of this size can hold.
     *
     * The chain runs down to 1 by 1, so a 256 by 256 texture holds 9 levels.
     *
     * @param width Width of mip level 0, in texels.
     * @param height Height of mip level 0, in texels.
     * @return The number of levels. It is 1 for a 1 by 1 texture and 0 for an
     * empty one.
     */
    [[nodiscard]] std::uint32_t mip_count_for(std::uint32_t width, std::uint32_t height);

    /**
     * @brief How many bytes one mip level takes.
     *
     * A BC7 level rounds up to whole 4 by 4 blocks, so a 2 by 2 level still
     * costs one block of 16 bytes. That rounding is why a caller must not work
     * this out from the extent alone.
     *
     * @param format How the texels are stored.
     * @param width Width of the level, in texels.
     * @param height Height of the level, in texels.
     * @return The size of that level, in bytes.
     */
    [[nodiscard]] std::size_t level_bytes(TextureFormat format, std::uint32_t width,
                                          std::uint32_t height);

    /**
     * @brief How many bytes a whole mip chain takes.
     * @param format How the texels are stored.
     * @param width Width of mip level 0, in texels.
     * @param height Height of mip level 0, in texels.
     * @param mip_count How many levels the chain holds.
     * @return The sum over every level.
     */
    [[nodiscard]] std::size_t chain_bytes(TextureFormat format, std::uint32_t width,
                                          std::uint32_t height, std::uint32_t mip_count);

    /**
     * @brief Reads the header of a cooked texture and points at its payload.
     *
     * This checks the magic, the version, and that the payload is exactly the
     * size the dimensions call for. A file that fails any check is reported by
     * name and refused, because the alternative is a device upload that reads
     * past the end of the buffer.
     *
     * @param bytes The whole file, as read from disk.
     * @param out The view to fill. It points into @p bytes.
     * @param where A name for the log, usually the asset path.
     * @return True when the file is a cooked texture this build understands.
     */
    [[nodiscard]] bool read_texture(std::span<const std::byte> bytes, TextureView& out,
                                    std::string_view where);

    /**
     * @brief The text form of a color space, for reflect/.
     *
     * Without this a sidecar would hold `"color_space": 1`, because reflect/
     * writes a plain enum as its underlying number. A person edits that file by
     * hand to fix a texture that came out wrong, so it holds `"Linear"` instead.
     *
     * @param value The color space to write.
     * @return "sRGB" or "Linear".
     */
    [[nodiscard]] std::string to_text(const ColorSpace& value);

    /**
     * @brief Reads the text form of a color space, for reflect/.
     * @param text The word to read. The comparison ignores letter case.
     * @param out The color space to fill. It stays as it was when this fails.
     * @return True when @p text names a color space.
     */
    [[nodiscard]] bool from_text(std::string_view text, ColorSpace& out);

} // namespace engine::assets
