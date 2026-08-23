#pragma once

/**
 * @file
 * @brief The cooker rule that turns an image file into a cooked texture.
 *
 * This is the only place that reads a PNG, builds a mip chain, or runs a block
 * compressor. `src/assets/texture.h` holds the file format the two sides agree
 * on, and the runtime reads that and nothing else.
 */

#include "assets/meta.h"
#include "assets/texture.h"
#include "import/writer.h"

#include <optional>
#include <cstddef>
#include <filesystem>
#include <span>
#include <string_view>

namespace engine::import {

    /**
     * @brief Whether this rule handles a file with this extension.
     * @param extension The extension, with the dot, in any letter case.
     * @return True for an image format stb_image reads.
     */
    [[nodiscard]] bool is_image_extension(const std::string& extension);

    /**
     * @brief Guesses how a texture must be read, from its file name.
     *
     * A guess is only ever written into a new sidecar. After that the sidecar
     * decides, so a wrong guess is one edit to fix and it never comes back.
     *
     * The rule follows the names that glTF exporters produce. A name that holds
     * `normal`, `roughness`, `metallic`, `occlusion`, `height`, `mask`, or the
     * packed `orm` reads as linear. Everything else reads as sRGB, because base
     * color is the common case and it is the one that looks wrong immediately.
     *
     * @param source The source image path.
     * @return The color space to record in a new sidecar.
     */
    [[nodiscard]] engine::assets::ColorSpace guess_color_space(
        const std::filesystem::path& source);

    /**
     * @brief Reads the sidecar of an image, and writes one with a guess when
     * there is none.
     *
     * Two rules ask an image for its identity. The texture rule asks because it
     * is about to cook it, and the glTF rule asks because a material stores the
     * identity of every texture it names. Whichever one arrives first may find
     * no sidecar and write it.
     *
     * So the guess belongs here rather than in either rule. A sidecar the glTF
     * rule wrote would otherwise carry the default of sRGB, and every normal map
     * in that model would read as color from then on.
     *
     * @param source The image path. The file must exist.
     * @param out The metadata to fill.
     * @return True when @p out holds a valid GUID.
     */
    [[nodiscard]] bool image_meta(const std::filesystem::path& source,
                                  engine::assets::AssetMeta& out,
                                  std::optional<engine::assets::ColorSpace> known = std::nullopt);

    /**
     * @brief Cooks one image into one cooked texture file.
     *
     * @param source The image to read. PNG, JPG, TGA, BMP, or PSD.
     * @param writer Where the cooked file goes.
     * @param cooked The cooked path, relative to the cooked root.
     * @param settings What the sidecar says to do.
     * @return True when the cooked file was written. False reports why in the
     * log, by source path.
     */
    [[nodiscard]] bool cook_texture(const std::filesystem::path& source, Writer& writer,
                                    const std::filesystem::path& cooked,
                                    const engine::assets::TextureImport& settings);

    /**
     * @brief Cooks an image already in memory into one cooked texture file.
     *
     * A glTF can carry an image inside itself, either in a buffer view or as a
     * data URI. Such an image has no file, so it has no sidecar and nothing
     * decides its import settings. The glTF rule supplies them.
     *
     * @param bytes The encoded image, in any format stb_image reads.
     * @param writer Where the cooked file goes.
     * @param cooked The cooked path, relative to the cooked root.
     * @param settings What to do with it. The glTF rule works these out from
     * the material slot the image is used in, because there is no file name to
     * guess from.
     * @param where A name for the log, usually the glTF path and the index.
     * @return True when the cooked file was written. False reports why in the
     * log.
     */
    [[nodiscard]] bool cook_texture_bytes(std::span<const std::byte> bytes, Writer& writer,
                                          const std::filesystem::path& cooked,
                                          const engine::assets::TextureImport& settings,
                                          std::string_view where);

} // namespace engine::import
