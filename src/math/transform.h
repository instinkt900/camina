#pragma once

/**
 * @file
 * @brief A position, a rotation, and a scale, and the matrix they compose to.
 *
 * This is the form a person edits and a file stores. A matrix is the form the
 * GPU reads. Keeping both, and converting one way only, avoids the drift that
 * comes from decomposing a matrix back into its parts.
 *
 * The conventions in DESIGN.md section 3 apply. Meters, radians, right-handed,
 * +Y up, and quaternions in wxyz order.
 */

#include "math/conventions.h"
#include "reflect/attributes.h"
#include "reflect/reflect.h"

#include <tuple>

namespace engine {

    /**
     * @brief A local transform, in the order scale, then rotate, then translate.
     *
     * The default is the identity. A new entity therefore sits at the origin,
     * unrotated, at its natural size.
     */
    struct Transform {
        /// @brief Position in meters, in the parent space.
        Vec3 position{ 0.0F, 0.0F, 0.0F };
        /// @brief Rotation, in wxyz order. The identity is `{ 1, 0, 0, 0 }`.
        Quat rotation{ 1.0F, 0.0F, 0.0F, 0.0F };
        /// @brief Scale on each axis. One is natural size.
        Vec3 scale{ 1.0F, 1.0F, 1.0F };
    };

    /**
     * @brief Composes a transform into a matrix.
     *
     * The order is translate, then rotate, then scale, read right to left. A
     * point therefore scales first and moves last, which is what an author
     * expects when they set all three.
     *
     * @param transform The parts to compose.
     * @return The matrix, column-major, per DESIGN.md section 3.
     */
    [[nodiscard]] inline Mat4 to_matrix(const Transform& transform) {
        Mat4 result = glm::translate(Mat4{ 1.0F }, transform.position);
        result *= glm::mat4_cast(transform.rotation);
        return glm::scale(result, transform.scale);
    }

} // namespace engine

/**
 * @brief Describes Transform for the inspector and for scene files.
 *
 * The description sits next to the type, so there is one place to change when a
 * field arrives. See DESIGN.md section 7.
 */
template <>
struct engine::reflect::Describe<engine::Transform> {
    static constexpr const char* name = "Transform"; ///< The name a scene file stores.
    /// @brief The fields, in the order an editor shows them.
    /// @return A tuple of field descriptors.
    static constexpr auto fields() {
        return std::make_tuple(
            ENGINE_FIELD(engine::Transform, position, Tooltip{ "Meters, in parent space" }),
            ENGINE_FIELD(engine::Transform, rotation, Tooltip{ "Quaternion, wxyz order" }),
            ENGINE_FIELD(engine::Transform, scale));
    }
};
