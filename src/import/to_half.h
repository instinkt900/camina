#pragma once

/**
 * @file
 * @brief Converts a float to a 16-bit half float.
 *
 * A cooked environment and the split sum lookup table both store half floats,
 * so the encoder lives here rather than being written once for each rule that
 * needs it. Issue #114 held the duplication this replaces.
 */

#include <cstdint>
#include <cstring>

namespace engine::import {

    /// @brief How many mantissa bits a 32-bit float carries.
    constexpr std::uint32_t kFloatMantissaBits = 23;
    /// @brief The exponent of a 32-bit float, once shifted down.
    constexpr std::uint32_t kFloatExponentMask = 0xFFU;
    /// @brief The mantissa of a 32-bit float, in place.
    constexpr std::uint32_t kFloatMantissaMask = 0x7FFFFFU;
    /// @brief The one a normal float leaves out of its mantissa.
    constexpr std::uint32_t kFloatImplicitOne = 0x800000U;
    /// @brief What a 32-bit float adds to its exponent to keep it unsigned.
    constexpr std::int32_t kFloatExponentBias = 127;
    /// @brief How many mantissa bits a half float carries.
    constexpr std::uint32_t kHalfMantissaBits = 10;
    /// @brief What a half float adds to its exponent to keep it unsigned.
    constexpr std::int32_t kHalfExponentBias = 15;
    /// @brief The exponent a half float uses for infinity and NaN.
    constexpr std::int32_t kHalfExponentMax = 0x1F;
    /// @brief The sign bit of a half, reached by shifting a float down.
    constexpr std::uint32_t kHalfSignShift = 16;
    /// @brief The sign bit of a half, in place.
    constexpr std::uint32_t kHalfSignMask = 0x8000U;
    /// @brief The largest finite half float: exponent 30, every mantissa bit set.
    constexpr std::uint32_t kHalfLargestFinite = 0x7BFFU;
    /// @brief Below this the value cannot be a denormal half either.
    constexpr std::int32_t kHalfDenormalFloor = -10;

    /**
     * @brief Turns a float into a 16-bit half float.
     *
     * This handles the three cases a naive shift gets wrong: a value too large for
     * half, which clamps to the largest finite half rather than becoming infinity,
     * a value too small, which flushes to zero rather than wrapping, and a
     * denormal result, which needs its mantissa shifted by hand.
     *
     * @param value The float to convert.
     * @return The half float as a raw 16-bit word.
     */
    [[nodiscard]] inline std::uint16_t to_half(float value) {
        std::uint32_t bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));

        const std::uint32_t sign = (bits >> kHalfSignShift) & kHalfSignMask;
        const std::int32_t exponent =
            static_cast<std::int32_t>((bits >> kFloatMantissaBits) & kFloatExponentMask) -
            kFloatExponentBias + kHalfExponentBias;
        std::uint32_t mantissa = bits & kFloatMantissaMask;

        if (exponent >= kHalfExponentMax) {
            // Too large for half, or already an infinity or a NaN. Clamp to the
            // largest finite half. An infinity in an environment map would poison
            // every average the mip chain takes, and a NaN would spread over the
            // whole level.
            return static_cast<std::uint16_t>(sign | kHalfLargestFinite);
        }
        if (exponent <= 0) {
            // A denormal half, or a value too small to represent at all.
            if (exponent < kHalfDenormalFloor) {
                return static_cast<std::uint16_t>(sign);
            }
            mantissa |= kFloatImplicitOne;
            const auto shift = static_cast<std::uint32_t>(
                static_cast<std::int32_t>(kFloatMantissaBits - kHalfMantissaBits) + 1 - exponent);
            return static_cast<std::uint16_t>(sign | (mantissa >> shift));
        }
        return static_cast<std::uint16_t>(
            sign | (static_cast<std::uint32_t>(exponent) << kHalfMantissaBits) |
            (mantissa >> (kFloatMantissaBits - kHalfMantissaBits)));
    }

} // namespace engine::import
