#pragma once

/**
 * @file
 * @brief Type traits that both reflection consumers need.
 *
 * The inspector and the serializer each have to tell a plain number from a glm
 * vector, a quaternion, or a list. Both need the same answer, so the test lives
 * here once. Rule 4.5 says reflect once and consume many times, and that applies
 * to the support code as well.
 */

#include "math/conventions.h"

#include <concepts>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace engine::reflect {

    /**
     * @brief A value that reads and writes as one piece of text.
     *
     * A type opts in by declaring two free functions next to itself:
     *
     * @code
     * std::string to_text(const Guid& value);
     * bool from_text(std::string_view text, Guid& out);
     * @endcode
     *
     * Argument-dependent lookup finds them, so reflect/ names no such type and
     * gains no dependency on the layer that defines one. The serializer then
     * writes a string instead of a nested object, and the inspector draws a
     * text box.
     *
     * `from_text` reports a text it cannot read, and leaves @p out as it was.
     * That is what lets both consumers keep the old value rather than accept a
     * half-typed one.
     *
     * @tparam T The type to test.
     */
    template <typename T>
    concept TextValue = requires(const T& value, std::string_view text, T& out) {
        { to_text(value) } -> std::convertible_to<std::string>;
        { from_text(text, out) } -> std::same_as<bool>;
    };

    /**
     * @brief Whether a type is a glm vector, and how long it is.
     *
     * The primary template answers no. The specialization answers yes and adds
     * the length and the element type.
     *
     * @tparam T The type to test.
     */
    template <typename T>
    struct GlmVector : std::false_type {};

    /// @cond
    // Doxygen documents the primary template above. The specialization repeats
    // the same meaning with deduced parameters and needs no second entry.
    template <glm::length_t L, typename T, glm::qualifier Q>
    struct GlmVector<glm::vec<L, T, Q>> : std::true_type {
        static constexpr glm::length_t length = L;
        using Element = T;
    };
    /// @endcond

    /**
     * @brief Whether a type is a glm quaternion.
     *
     * A quaternion is not a vector to either consumer. It reads and writes in
     * wxyz order, per DESIGN.md section 3.
     *
     * @tparam T The type to test.
     */
    template <typename T>
    struct GlmQuat : std::false_type {};

    /// @cond
    template <typename T, glm::qualifier Q>
    struct GlmQuat<glm::qua<T, Q>> : std::true_type {
        using Element = T;
    };
    /// @endcond

    /**
     * @brief Whether a type is a std::vector, and what it holds.
     * @tparam T The type to test.
     */
    template <typename T>
    struct StdVector : std::false_type {};

    /// @cond
    template <typename T, typename A>
    struct StdVector<std::vector<T, A>> : std::true_type {
        using Element = T;
    };
    /// @endcond

} // namespace engine::reflect
