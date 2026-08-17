#pragma once

/**
 * @file
 * @brief Finding the entity under a ray, which is how a click selects one.
 *
 * Clicking a thing is how everybody expects to select it, and a hierarchy tree
 * stops scaling the moment a scene holds more than a screenful. Issue #34 has
 * been open since M3.3 for that reason.
 *
 * The search takes the bounds through a callback rather than reaching into the
 * renderer for them. So `src/editor/` needs nothing from `src/render/`, and a
 * test drives the whole thing with bounds it makes up and no GPU at all.
 */

// core/entt.h points ENTT_ASSERT at ENGINE_ASSERT, and it has to come before any
// EnTT header. It fails the build with a message when the order is wrong.
#include "core/entt.h"
#include "core/guid.h"
#include "math/ray.h"

#include <entt/entity/entity.hpp>
#include <entt/entity/fwd.hpp>

#include <functional>

namespace engine::scene {
    class World;
}

namespace engine::editor {

    /**
     * @brief Answers what a mesh occupies, in its own local space.
     *
     * The editor hands over one that reads `render::MeshCache`. A test hands
     * over one that answers from a table.
     *
     * @param mesh The identity a `scene::MeshRenderer` names.
     * @param out_min Receives the smallest corner of the bounds.
     * @param out_max Receives the largest corner.
     * @return True when the mesh is known. False leaves the entity unpickable,
     * which is the right answer for a mesh that has not loaded yet.
     */
    using BoundsLookup = std::function<bool(Guid mesh, Vec3& out_min, Vec3& out_max)>;

    /**
     * @brief The entity a ray hits first.
     *
     * Only an entity with a `scene::MeshRenderer` can be hit. A light, a camera,
     * and a bare transform occupy nothing, and picking something invisible would
     * be worse than picking nothing.
     *
     * The ray is put into the local space of each candidate, so the test is
     * against the oriented box of the entity rather than a box around it. A
     * sphere would select the empty corner of a long thin wall.
     *
     * @param world The world to search. Its matrices must be composed, which
     * `World::update` does once for each frame.
     * @param ray The ray, in world space.
     * @param bounds What each mesh occupies.
     * @return The nearest entity the ray meets, or `entt::null` for a miss.
     */
    [[nodiscard]] entt::entity pick_entity(const scene::World& world, const Ray& ray,
                                           const BoundsLookup& bounds);

} // namespace engine::editor
