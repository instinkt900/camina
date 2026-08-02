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

#include <type_traits>
#include <vector>

namespace engine::reflect {

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
