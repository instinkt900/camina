#pragma once

/**
 * @file
 * @brief The wireframe that shows where a scene camera is and what it sees.
 *
 * The editor draws through its own view, so the camera a game plays through is
 * off screen as soon as somebody flies away from it. It has no mesh and nothing
 * else marks it, so without this a person cannot find the camera they are
 * authoring, let alone see which way it points.
 *
 * The shape is a pyramid from the camera position out to a fixed distance, with
 * a bar across the top of the far end. The bar is what says which way is up,
 * which a symmetrical pyramid cannot.
 */

// core/entt.h points ENTT_ASSERT at ENGINE_ASSERT, and it has to come before any
// EnTT header. It fails the build with a message when the order is wrong.
#include "core/entt.h"
#include "math/conventions.h"
#include "physics/debug_draw.h"

#include <entt/entity/fwd.hpp>

#include <vector>

namespace engine::scene {
    class World;
}

namespace engine::editor {

    /**
     * @brief How far the drawn frustum reaches, in meters.
     *
     * The engine has no far plane, so the wireframe needs a length of its own.
     * Four meters reads well in a room and stays out of the way. It says which
     * way the camera looks rather than how far it can see, which is a thing no
     * frustum in this engine can say.
     */
    inline constexpr float kCameraLinesLength = 4.0F;

    /// @brief The color of the wireframe. Linear, per DESIGN.md §3.
    inline constexpr Vec3 kCameraLinesColor{ 0.95F, 0.85F, 0.2F };

    /**
     * @brief Builds the wireframe of one camera entity.
     *
     * @warning **The horizontal spread is the aspect the caller passes, not one
     * the camera holds.** A `scene::Camera` records a vertical field of view and
     * nothing about the shape of the window a game will run in, so the width of
     * this pyramid is a guess and the height is exact. Pass the aspect the
     * editor is drawing at, which is the closest thing to an answer available.
     *
     * @param world The world the camera belongs to. Its matrices must be
     * composed, which `World::update` does once for each frame.
     * @param camera The camera entity. It must carry a `scene::Camera`.
     * @param aspect Width divided by height to draw the pyramid at. A value at
     * or below zero is refused with 1, which is square.
     * @param out Receives the lines. Cleared first, so a caller can hold one
     * vector across frames and allocate once.
     */
    void camera_lines(const scene::World& world, entt::entity camera, float aspect,
                      std::vector<physics::DebugLine>& out);

} // namespace engine::editor
