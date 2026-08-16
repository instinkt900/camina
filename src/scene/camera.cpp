#include "scene/camera.h"

#include "core/log.h"
#include "scene/components.h"
#include "scene/world.h"

#include <cstddef>
#include <string>

namespace engine::scene {

    namespace {

        /// Names an entity for a report, or gives its number when it has no name.
        [[nodiscard]] std::string label_of(const entt::registry& entities, entt::entity entity) {
            const auto* named = entities.try_get<Name>(entity);
            if (named != nullptr && !named->value.empty()) {
                return named->value;
            }
            return "entity " + std::to_string(entt::to_integral(entity));
        }

    } // namespace

    entt::entity primary_camera(const World& world) {
        const entt::registry& entities = world.registry();
        const auto view = entities.view<const Camera>();

        entt::entity chosen = entt::null;
        entt::entity earliest = entt::null;
        std::size_t cameras = 0;

        // The earliest entity rather than the first one the view hands over.
        // EnTT iterates its pool, and that order is neither creation order nor
        // stable across a component being added or removed. A scene file builds
        // its entities in the order it lists them, so the smallest one is the
        // first camera a person wrote. Picking off the view instead gives a
        // scene with two cameras a different answer on different days.
        for (const entt::entity entity : view) {
            ++cameras;
            if (earliest == entt::null || entity < earliest) {
                earliest = entity;
            }
            if (view.get<const Camera>(entity).primary &&
                (chosen == entt::null || entity < chosen)) {
                chosen = entity;
            }
        }

        if (chosen == entt::null) {
            // A person who cleared the flag on the only camera still sees their
            // scene. Refusing to draw would look like a broken renderer.
            chosen = earliest;
            if (chosen != entt::null) {
                ENGINE_LOG_WARN("No camera is marked primary, so the scene plays through {}.",
                                label_of(entities, chosen));
            }
        } else if (cameras > 1) {
            ENGINE_LOG_INFO("The scene holds {} cameras, and it plays through {}.", cameras,
                            label_of(entities, chosen));
        }

        return chosen;
    }

    Mat4 clip_from_world(const World& world, entt::entity camera, float aspect) {
        const Camera& settings = world.registry().get<const Camera>(camera);
        const Mat4 projection = perspective_reverse_z(glm::radians(settings.fov_degrees),
                                                      aspect > 0.0F ? aspect : 1.0F,
                                                      settings.near_plane);
        // The view is the inverse of where the camera stands. glm::inverse
        // rather than affineInverse, because a scaled camera entity is legal
        // and the cheap form assumes no scale.
        return projection * glm::inverse(world.world_matrix(camera));
    }

    void camera_pose(const World& world, entt::entity camera, Vec3& out_position,
                     Vec3& out_forward) {
        const Mat4& matrix = world.world_matrix(camera);
        out_position = Vec3{ matrix[3] };
        // A camera looks down its own -Z, per DESIGN.md section 3. Column 2 is
        // where +Z points, so the forward is its negation.
        out_forward = glm::normalize(-Vec3{ matrix[2] });
    }

} // namespace engine::scene
