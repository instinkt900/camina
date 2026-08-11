#pragma once

/**
 * @file
 * @brief What Box3D assumes about the world, read out of the library.
 *
 * DESIGN.md section 3 holds the engine conventions, and a physics library that
 * disagrees with them produces exactly the symptoms that section warns about:
 * geometry that falls the wrong way, and a cause that looks like a bug in the
 * gameplay code.
 *
 * Box3D agrees. It is right-handed, its heightfield spans X and Z with the
 * height along Y, and the world definition it hands out has gravity pointing
 * down -Y. This file reads that vector out of the library rather than repeating
 * the number, so an update that turns the world over fails a test instead of
 * turning the game over.
 *
 * Nothing here opens a device or names a Box3D type. Only files under
 * src/physics/ include a Box3D header, and scripts/check-box3d-containment.sh
 * enforces that.
 */

#include "math/conventions.h"

namespace engine::physics {

    /**
     * @brief The gravity Box3D applies when a world definition names none.
     *
     * The value comes from the library at run time. It is -10 meters per second
     * squared along Y at the pinned commit, which is a game number rather than
     * the 9.81 of the world. The Box3D documentation says -9.8 and the code says
     * -10, so read the code.
     *
     * @return The default gravity, in meters per second squared.
     */
    [[nodiscard]] Vec3 default_gravity();

} // namespace engine::physics
