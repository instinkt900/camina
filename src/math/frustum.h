#pragma once

/**
 * @file
 * @brief The six planes of a view frustum, and what they contain.
 *
 * Nothing here opens a device or names a Vulkan type, so a test can drive it
 * with no GPU. That matters because a sign error in a plane culls the whole
 * scene or culls nothing, and both look like a bug somewhere else entirely.
 */

#include "math/conventions.h"

#include <array>
#include <cmath>
#include <cstddef>

namespace engine {

    /**
     * @brief One plane, as the coefficients of `ax + by + cz + d = 0`.
     *
     * The normal points into the volume the frustum keeps, so a point is inside
     * when `dot(normal, point) + distance` is not negative. Storing it this way
     * rather than as a point and a normal is what makes the test one dot
     * product.
     */
    struct Plane {
        Vec3 normal{ 0.0F, 0.0F, 0.0F }; ///< Unit length, pointing inward.
        float distance = 0.0F;           ///< The signed distance from the origin.

        /**
         * @brief How far a point sits from the plane, along the normal.
         * @param point The point to measure, in the same space as the plane.
         * @return Positive inside, negative outside, and zero exactly on it.
         */
        [[nodiscard]] float signed_distance(const Vec3& point) const {
            return glm::dot(normal, point) + distance;
        }
    };

    /// @brief How many planes bound a frustum.
    inline constexpr std::size_t kFrustumPlanes = 6;

    /**
     * @brief The six bounding planes of a view frustum, all pointing inward.
     *
     * The order is left, right, bottom, top, near, far, and it carries no
     * meaning. Every reader iterates all six, so the labels are for somebody
     * reading the extraction and nothing else. Two entries swapped changes no
     * answer, which a mutation test confirmed.
     */
    struct Frustum {
        std::array<Plane, kFrustumPlanes> planes{}; ///< All six, normals inward.
    };

    /**
     * @brief Pulls the six planes out of a view projection matrix.
     *
     * Each plane is a sum or a difference of two rows of the matrix, which is
     * the Gribb and Hartmann method. It works for any projection, including the
     * infinite reverse-Z one this engine uses, because it reads the matrix
     * rather than assuming how the matrix was built.
     *
     * @warning Reverse-Z is what makes the depth pair surprising. Depth runs
     * from 1 at the near plane to 0 at infinity, so `w - z` is the near plane
     * here and `z` is the far one. A conventional 0 to 1 range wants those the
     * other way round, and writing `z` for the near plane is therefore the
     * mistake to expect. It does not swap the pair. It drops the near plane
     * entirely, because the far one is already degenerate under an infinite
     * projection. The frustum then has no depth bound at all, and it keeps a
     * light behind the camera.
     *
     * The planes come out normalized, so signed_distance() is a real distance in
     * world units and a sphere radius can be compared against it directly.
     *
     * @param clip_from_world The camera matrix, with no model transform in it.
     * @return The six planes, in world space.
     */
    [[nodiscard]] inline Frustum frustum_from_view_projection(const Mat4& clip_from_world) {
        // Row-major access out of a column-major matrix: row i is the i-th
        // element of each column. Writing it out once here keeps the sums below
        // readable.
        const auto row = [&clip_from_world](int i) {
            return Vec4{ clip_from_world[0][i], clip_from_world[1][i], clip_from_world[2][i],
                         clip_from_world[3][i] };
        };
        const Vec4 x = row(0);
        const Vec4 y = row(1);
        const Vec4 z = row(2);
        const Vec4 w = row(3);

        // Clip space keeps -w <= x <= w and -w <= y <= w, so each side plane is
        // w plus or minus the matching row. Depth keeps 0 <= z <= w, so the two
        // depth planes are z itself and w minus z.
        //
        // Under reverse-Z, z = w at the near plane and z = 0 at infinity. So
        // "w - z" is the near plane and "z" is the far one, which is the
        // opposite of the conventional assignment. See the warning above.
        const std::array<Vec4, kFrustumPlanes> raw{ {
            w + x, // left
            w - x, // right
            w + y, // bottom
            w - y, // top
            w - z, // near
            z,     // far
        } };

        Frustum out;
        for (std::size_t i = 0; i < kFrustumPlanes; ++i) {
            const Vec3 normal{ raw[i] };
            const float length = glm::length(normal);
            if (length > 0.0F) {
                out.planes[i] = Plane{ normal / length, raw[i].w / length };
            } else {
                // A degenerate plane, which an infinite projection produces for
                // the far one: every coefficient of the z row is zero there. A
                // plane that bounds nothing must not cull anything, and a zero
                // normal with a positive distance keeps every point inside.
                out.planes[i] = Plane{ Vec3{ 0.0F, 0.0F, 0.0F }, 1.0F };
            }
        }
        return out;
    }

    /**
     * @brief Whether a sphere touches or sits inside a frustum.
     *
     * This is the conservative test. A sphere that crosses the corner between
     * two planes can be outside the frustum and still pass, because it is
     * outside neither plane by more than its radius. That answer is wrong in the
     * cheap direction: it keeps something it could have dropped, and it never
     * drops something it should have kept.
     *
     * @param frustum The planes, from frustum_from_view_projection().
     * @param center Where the sphere is, in the same space as the planes.
     * @param radius How big it is. A radius of zero tests the point.
     * @return False only when the sphere is wholly outside one plane.
     */
    [[nodiscard]] inline bool frustum_contains_sphere(const Frustum& frustum, const Vec3& center,
                                                      float radius) {
        for (const Plane& plane : frustum.planes) {
            if (plane.signed_distance(center) < -radius) {
                return false;
            }
        }
        return true;
    }

    /**
     * @brief Whether any part of an oriented box is inside the frustum.
     *
     * The tight companion to ::frustum_contains_sphere. The box arrives as a
     * center and three half axes, which is what ::world_box_from_bounds
     * returns, and the parameters are loose rather than a struct for the same
     * reason the sphere test takes a center and a radius.
     *
     * For each plane it projects the box onto the plane normal. The projection
     * reaches `|n.x| + |n.y| + |n.z|` from the center, summing the three axes,
     * and the box is outside when the center sits further out than that. This
     * is the same "wrong in the cheap direction" test the sphere gets: a box
     * that straddles two planes without crossing the volume is kept.
     *
     * **Prefer the sphere unless the meshes are long.** This costs three dot
     * products for each plane rather than one, and a sphere around a compact
     * mesh is barely larger than the mesh. The difference is worth paying for
     * when one entity spans a large part of the world, because then the sphere
     * reaches into every volume nearby and rejects nothing.
     *
     * @param frustum The planes, from frustum_from_view_projection().
     * @param center Where the box is, in the same space as the planes.
     * @param axis_x The half extent along local X, as a world-space vector.
     * @param axis_y The half extent along local Y, as a world-space vector.
     * @param axis_z The half extent along local Z, as a world-space vector.
     * @return False only when the box is wholly outside one plane.
     */
    [[nodiscard]] inline bool frustum_contains_box(const Frustum& frustum, const Vec3& center,
                                                   const Vec3& axis_x, const Vec3& axis_y,
                                                   const Vec3& axis_z) {
        for (const Plane& plane : frustum.planes) {
            const float reach = std::abs(glm::dot(plane.normal, axis_x)) +
                                std::abs(glm::dot(plane.normal, axis_y)) +
                                std::abs(glm::dot(plane.normal, axis_z));
            if (plane.signed_distance(center) < -reach) {
                return false;
            }
        }
        return true;
    }

} // namespace engine
