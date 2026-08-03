#pragma once

/**
 * @file
 * @brief A stable identity for an asset, and its text form.
 *
 * A file path is not an identity. Rename a texture and every material that
 * named it breaks. So a source asset carries a GUID in a sidecar file, and
 * every cooked file and every scene stores that GUID instead of a path.
 *
 * The value is 128 bits, written as a UUID. The text form is what a person
 * reads in a diff, so it matters more than the packed form does.
 *
 * This type lives in core/ rather than assets/, because reflect/ has to
 * serialize one and reflect/ sits below assets/. See DESIGN.md section 6.
 */

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace engine {

    /**
     * @brief A 128-bit identity that survives a rename and a move.
     *
     * The default value is the null GUID, and valid() reports it. A null GUID
     * never names an asset, so a database lookup for one is a miss.
     *
     * @code
     * const engine::Guid id = engine::Guid::generate();
     * const std::string text = id.to_text();
     * @endcode
     */
    struct Guid {
        /// @brief How long the text form is, without a terminator.
        static constexpr std::size_t kTextLength = 36;

        std::uint64_t high = 0; ///< The first eight bytes, most significant first.
        std::uint64_t low = 0;  ///< The last eight bytes, most significant first.

        /// @brief Reports whether the GUID names anything.
        /// @return True when the value is not the null GUID.
        [[nodiscard]] constexpr bool valid() const noexcept { return high != 0 || low != 0; }

        /**
         * @brief Makes a new GUID that no other call returns.
         *
         * The bytes come from the system random source. The version and variant
         * bits follow RFC 4122 version 4, so the text form is a real UUID that
         * other tools accept.
         *
         * @return The new GUID.
         */
        [[nodiscard]] static Guid generate();

        /**
         * @brief Writes the GUID in the 8-4-4-4-12 form, with lowercase digits.
         * @return The text, 36 characters long. A null GUID writes all zeros.
         */
        [[nodiscard]] std::string to_text() const;

        /**
         * @brief Reads a GUID back from its text form.
         *
         * The reader accepts uppercase and lowercase digits. It rejects any
         * other length, a misplaced dash, and a non-hexadecimal digit. It
         * leaves @p out alone when it rejects the text.
         *
         * @param text The text to read.
         * @param out The GUID to fill.
         * @return True when the text was a GUID.
         */
        [[nodiscard]] static bool parse(std::string_view text, Guid& out);

        /// @brief Compares two GUIDs for equality.
        /// @param a The first GUID.
        /// @param b The second GUID.
        /// @return True when both halves match.
        friend constexpr bool operator==(Guid a, Guid b) noexcept {
            return a.high == b.high && a.low == b.low;
        }

        /// @brief Compares two GUIDs for inequality.
        /// @param a The first GUID.
        /// @param b The second GUID.
        /// @return True when either half differs.
        friend constexpr bool operator!=(Guid a, Guid b) noexcept { return !(a == b); }

        /**
         * @brief Orders two GUIDs, so one can key an ordered container.
         * @param a The first GUID.
         * @param b The second GUID.
         * @return True when @p a sorts before @p b.
         */
        friend constexpr bool operator<(Guid a, Guid b) noexcept {
            return a.high != b.high ? a.high < b.high : a.low < b.low;
        }
    };

    /**
     * @brief The text form of a GUID, for reflect/.
     *
     * reflect/traits.h finds this by argument-dependent lookup, which is how a
     * GUID field reaches a file as a string rather than as a nested object.
     * The reflection layer therefore never names this type.
     *
     * @param value The GUID to write.
     * @return The text form.
     */
    [[nodiscard]] inline std::string to_text(const Guid& value) { return value.to_text(); }

    /**
     * @brief Reads a GUID back from text, for reflect/.
     * @param text The text to read.
     * @param out The GUID to fill. It stays as it was when the text is bad.
     * @return True when the text was a GUID.
     */
    [[nodiscard]] inline bool from_text(std::string_view text, Guid& out) {
        return Guid::parse(text, out);
    }

} // namespace engine

/// @cond
// Hash support, so a GUID can key an unordered container. Doxygen 1.9.8 cannot
// resolve a specialization of a std template, in the same way as Handle in
// core/handle.h, so this is hidden from the docs rather than documented.
template <>
struct std::hash<engine::Guid> {
    std::size_t operator()(const engine::Guid& guid) const noexcept {
        // A GUID already holds random bits, so a cheap mix is enough. The
        // rotate keeps two GUIDs that hold the same two halves the other way
        // around from landing in the same bucket.
        constexpr unsigned kHalfWidth = 32U;
        const std::uint64_t rotated = (guid.low << kHalfWidth) | (guid.low >> kHalfWidth);
        return static_cast<std::size_t>(guid.high ^ rotated);
    }
};
/// @endcond
