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
 * `src/assets/texture.h` holds the cubemap format. A cubemap is a texture with
 * six faces, so this writes the same format the image rule does, with a face
 * count of six and a half float payload. `src/assets/irradiance.h` holds the
 * second output, which is the diffuse half of image based lighting.
 */

#include "assets/manifest.h"
#include "assets/meta.h"
#include "core/guid.h"

#include <filesystem>
#include <string>
#include <vector>

namespace engine::import {

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
     * @brief Turns an equirectangular HDR image into the two parts of an environment.
     *
     * The rule writes both halves of image based lighting from one read of the
     * panorama.
     *
     * Part one is the cubemap, and it keeps the identity of the source file, so
     * a scene that names an environment needs no change. Its mip chain is the
     * prefiltered specular chain: level 0 is the environment as it is, and each
     * level below it holds the GGX lobe for a rougher surface. That is why the
     * chain is not a box filter.
     *
     * Part two is the irradiance, as nine coefficients of a spherical harmonic,
     * under a GUID `Guid::derive` works out from @p parent under the kind word
     * `irradiance`. A caller finds it from the environment identity alone.
     *
     * Everything happens in linear light, which needs no decode step here
     * because an HDR file is already linear. See DESIGN.md section 3.
     *
     * @param source The `.hdr` file to read.
     * @param out_root The cooked root every output is written under.
     * @param relative The source path, relative to the content root.
     * @param parent The identity from the sidecar. The cubemap keeps it.
     * @param settings The face size and the ray budget, from the sidecar.
     * @param outputs Receives one entry for each file written.
     * @return True when the file was read, projected, and both parts written.
     */
    [[nodiscard]] bool cook_environment(
        const std::filesystem::path& source, const std::filesystem::path& out_root,
        const std::filesystem::path& relative, engine::Guid parent,
        const engine::assets::EnvironmentImport& settings,
        std::vector<engine::assets::ManifestOutput>& outputs);

} // namespace engine::import
