#include "editor/placement.h"

#include "math/transform.h"
#include "scene/components.h"
#include "scene/world.h"

namespace engine::editor {

    void place_entity(scene::World& world, entt::entity entity, const Mat4& world_matrix) {
        Mat4 local = world_matrix;

        // Divide the parent out. A child of a moved parent lands where the
        // pointer is only because of this: its local transform is relative to
        // whatever the parent is doing, and a gizmo works in world space.
        const auto* node = world.registry().try_get<scene::Hierarchy>(entity);
        if (node != nullptr && node->parent != entt::null) {
            local = glm::inverse(world.world_matrix(node->parent)) * world_matrix;
        }

        world.set_local(entity, from_matrix(local));
    }

} // namespace engine::editor
