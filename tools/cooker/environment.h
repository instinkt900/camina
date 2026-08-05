#pragma once

/**
 * @file
 * @brief The cooker rule that turns an HDR image into an environment cubemap.
 *
 * An equirectangular `.hdr` file is one rectangle that wraps the whole sphere.
 * A GPU samples an environment by direction, which is a cubemap. This rule is
 * the projection between the two, and it is a cook step because the projection
 * costs the same every run and the answer never changes.
 *
 * `src/assets/texture.h` holds the file format. A cubemap is a texture with six
 * faces, so this writes the same format the image rule does, with a face count
 * of six and a half float payload.
 */

#include "assets/meta.h"

#include <filesystem>
#include <string>

namespace cooker {

    /**
     * @brief Whether this rule handles a file with this extension.
     *
     * Only `.hdr`. A `.png` sky is not an environment, because an 8-bit channel
     * cannot carry a sun, and reading one as an environment would give a dull
     * grey reflection with nothing to say why.
     *
     * @param extension The extension, with the dot, in any letter case.
     * @return True for a Radiance HDR file.
     */
    [[nodiscard]] bool is_environment_extension(const std::string& extension);

    /**
     * @brief Projects an equirectangular HDR image onto a cubemap and writes it.
     *
     * The mip chain is built in linear light, which needs no decode step here
     * because an HDR file is already linear. See DESIGN.md section 3 for why
     * that matters for an 8-bit image and not for this one.
     *
     * @param source The `.hdr` file to read.
     * @param destination Where to write the cooked cubemap.
     * @param settings The face size, from the sidecar.
     * @return True when the file was read, projected, and written.
     */
    [[nodiscard]] bool cook_environment(const std::filesystem::path& source,
                                        const std::filesystem::path& destination,
                                        const engine::assets::EnvironmentImport& settings);

} // namespace cooker
