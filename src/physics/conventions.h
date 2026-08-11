#pragma once

/**
 * @file
 * @brief What Box3D assumes about the world, read out of the library.
 *
 * DESIGN.md section 3 holds the engine conventions. A physics library that
 * disagrees with them produces exactly the symptoms that section warns about.
 * Geometry falls the wrong way, and the cause looks like a bug in the gameplay
 * code.
 *
 * Box3D agrees. It is right-handed, and its heightfield spans X and Z with the
 * height along Y. The world definition it hands out has gravity pointing down
 * -Y. This file reads that vector out of the library rather than repeating the
 * number. So an update that turns the world over fails a test instead of
 * turning the game over.
 *
 * Nothing here opens a device or names a Box3D type. Only files under
 * src/physics/ include a Box3D header, and scripts/check-box3d-containment.sh
 * enforces that.
 */

#include "math/conventions.h"

/// @brief Rigid body simulation, and what the simulation library assumes.
namespace engine::physics {

    /**
     * @brief The gravity Box3D applies when a world definition names none.
     *
     * The value comes from the library at run time. At the pinned commit it is
     * -10 meters per second squared along Y. That is a game number rather than
     * the 9.81 of the world. The Box3D documentation says -9.8 and the code says
     * -10, so read the code.
     *
     * @return The default gravity, in meters per second squared.
     */
    [[nodiscard]] Vec3 default_gravity();

} // namespace engine::physics
