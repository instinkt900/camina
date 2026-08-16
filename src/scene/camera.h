#pragma once

/**
 * @file
 * @brief Finding the camera a scene plays through, and the matrix it gives.
 *
 * `scene::Camera` in `scene/components.h` says what a camera is. This says
 * which one a frame draws with, and turns it into the one matrix the passes
 * take.
 *
 * It is separate from the component because it reads the world, and a component
 * header that reached into the world would be included by everything that
 * carries one.
 */

// core/entt.h points ENTT_ASSERT at ENGINE_ASSERT, and it has to come before any
// EnTT header. It fails the build with a message when the order is wrong.
#include "core/entt.h"
#include "math/conventions.h"

#include <entt/entity/entity.hpp>
#include <entt/entity/fwd.hpp>

namespace engine::scene {

    class World;
    struct Camera;

    /**
     * @brief The camera the game plays through.
     *
     * The earliest entity with a `Camera` that has `primary` set wins. A scene
     * with no primary one falls back to the earliest camera of any kind, so a
     * person who clears the flag on the only camera still sees their scene.
     *
     * **Earliest is by entity, not by the order the registry hands them over.**
     * A scene file builds its entities in the order it lists them, so this is
     * the first camera somebody wrote. Taking whichever one the view yields
     * first would answer differently after any component was added or removed,
     * and a camera that changes on its own is a bad afternoon.
     *
     * **This reports what it chose when a scene holds more than one.** It says
     * so once for each call.
     *
     * @warning Call this when the world changes, not once for each frame. Both
     * applications call it where they bind a camera: at load, after a scene
     * reload, after a play session stops, and when the entity it holds is
     * destroyed under it. A caller that asked every frame would report every
     * frame and search the pool every frame, and neither answer changes until
     * an entity does.
     *
     * @param world The world to search.
     * @return The camera entity, or `entt::null` when the scene carries none.
     */
    [[nodiscard]] entt::entity primary_camera(const World& world);

    /**
     * @brief The camera of one entity, as clip space from world space.
     *
     * The view is the inverse of the entity's world matrix, so the camera looks
     * down its own -Z with its own +Y up. `DESIGN.md` §3 holds both. The
     * projection is the infinite reverse-Z one, so the near plane maps to depth
     * 1 and there is no far plane to choose.
     *
     * A model matrix is not part of this. Each entity supplies its own, and
     * `World::update` has already composed it.
     *
     * @param world The world the entity belongs to.
     * @param camera The camera entity. It must carry a `Camera`.
     * @param aspect Width divided by height of the image the scene renders
     * into. A value at or below zero is refused with 1, which is square.
     * @return The matrix the passes take as the camera.
     */
    [[nodiscard]] Mat4 clip_from_world(const World& world, entt::entity camera, float aspect);

    /**
     * @brief Where a camera entity stands and which way it looks.
     *
     * The position is the translation of the world matrix and the forward is
     * its -Z column, normalized. A script that acts along the line of sight
     * reads these, through `play::View`.
     *
     * @param world The world the entity belongs to.
     * @param camera The camera entity.
     * @param out_position Receives the world position.
     * @param out_forward Receives the unit vector it looks along.
     */
    void camera_pose(const World& world, entt::entity camera, Vec3& out_position,
                     Vec3& out_forward);

} // namespace engine::scene
