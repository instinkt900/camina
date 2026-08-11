#include "physics/simulation.h"

#include "core/log.h"
#include "core/profile.h"
#include "physics/components.h"
#include "scene/components.h"
#include "scene/world.h"

#include <entt/entity/registry.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/matrix_decompose.hpp>

#include <string>

namespace engine::physics {

    namespace {

        /// Names an entity for a message. The Name component when it has one, and
        /// the entity number when it does not, because a message that says
        /// "an entity" sends the reader looking through the whole scene.
        [[nodiscard]] std::string describe(const scene::World& world, entt::entity entity) {
            const auto* name = world.registry().try_get<scene::Name>(entity);
            if (name != nullptr && !name->value.empty()) {
                return name->value;
            }
            return "entity " + std::to_string(static_cast<std::uint32_t>(entity));
        }

        /**
         * Pulls the position and the rotation out of a world matrix.
         *
         * The scale is dropped rather than passed on. A rigid body has no scale,
         * and a collider does not read one either, which is issue #237. Dividing
         * it out here is what stops a scaled entity from handing the solver a
         * rotation matrix that is not a rotation.
         */
        void decompose(const Mat4& matrix, Vec3& position, Quat& rotation) {
            position = Vec3{ matrix[3] };

            Mat3 basis{ matrix };
            for (glm::length_t axis = 0; axis < 3; ++axis) {
                const float length = glm::length(basis[axis]);
                if (length > 0.0F) {
                    basis[axis] /= length;
                }
            }
            rotation = glm::quat_cast(basis);
        }

    } // namespace

    Simulation::Simulation(std::uint32_t worker_count)
        : m_world(worker_count) {}

    void Simulation::build(scene::World& world) {
        ENGINE_PROFILE_ZONE_N("physics build");

        for (const auto& [entity, body] : m_bodies) {
            m_world.destroy_body(body);
        }
        m_bodies.clear();

        // A body starts where its entity sits, so every world matrix has to be
        // current before any of them is read.
        world.update();

        entt::registry& registry = world.registry();
        const auto view = registry.view<const RigidBody>();

        for (const entt::entity entity : view) {
            const RigidBody& description = view.get<const RigidBody>(entity);

            const auto* box = registry.try_get<const BoxCollider>(entity);
            const auto* sphere = registry.try_get<const SphereCollider>(entity);
            if (box == nullptr && sphere == nullptr) {
                ENGINE_LOG_WARN("{} has a RigidBody and no collider, so it falls through "
                                "everything. Add a BoxCollider or a SphereCollider.",
                                describe(world, entity));
            }

            // DESIGN.md section 9. Parenting says the child goes where the parent
            // goes, and a dynamic body goes where the solver puts it. Refusing is
            // what keeps the hierarchy from meaning two things at once.
            const auto& hierarchy = registry.get<scene::Hierarchy>(entity);
            if (description.type == BodyType::Dynamic && hierarchy.parent != entt::null) {
                ENGINE_LOG_ERROR("{} is a dynamic body under a parent, so it gets no body. "
                                 "A dynamic body sits at the root, because the solver decides "
                                 "where it goes. See DESIGN.md section 9.",
                                 describe(world, entity));
                continue;
            }

            Vec3 position{ 0.0F, 0.0F, 0.0F };
            Quat rotation{ 1.0F, 0.0F, 0.0F, 0.0F };
            decompose(world.world_matrix(entity), position, rotation);

            const BodyId body = m_world.add_body(description.type, position, rotation);
            const SurfaceMaterial material{ .density = description.density,
                                            .friction = description.friction,
                                            .restitution = description.restitution };

            if (box != nullptr) {
                m_world.add_box(body, box->half_extents, material);
            }
            if (sphere != nullptr) {
                m_world.add_sphere(body, sphere->radius, material);
            }

            m_bodies.emplace(entity, body);
        }

        ENGINE_LOG_INFO("Physics built {} bodies.", m_bodies.size());
    }

    void Simulation::step(scene::World& world, float delta_seconds, std::uint32_t substeps) {
        ENGINE_PROFILE_ZONE_N("physics simulation step");

        entt::registry& registry = world.registry();

        // The entity owns a static or kinematic body, so its transform goes in
        // before the solver runs. update() first, because an entity moved this
        // frame has a world matrix that is still the one from last frame.
        world.update();
        for (const auto& [entity, body] : m_bodies) {
            const BodyType type = registry.get<const RigidBody>(entity).type;
            if (type == BodyType::Dynamic) {
                continue;
            }

            Vec3 position{ 0.0F, 0.0F, 0.0F };
            Quat rotation{ 1.0F, 0.0F, 0.0F, 0.0F };
            decompose(world.world_matrix(entity), position, rotation);

            if (type == BodyType::Kinematic) {
                // A velocity rather than a teleport, so what rests on it is
                // carried rather than left behind.
                m_world.set_body_target(body, position, rotation, delta_seconds);
            } else {
                m_world.set_body_transform(body, position, rotation);
            }
        }

        m_world.step(delta_seconds, substeps);

        // The solver owns a dynamic body, so its transform comes back out. A
        // dynamic body is at the root, which is what makes this a store rather
        // than a conversion out of world space.
        for (const auto& [entity, body] : m_bodies) {
            if (registry.get<const RigidBody>(entity).type != BodyType::Dynamic) {
                continue;
            }

            Transform local = world.local(entity);
            local.position = m_world.body_position(body);
            local.rotation = m_world.body_rotation(body);

            // The scale is left as the scene set it. A rigid body has none to
            // give back.
            world.set_local(entity, local);
        }
    }

    std::size_t Simulation::body_count() const {
        return m_bodies.size();
    }

    bool Simulation::has_body(entt::entity entity) const {
        return m_bodies.find(entity) != m_bodies.end();
    }

    World& Simulation::world() {
        return m_world;
    }

} // namespace engine::physics
