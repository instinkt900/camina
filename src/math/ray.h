#pragma once

/**
 * @file
 * @brief A ray, the box test it needs, and the way one comes out of a screen.
 *
 * Picking an entity by clicking it starts here. Nothing in this file opens a
 * device or names a Vulkan type, so a test drives all of it with no GPU, which
 * matters because every mistake available here is a silent one: a ray that
 * points backwards picks nothing, and a box test that is off by a sign picks the
 * wrong thing.
 */

#include "math/conventions.h"

#include <algorithm>
#include <limits>

namespace engine {

    /// @brief A half line, as a point and a direction.
    struct Ray {
        Vec3 origin{ 0.0F, 0.0F, 0.0F };     ///< Where it starts, in world space.
        Vec3 direction{ 0.0F, 0.0F, -1.0F }; ///< Which way it goes. A unit vector.
    };

    /**
     * @brief Where a ray enters an axis-aligned box, if it does.
     *
     * The slab test: clip the ray against each pair of parallel faces and keep
     * the overlap.
     *
     * **The distance is to the first surface ahead of the origin, which is the
     * far side when the origin is inside the box.** A picker in a room is
     * inside the bounding box of that room, and a hit at zero would beat every
     * object in it: clicking anything would select the room. Reporting where the
     * ray leaves instead puts the room behind its own contents, which is where a
     * person sees it.
     *
     * **This is an oriented box test as well.** Put the ray into the local space
     * of an entity first and the local bounds become an oriented box in world
     * space for free, which is what a picker wants: a sphere around a long thin
     * wall is mostly empty space, and clicking that space would select the wall.
     *
     * @param ray The ray to test. The direction need not be normalized, but the
     * distance comes back in units of that direction when it is not. A very
     * small component gives a very large distance rather than a miss, which is
     * the honest answer: a caller that cares about how far away a hit is
     * compares it, and a picker's nearest-wins search discards it for free.
     * @param min The smallest corner of the box.
     * @param max The largest corner of the box.
     * @param out_distance Receives how far along the ray the hit is. Untouched
     * when there is no hit.
     * @return True when the ray meets the box ahead of its origin.
     */
    [[nodiscard]] inline bool ray_hits_box(const Ray& ray, const Vec3& min, const Vec3& max,
                                           float& out_distance) {
        // Not clamped at zero. A negative entry means the origin is past that
        // pair of faces already, and the exit below is then the surface ahead.
        float nearest = std::numeric_limits<float>::lowest();
        float furthest = std::numeric_limits<float>::max();

        for (glm::length_t axis = 0; axis < 3; ++axis) {
            const float direction = ray.direction[axis];
            const float origin = ray.origin[axis];

            if (direction == 0.0F) {
                // Exactly parallel to this pair of faces. It misses unless it
                // started between them, and zero divided by zero would give a
                // NaN, which compares false against everything and would read
                // as a hit.
                //
                // **Only exactly zero.** A threshold here reports a miss for a
                // direction that is merely small, and a ray transformed into the
                // local space of a scaled entity can carry small components.
                // Every other value divides to a finite number or to an
                // infinity, and the compares below are correct for both.
                if (origin < min[axis] || origin > max[axis]) {
                    return false;
                }
                continue;
            }

            const float inverse = 1.0F / direction;
            float enter = (min[axis] - origin) * inverse;
            float leave = (max[axis] - origin) * inverse;
            if (enter > leave) {
                std::swap(enter, leave);
            }

            nearest = std::max(nearest, enter);
            furthest = std::min(furthest, leave);
            if (nearest > furthest) {
                return false;
            }
        }

        if (furthest < 0.0F) {
            // The whole box is behind the origin.
            return false;
        }

        // Inside the box gives a negative entry, and then the surface ahead is
        // the one the ray leaves by.
        out_distance = nearest >= 0.0F ? nearest : furthest;
        return true;
    }

    /**
     * @brief Turns a pixel inside a rectangle into normalized device coordinates.
     *
     * The rectangle is where the picture was drawn, in the same coordinates the
     * pixel is in. The answer runs from -1 to 1 across it, and **-1 is the top**
     * because Vulkan's Y runs down and the projection already accounts for it.
     * So there is no flip here, and adding one is the mistake this exists to
     * stop repeating.
     *
     * @param pixel_x Where the pointer is, across.
     * @param pixel_y Where the pointer is, down.
     * @param x Left edge of the picture.
     * @param y Top edge of the picture.
     * @param width Width of the picture. A value at or below zero gives 0.
     * @param height Height of the picture. A value at or below zero gives 0.
     * @return The point, in normalized device coordinates.
     */
    [[nodiscard]] inline Vec2 ndc_from_pixel(float pixel_x, float pixel_y, float x, float y,
                                             float width, float height) {
        return Vec2{ width > 0.0F ? (2.0F * (pixel_x - x) / width) - 1.0F : 0.0F,
                     height > 0.0F ? (2.0F * (pixel_y - y) / height) - 1.0F : 0.0F };
    }

    /**
     * @brief The ray through a point on the screen.
     *
     * The point arrives in normalized device coordinates, which run from -1 to
     * 1 across the picture. **Y follows Vulkan, so -1 is the top**, per
     * `math/conventions.h`. A caller with a pixel and a rectangle gets there
     * with `2 * (pixel - corner) / size - 1` and no flip.
     *
     * The near plane is depth 1 under reverse-Z, and there is no far plane to
     * unproject against, so the direction comes from the camera position rather
     * than from a second point. That is exact for a perspective camera and it
     * is the reason this takes the position separately.
     *
     * @param world_from_clip The inverse of the matrix the picture was drawn
     * with.
     * @param camera_position Where the camera stands, in world space.
     * @param ndc_x Across the picture, -1 at the left edge.
     * @param ndc_y Down the picture, -1 at the top edge.
     * @return The ray, with a normalized direction.
     */
    [[nodiscard]] inline Ray ray_through_ndc(const Mat4& world_from_clip,
                                             const Vec3& camera_position, float ndc_x,
                                             float ndc_y) {
        // Depth 1 is the near plane under reverse-Z. See DESIGN.md section 3.
        const Vec4 on_near = world_from_clip * Vec4{ ndc_x, ndc_y, 1.0F, 1.0F };
        const Vec3 point = Vec3{ on_near } / on_near.w;
        return Ray{ .origin = camera_position, .direction = glm::normalize(point - camera_position) };
    }

} // namespace engine
