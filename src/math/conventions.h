#pragma once

/**
 * @file
 * @brief Math types and the engine coordinate conventions.
 *
 * DESIGN.md section 3 holds the rationale. In short:
 *
 * - Right-handed, +Y up, -Z forward. This matches glTF, so import needs no
 *   conversion.
 * - Meters, kilograms, seconds.
 * - Column-major matrices.
 * - Reverse-Z depth. Near is 1 and far is 0.
 * - Quaternions in wxyz storage order.
 * - Linear color working space. Convert sRGB at texture read and at final write
 *   only.
 *
 * The build sets the GLM defines below. See src/CMakeLists.txt. The checks here
 * fail the build if a target forgets them, because a mismatch produces inverted
 * geometry that is hard to trace back to its cause.
 */

#if !defined(GLM_FORCE_DEPTH_ZERO_TO_ONE)
#error "GLM_FORCE_DEPTH_ZERO_TO_ONE is not defined. Link engine::core. See DESIGN.md section 3."
#endif
#if !defined(GLM_FORCE_RADIANS)
#error "GLM_FORCE_RADIANS is not defined. Link engine::core. See DESIGN.md section 3."
#endif
#if !defined(GLM_FORCE_QUAT_DATA_WXYZ)
#error "GLM_FORCE_QUAT_DATA_WXYZ is not defined. Link engine::core. See DESIGN.md section 3."
#endif

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cmath>

namespace engine {

    /// @brief Two-component float vector.
    using Vec2 = glm::vec2;
    /// @brief Three-component float vector.
    using Vec3 = glm::vec3;
    /// @brief Four-component float vector.
    using Vec4 = glm::vec4;
    /// @brief Two-component signed integer vector.
    using IVec2 = glm::ivec2;
    /// @brief Three-component signed integer vector.
    using IVec3 = glm::ivec3;
    /// @brief Two-component unsigned integer vector.
    using UVec2 = glm::uvec2;
    /// @brief 3x3 column-major float matrix.
    using Mat3 = glm::mat3;
    /// @brief 4x4 column-major float matrix.
    using Mat4 = glm::mat4;
    /// @brief Rotation quaternion in wxyz storage order.
    using Quat = glm::quat;

    /// @brief The world right axis, +X. Read this instead of writing a literal.
    inline constexpr Vec3 world_right{ 1.0F, 0.0F, 0.0F };
    /// @brief The world up axis, +Y. Read this instead of writing a literal.
    inline constexpr Vec3 world_up{ 0.0F, 1.0F, 0.0F };
    /// @brief The world forward axis, -Z. Read this instead of writing a literal.
    inline constexpr Vec3 world_forward{ 0.0F, 0.0F, -1.0F };

    /**
     * @brief Builds an infinite reverse-Z projection matrix for Vulkan clip space.
     *
     * The matrix maps the near plane to depth 1 and infinity to depth 0. Use it
     * with a float depth buffer and a GREATER depth compare. This layout puts the
     * floating point precision where the geometry is, which removes almost all
     * z-fighting in the distance.
     *
     * The negated [1][1] term flips Y, because Vulkan clip space points down while
     * OpenGL points up.
     *
     * @param fov_y_radians Vertical field of view, in radians.
     * @param aspect Width divided by height.
     * @param z_near Distance to the near plane. There is no far plane.
     * @return The projection matrix.
     */
    inline Mat4 perspective_reverse_z(float fov_y_radians, float aspect, float z_near) {
        const float focal = 1.0F / std::tan(fov_y_radians * 0.5F);

        Mat4 result(0.0F);
        result[0][0] = focal / aspect;
        result[1][1] = -focal;
        result[2][2] = 0.0F;
        result[2][3] = -1.0F;
        result[3][2] = z_near;
        return result;
    }

    /**
     * @brief Builds a reverse-Z orthographic projection for Vulkan clip space.
     *
     * A directional light has no position, so its shadow map is orthographic.
     * This maps @p z_near to depth 1 and @p z_far to depth 0, which is the same
     * sense perspective_reverse_z() uses, so one depth compare serves both.
     *
     * The light looks down its own -Z, as a camera does. So a caller passes
     * distances in front of the light, and both grow away from it.
     *
     * The negated [1][1] term flips Y for the same reason as the perspective
     * form.
     *
     * @param half_width Half the volume across, in world units.
     * @param half_height Half the volume up, in world units.
     * @param z_near Distance to the near plane, which maps to depth 1.
     * @param z_far Distance to the far plane, which maps to depth 0.
     * @return The projection matrix.
     *
     * @warning @p z_far must be greater than @p z_near. An equal pair would
     * divide by zero and fill the map with infinities.
     */
    inline Mat4 orthographic_reverse_z(float half_width, float half_height, float z_near,
                                       float z_far) {
        const float depth = z_far - z_near;

        Mat4 result(1.0F);
        result[0][0] = 1.0F / half_width;
        result[1][1] = -1.0F / half_height;
        // The light looks down -Z, so a point in front of it has a negative z in
        // light space. Both terms carry that sign, which is what puts the near
        // plane at 1 and the far plane at 0 rather than the other way round.
        result[2][2] = 1.0F / depth;
        result[3][2] = z_far / depth;
        return result;
    }

    /**
     * @brief The depth compare sense that matches the reverse-Z convention.
     *
     * True means the test keeps the larger depth value. The render backend
     * translates this into its own enum.
     */
    inline constexpr bool depth_compare_is_greater = true;

} // namespace engine
