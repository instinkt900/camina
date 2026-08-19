// Tests for to_half, the float-to-half-float converter that the environment
// rule and the BRDF rule both use. The function handles three cases a naive
// shift gets wrong, so these drive each one explicitly.

#include "import/to_half.h"

#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>

namespace {

    void check(bool condition, const char* message) {
        if (!condition) {
            std::cerr << "FAILED: " << message << "\n";
            std::exit(1);
        }
    }

    void test_one_point_oh() {
        const std::uint16_t result = engine::import::to_half(1.0F);
        // 1.0f in IEEE 754 half is 0x3C00.
        check(result == 0x3C00U, "1.0");
    }

    void test_zero_point_five() {
        const std::uint16_t result = engine::import::to_half(0.5F);
        // 0.5f in IEEE 754 half is 0x3800.
        check(result == 0x3800U, "0.5");
    }

    void test_zero() {
        const std::uint16_t result = engine::import::to_half(0.0F);
        check(result == 0U, "zero gives zero");
    }

    void test_negative_zero() {
        const std::uint16_t result = engine::import::to_half(-0.0F);
        // Negative zero has the sign bit set.
        check(result == 0x8000U, "negative zero carries the sign bit");
    }

    void test_largest_finite() {
        // The largest finite half holds 65504.0.
        const std::uint16_t expected = 0x7BFFU;
        const std::uint16_t result = engine::import::to_half(65504.0F);
        check(result == expected, "largest finite half fits");
    }

    void test_clamps_past_largest() {
        // 66000 is past what a half can hold. It must clamp to the largest
        // finite half rather than become an infinity.
        const std::uint16_t result = engine::import::to_half(66000.0F);
        check(result == 0x7BFFU, "a value too large clamps rather than becoming infinity");
    }

    void test_infinity_clamps() {
        const std::uint16_t result =
            engine::import::to_half(std::numeric_limits<float>::infinity());
        check(result == 0x7BFFU, "an infinity clamps to the largest finite half");
    }

    void test_nan_clamps() {
        const std::uint16_t result =
            engine::import::to_half(std::numeric_limits<float>::quiet_NaN());
        // Any NaN bit pattern clamps to the largest finite half.
        check(result == 0x7BFFU, "a NaN clamps to the largest finite half");
    }

    void test_denormal() {
        // 2^{-14} * (1 + 1/1024) is the largest denormal half, about 6.10e-5.
        const float smallest_normal = 6.103515625e-5F;
        const float denormal = smallest_normal * 0.5F;
        const std::uint16_t result = engine::import::to_half(denormal);
        // A denormal must not flush to zero.
        check(result != 0U && (result & 0x7FFFU) != 0U, "a denormal does not flush to zero");
        // The sign bits stay clear.
        check((result & 0x8000U) == 0U, "a positive denormal has no sign bit");
    }

    void test_tiny_value_flushes() {
        // A value below what a denormal half can hold.
        const float tiny = 1.0e-10F;
        const std::uint16_t result = engine::import::to_half(tiny);
        check(result == 0U, "a value too small flushes to zero");
    }

    void test_negative() {
        const std::uint16_t result = engine::import::to_half(-1.0F);
        // -1.0 in IEEE 754 half is 0xBC00.
        check(result == 0xBC00U, "negative one carries the sign bit");
    }

    void test_mantissa_truncation() {
        // 1.0 + 2^{-10} is the smallest half increment above 1.0. The
        // converter truncates the extra float bits rather than rounding.
        const std::uint16_t result = engine::import::to_half(1.0F + (1.0F / 1024.0F));
        // The half mantissa bit 0 is set, so the result is 0x3C01 and not
        // the rounded-down 0x3C00.
        check(result == 0x3C01U, "the mantissa truncates (no rounding)");
    }

} // namespace

int main() {
    test_one_point_oh();
    test_zero_point_five();
    test_zero();
    test_negative_zero();
    test_largest_finite();
    test_clamps_past_largest();
    test_infinity_clamps();
    test_nan_clamps();
    test_denormal();
    test_tiny_value_flushes();
    test_negative();
    test_mantissa_truncation();

    std::cout << "All good.\n";
    return 0;
}
