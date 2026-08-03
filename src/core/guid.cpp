#include "core/guid.h"

#include <array>
#include <cstddef>
#include <random>

namespace engine {

    namespace {

        /// Where the dashes sit in the 8-4-4-4-12 form.
        constexpr std::array<std::size_t, 4> kDashPositions{ 8, 13, 18, 23 };

        constexpr std::size_t kHexDigitsPerHalf = 16;
        constexpr unsigned kBitsPerHexDigit = 4;
        constexpr std::uint64_t kHexDigitMask = 0xF;

        /// Turns one hexadecimal digit into its value, or reports a bad digit.
        [[nodiscard]] bool hex_value(char digit, std::uint64_t& out) {
            constexpr int kDecadeOffset = 10;
            if (digit >= '0' && digit <= '9') {
                out = static_cast<std::uint64_t>(digit - '0');
                return true;
            }
            if (digit >= 'a' && digit <= 'f') {
                out = static_cast<std::uint64_t>(digit - 'a' + kDecadeOffset);
                return true;
            }
            if (digit >= 'A' && digit <= 'F') {
                out = static_cast<std::uint64_t>(digit - 'A' + kDecadeOffset);
                return true;
            }
            return false;
        }

        /// Appends one 64-bit half as 16 lowercase hexadecimal digits.
        void write_half(std::string& out, std::uint64_t half) {
            constexpr std::string_view kDigits = "0123456789abcdef";
            for (std::size_t i = 0; i < kHexDigitsPerHalf; ++i) {
                const unsigned shift =
                    static_cast<unsigned>(kHexDigitsPerHalf - 1 - i) * kBitsPerHexDigit;
                out.push_back(kDigits[(half >> shift) & kHexDigitMask]);
            }
        }

        /// Whether a position in the text form holds a dash rather than a digit.
        [[nodiscard]] bool is_dash_position(std::size_t index) {
            for (const std::size_t dash : kDashPositions) {
                if (dash == index) {
                    return true;
                }
            }
            return false;
        }

    } // namespace

    Guid Guid::generate() {
        // Seeded once for each thread. std::random_device is slow on some
        // platforms, and a cook run makes one GUID for every new source file.
        static thread_local std::mt19937_64 engine_state{ std::random_device{}() };
        std::uniform_int_distribution<std::uint64_t> bits;

        Guid guid{ .high = bits(engine_state), .low = bits(engine_state) };

        // RFC 4122 version 4 sits in the top nibble of the third group, and the
        // variant sits in the top two bits of the fourth. Setting them costs
        // six bits of randomness and makes the text a UUID other tools accept.
        constexpr std::uint64_t kVersionMask = 0xF000;
        constexpr std::uint64_t kVersionFour = 0x4000;
        constexpr std::uint64_t kVariantMask = 0xC000000000000000ULL;
        constexpr std::uint64_t kVariantRfc4122 = 0x8000000000000000ULL;

        guid.high = (guid.high & ~kVersionMask) | kVersionFour;
        guid.low = (guid.low & ~kVariantMask) | kVariantRfc4122;
        return guid;
    }

    std::string Guid::to_text() const {
        std::string packed;
        packed.reserve(kHexDigitsPerHalf * 2);
        write_half(packed, high);
        write_half(packed, low);

        std::string text;
        text.reserve(kTextLength);
        for (const char digit : packed) {
            if (is_dash_position(text.size())) {
                text.push_back('-');
            }
            text.push_back(digit);
        }
        return text;
    }

    bool Guid::parse(std::string_view text, Guid& out) {
        if (text.size() != kTextLength) {
            return false;
        }

        // Read into a local and assign at the end, so a rejected text leaves
        // the caller's GUID as it was.
        Guid parsed;
        std::size_t digits_read = 0;

        for (std::size_t i = 0; i < text.size(); ++i) {
            if (is_dash_position(i)) {
                if (text[i] != '-') {
                    return false;
                }
                continue;
            }

            std::uint64_t value = 0;
            if (!hex_value(text[i], value)) {
                return false;
            }

            std::uint64_t& half = digits_read < kHexDigitsPerHalf ? parsed.high : parsed.low;
            half = (half << kBitsPerHexDigit) | value;
            ++digits_read;
        }

        out = parsed;
        return true;
    }

} // namespace engine
