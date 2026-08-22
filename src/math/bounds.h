#pragma once

/**
 * @file
 * @brief An axis-aligned box, and the world-space sphere that holds it.
 *
 * A cooked mesh carries its bounds in its own local space. A cull test needs
 * them where the camera is, which means putting the box through the world
 * matrix of the entity. Nothing here opens a device or names a Vulkan type, so
 * a test can drive it with no GPU.
 *
 * That matters for the same reason it matters in frustum.h. A radius that comes
 * out too small drops a mesh that is on screen, and the hole appears at the edge
 * of the view where nobody is looking for it.
 */

#include "math/conventions.h"

#include <algorithm>

namespace engine {

    /**
     * @brief A sphere, as a center and a radius.
     *
     * The shape frustum_contains_sphere() takes. A sphere rather than a box
     * because one plane test is a dot product and a compare, and an oriented box
     * needs eight.
     */
    struct Sphere {
        Vec3 center{ 0.0F, 0.0F, 0.0F }; ///< Where it sits, in world space.
        float radius = 0.0F;             ///< How big it is, in meters.
    };

    /**
     * @brief A box in world space, as a center and three half axes.
     *
     * The axes carry both direction and length, so a rotated or scaled entity
     * needs no separate rotation. The box is the set of points
     * `center + a*axis_x + b*axis_y + c*axis_z` for each of a, b and c in
     * [-1, 1].
     *
     * This is the tight bound. Sphere is the loose one, and it is still the
     * right answer where a test runs often and the meshes are small. See
     * ::frustum_contains_box for when the difference is worth paying for.
     */
    struct Obb {
        Vec3 center{ 0.0F, 0.0F, 0.0F }; ///< Where it sits, in world space.
        Vec3 axis_x{ 0.0F, 0.0F, 0.0F }; ///< Half extent along local X, in world space.
        Vec3 axis_y{ 0.0F, 0.0F, 0.0F }; ///< Half extent along local Y, in world space.
        Vec3 axis_z{ 0.0F, 0.0F, 0.0F }; ///< Half extent along local Z, in world space.
    };

    /**
     * @brief The local box of a mesh put into world space, without growing it.
     *
     * The same inputs ::world_sphere_from_bounds takes, and the tight answer
     * rather than the loose one. A long thin mesh gets a sphere far larger than
     * itself, and that sphere keeps the mesh in every volume it comes near.
     *
     * @param world_from_local The transform of the entity, translation included.
     * @param min The smallest corner of the box, in local space.
     * @param max The largest corner of the box, in local space.
     * @return The box, in the space @p world_from_local maps into.
     */
    [[nodiscard]] inline Obb world_box_from_bounds(const Mat4& world_from_local, const Vec3& min,
                                                   const Vec3& max) {
        const Vec3 local_center = (min + max) * 0.5F;
        const Vec3 half_extent = (max - min) * 0.5F;
        return Obb{
            .center = Vec3{ world_from_local * Vec4{ local_center, 1.0F } },
            // Each column of the linear part, scaled by the half extent along
            // its own axis. That is the same decomposition the sphere above
            // sums over, kept apart rather than folded into one length.
            .axis_x = Vec3{ world_from_local[0] } * half_extent.x,
            .axis_y = Vec3{ world_from_local[1] } * half_extent.y,
            .axis_z = Vec3{ world_from_local[2] } * half_extent.z,
        };
    }

    /**
     * @brief The smallest sphere around a local box put into world space.
     *
     * The box arrives as two opposite corners in the local space of a mesh. This
     * turns it into the sphere that holds the transformed box, centered on the
     * transformed center.
     *
     * The radius is exact for the transformed box rather than an upper bound on
     * it. Each corner offset is the linear part of the matrix applied to a
     * signed combination of the half extents, and opposite corners give offsets
     * of the same length. So four of the eight corners settle it, and the
     * longest of those four is the radius.
     *
     * @warning The cheap form of this is wrong in the dangerous direction.
     * Scaling the local half diagonal by the longest matrix column looks like a
     * safe upper bound and is not. Three columns that lean the same way have a
     * signed sum longer than any one of them, which is the largest singular
     * value exceeding every column length. An underestimate drops a mesh that
     * is in view, and the hole appears at the edge of the frame.
     *
     * @param world_from_local The transform of the entity, translation included.
     * @param min The smallest corner of the box, in local space.
     * @param max The largest corner of the box, in local space.
     * @return The sphere, in the space @p world_from_local maps into.
     *
     * @code
     * const engine::Sphere sphere =
     *     engine::world_sphere_from_bounds(transform.matrix, mesh.min, mesh.max);
     * if (!engine::frustum_contains_sphere(frustum, sphere.center, sphere.radius)) {
     *     return; // Off screen. Issue no draw.
     * }
     * @endcode
     */
    [[nodiscard]] inline Sphere world_sphere_from_bounds(const Mat4& world_from_local,
                                                         const Vec3& min, const Vec3& max) {
        const Vec3 local_center = (min + max) * 0.5F;
        const Vec3 half_extent = (max - min) * 0.5F;

        // The three columns of the linear part, each already scaled by the half
        // extent along its own axis. A corner offset is then a signed sum of
        // these three vectors.
        const Vec3 axis_x = Vec3{ world_from_local[0] } * half_extent.x;
        const Vec3 axis_y = Vec3{ world_from_local[1] } * half_extent.y;
        const Vec3 axis_z = Vec3{ world_from_local[2] } * half_extent.z;

        // Four of the eight sign combinations. The other four are these negated,
        // and a negated vector has the same length. Compared as squared lengths,
        // so the square root runs once rather than four times.
        const auto length_squared = [](const Vec3& v) { return glm::dot(v, v); };
        const float longest = std::max({
            length_squared(axis_x + axis_y + axis_z),
            length_squared(axis_x + axis_y - axis_z),
            length_squared(axis_x - axis_y + axis_z),
            length_squared(axis_x - axis_y - axis_z),
        });

        return Sphere{ .center = Vec3{ world_from_local * Vec4{ local_center, 1.0F } },
                       .radius = std::sqrt(longest) };
    }

} // namespace engine
