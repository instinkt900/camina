#include "editor/picking.h"

#include "scene/components.h"
#include "scene/world.h"

#include <limits>

namespace engine::editor {

    entt::entity pick_entity(const scene::World& world, const Ray& ray,
                             const BoundsLookup& bounds) {
        entt::entity nearest = entt::null;
        float nearest_distance = std::numeric_limits<float>::max();

        for (const auto [entity, renderer] :
             world.registry().view<const scene::MeshRenderer>().each()) {
            Vec3 min{ 0.0F };
            Vec3 max{ 0.0F };
            if (!bounds || !bounds(renderer.mesh, min, max)) {
                continue;
            }

            // Into the local space of the entity, which turns the local box into
            // an oriented box in the world for the price of one inverse.
            const Mat4 local_from_world = glm::inverse(world.world_matrix(entity));
            const Ray local{
                .origin = Vec3{ local_from_world * Vec4{ ray.origin, 1.0F } },
                // A direction is not a point, so the translation column must not
                // reach it. Without the zero it would be the difference between
                // two points and every off-origin entity would miss.
                .direction = Vec3{ local_from_world * Vec4{ ray.direction, 0.0F } },
            };

            float distance = 0.0F;
            if (!ray_hits_box(local, min, max, distance)) {
                continue;
            }

            // The distance is in units of the local direction, which a scaled
            // entity stretches. Measuring in world space is what keeps two
            // entities of different scale comparable.
            const Vec3 hit_local = local.origin + (local.direction * distance);
            const Vec3 hit_world = Vec3{ world.world_matrix(entity) * Vec4{ hit_local, 1.0F } };
            const float world_distance = glm::length(hit_world - ray.origin);

            if (world_distance < nearest_distance) {
                nearest_distance = world_distance;
                nearest = entity;
            }
        }

        return nearest;
    }

} // namespace engine::editor
