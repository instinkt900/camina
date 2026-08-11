#pragma once

/**
 * @file
 * @brief The wireframe of a physics world, as plain line segments.
 *
 * A collider is invisible. When a crate rests a little above the floor, or a
 * collider is half the size the mesh suggests, the picture shows a mesh that
 * floats and nothing says why. This is what turns that into something a person
 * can see.
 *
 * The lines come from Box3D rather than from the components the engine holds.
 * The point is to see what the solver thinks is there, not what we believe we
 * told it, and those two disagreeing is exactly the bug worth finding.
 *
 * Nothing here names a Box3D type, so `render/` can draw these with no physics
 * headers anywhere near it.
 */

#include "math/conventions.h"

namespace engine::physics {

    /**
     * @brief One line of a debug wireframe, in world space.
     *
     * A line rather than a shape, because every collider reduces to lines and a
     * renderer that takes lines needs to know nothing about colliders.
     */
    struct DebugLine {
        Vec3 from{ 0.0F, 0.0F, 0.0F };  ///< One end, in world space.
        Vec3 to{ 0.0F, 0.0F, 0.0F };    ///< The other end, in world space.
        Vec3 color{ 1.0F, 1.0F, 1.0F }; ///< Linear color. See DESIGN.md section 3.
    };

    /**
     * @brief How far from the origin the debug draw reaches, in meters.
     *
     * Box3D culls the debug draw against a box before it reports anything, so a
     * body outside this one draws nothing at all. The value is the Box3D
     * default. It covers the sandbox room many times over.
     *
     * @warning A scene larger than this loses the wireframe of whatever sits
     * outside it, with no message. Intel Sponza is about 30 meters across and
     * fits. A world-scale level would not.
     */
    inline constexpr float kDebugDrawReach = 100.0F;

} // namespace engine::physics
