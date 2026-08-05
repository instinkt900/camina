#pragma once

/**
 * @file
 * @brief The cooked irradiance format, shared by the cooker and the runtime.
 *
 * The diffuse half of image based lighting. An environment lights a matte
 * surface from every direction at once, and integrating that per pixel is far
 * too expensive. Nine coefficients of a second order spherical harmonic hold
 * the answer to within a percent or two, because irradiance over a hemisphere
 * is a very smooth function of the normal. See DESIGN.md section 9.
 *
 * The cooker writes this file and nothing else does. It is a sub-asset of the
 * `.hdr` rule, under a GUID `Guid::derive` works out from the panorama, so a
 * scene names one environment and gets both parts.
 *
 * @warning The coefficients carry the convolution already. Evaluating the
 * standard basis against a normal gives irradiance, not radiance. See
 * engine::assets::IrradianceSH for the exact sum a shader must write.
 */

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace engine::assets {

    /// @brief The name a cooked irradiance file carries after the source name.
    inline constexpr const char* kIrradianceExtension = ".irr";

    /**
     * @brief The first four bytes of a cooked irradiance file.
     *
     * The value spells "CIRR" when a person opens the file in a hex viewer.
     */
    inline constexpr std::uint32_t kIrradianceMagic = 0x52524943U;

    /// @brief The format version this build writes and reads.
    inline constexpr std::uint32_t kIrradianceVersion = 1;

    /// @brief How many coefficients a second order spherical harmonic holds.
    inline constexpr std::uint32_t kIrradianceCoefficients = 9;

    /// @brief How many channels each coefficient carries. Linear RGB.
    inline constexpr std::uint32_t kIrradianceChannels = 3;

    /**
     * @brief The nine RGB coefficients, and how to read them.
     *
     * A shader rebuilds irradiance from a normal with the standard second order
     * basis. Written out, with `n` normalized:
     *
     * @code
     * vec3 irradiance =
     *       c[0]
     *     + c[1] * n.y + c[2] * n.z + c[3] * n.x
     *     + c[4] * (n.x * n.y)
     *     + c[5] * (n.y * n.z)
     *     + c[6] * ((3.0 * n.z * n.z) - 1.0)
     *     + c[7] * (n.x * n.z)
     *     + c[8] * ((n.x * n.x) - (n.y * n.y));
     * @endcode
     *
     * Every constant the basis needs is folded into the coefficients at cook
     * time, together with the per band convolution that turns radiance into
     * irradiance. So the sum above is the whole of it, and a shader carries no
     * table of its own to drift from this one.
     *
     * Divide by pi for the Lambert term. That belongs to the material rather
     * than to the environment, so the cooker leaves it alone.
     */
    struct IrradianceSH {
        /// @brief Nine coefficients, each linear RGB.
        std::array<std::array<float, kIrradianceChannels>, kIrradianceCoefficients> c{};
    };

    /**
     * @brief The fixed-size header at the start of a cooked irradiance file.
     *
     * Every field is a 32-bit unsigned integer, so the layout is the same on
     * both platforms and the struct needs no packing attribute.
     */
    struct IrradianceHeader {
        std::uint32_t magic = kIrradianceMagic;               ///< ::kIrradianceMagic. Checked first.
        std::uint32_t version = kIrradianceVersion;           ///< ::kIrradianceVersion when written.
        std::uint32_t coefficients = kIrradianceCoefficients; ///< ::kIrradianceCoefficients.
        std::uint32_t channels = kIrradianceChannels;         ///< ::kIrradianceChannels.
    };

    /**
     * @brief Reads a cooked irradiance file.
     *
     * This checks the magic, the version, and that the file holds exactly the
     * coefficients the header claims. A file that fails any check is reported
     * by name and refused.
     *
     * @param bytes The whole file, as read from disk.
     * @param out The coefficients to fill. Untouched when this fails.
     * @param where A name for the log, usually the asset path.
     * @return True when the file is cooked irradiance this build understands.
     */
    [[nodiscard]] bool read_irradiance(std::span<const std::byte> bytes, IrradianceSH& out,
                                       std::string_view where);

} // namespace engine::assets
