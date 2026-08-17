#pragma once

/**
 * @file
 * @brief Moving an entity to a pose given in world space.
 *
 * A gizmo works in world space, and an entity stores a transform relative to
 * its parent. This is the step between the two, and it is the part with the
 * arithmetic in it, so it lives here rather than beside the ImGuizmo calls in
 * `apps/editor/gizmo.h`. A test drives it with no window and no library.
 */

// core/entt.h points ENTT_ASSERT at ENGINE_ASSERT, and it has to come before any
// EnTT header. It fails the build with a message when the order is wrong.
#include "core/entt.h"
#include "math/conventions.h"

#include <entt/entity/fwd.hpp>

namespace engine::scene {
    class World;
}

namespace engine::editor {

    /**
     * @brief Moves an entity so that its world pose is @p world_matrix.
     *
     * The parent is divided out, so a child of a moved parent lands where it
     * was put rather than where its old local transform would take it.
     *
     * The write goes through `World::set_local`, which marks the subtree stale.
     * Writing the component directly would leave every descendant behind, which
     * is the bug issue #302 named before the work started.
     *
     * @warning **A mirrored matrix comes back unmirrored**, because
     * `engine::from_matrix` reads a scale as a length. No gizmo produces one.
     *
     * @param world The world the entity belongs to. Its matrices must be
     * composed, which `World::update` does once for each frame.
     * @param entity The entity to move.
     * @param world_matrix Where it should end up, in world space.
     */
    void place_entity(scene::World& world, entt::entity entity, const Mat4& world_matrix);

} // namespace engine::editor
