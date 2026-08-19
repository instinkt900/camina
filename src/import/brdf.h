#pragma once

/**
 * @file
 * @brief The cooker rule that builds the split sum BRDF table.
 *
 * The specular half of image based lighting splits into two averages. One is
 * the environment blurred by roughness, which the `.hdr` rule writes. The other
 * is this: how much of a reflection survives the surface, as a scale and a bias
 * on the Fresnel reflectance at normal incidence.
 *
 * It depends on no environment, no material, and no scene. It is the same table
 * in every project, so it is cooked once from a source file that carries only
 * its size and its ray budget. Every environment then shares it.
 *
 * `src/assets/texture.h` holds the output format. The table is a two channel
 * function stored in a four channel texture, so red is the scale and green is
 * the bias, and blue and alpha are unused.
 */

#include "assets/meta.h"

#include <filesystem>
#include <string>

namespace engine::import {

    /**
     * @brief Whether this rule handles a file with this extension.
     *
     * Only `.brdf`. The extension names what the table is rather than what it
     * is shaped like, so a lookup table of some other kind later does not land
     * on this rule by accident.
     *
     * @param extension The extension, with the dot, in any letter case.
     * @return True for a BRDF table source.
     */
    [[nodiscard]] bool is_brdf_extension(const std::string& extension);

    /**
     * @brief Integrates the split sum BRDF and writes it as a cooked texture.
     *
     * One axis of the table is the cosine of the angle to the viewer and the
     * other is roughness, both running from 0 to 1 across the texels. A shader
     * reads it with those two numbers and multiplies the prefiltered
     * environment by `F0 * red + green`.
     *
     * The source file is read for nothing but its identity. Everything the rule
     * needs is in @p settings.
     *
     * @param destination Where to write the cooked table.
     * @param settings The size and the ray budget, from the sidecar.
     * @return True when the table was integrated and written.
     */
    [[nodiscard]] bool cook_brdf(const std::filesystem::path& destination,
                                 const engine::assets::BrdfImport& settings);

} // namespace engine::import
