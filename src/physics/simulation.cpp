#include "physics/simulation.h"

#include "core/log.h"
#include "core/profile.h"
#include "physics/components.h"
#include "scene/components.h"
#include "scene/world.h"

#include <entt/entity/registry.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/matrix_decompose.hpp>

#include <algorithm>
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
         * How far two scales have to differ before the shape is rebuilt.
         *
         * A rebuild throws the contacts of a body away, so this must not fire
         * on the last bit of a float. A scale that arrived through a matrix
         * decompose carries rounding, and a body resting on another one would
         * otherwise rebuild every step and never settle.
         */
        constexpr float kScaleEpsilon = 1e-4F;

        /**
         * Reads the scale out of a world matrix.
         *
         * The length of each basis column, which is what a decompose divides
         * out. A negative scale reads as a positive one here, because a length
         * has no sign. A mirrored collider is not a thing Box3D can hold, and
         * the size is right either way.
         */
        [[nodiscard]] Vec3 matrix_scale(const Mat4& matrix) {
            const Mat3 basis{ matrix };
            return Vec3{ glm::length(basis[0]), glm::length(basis[1]), glm::length(basis[2]) };
        }

        /**
         * Pulls the position and the rotation out of a world matrix.
         *
         * The scale is divided out rather than passed on, because a rigid body
         * has no scale. Handing the solver a basis that is not a rotation is
         * what this stops. A collider reads the scale separately, through
         * matrix_scale().
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

        /// Whether two scales differ enough to be worth a rebuild.
        [[nodiscard]] bool scale_changed(const Vec3& before, const Vec3& after) {
            return glm::any(glm::greaterThan(glm::abs(after - before), Vec3{ kScaleEpsilon }));
        }

        /**
         * The one number a sphere can take from a scale that has three.
         *
         * Box3D holds one radius, so an ellipsoid has no representation. The
         * largest axis is the safe choice of the three: a collider larger than
         * the picture holds the body up, and a smaller one lets it sink through
         * the floor. The caller says so in the log, because the picture and the
         * collider do disagree here.
         */
        [[nodiscard]] float sphere_scale(const Vec3& scale) {
            return std::max({ scale.x, scale.y, scale.z });
        }

        /// Whether a scale is the same along all three axes.
        [[nodiscard]] bool uniform(const Vec3& scale) {
            return std::abs(scale.x - scale.y) <= kScaleEpsilon &&
                   std::abs(scale.y - scale.z) <= kScaleEpsilon;
        }

    } // namespace

    Simulation::Simulation(std::uint32_t worker_count)
        : m_world(worker_count) {}

    void Simulation::build(scene::World& world) {
        ENGINE_PROFILE_ZONE_N("physics build");

        for (const auto& [entity, body] : m_bodies) {
            m_world.destroy_body(body.id);
        }
        m_bodies.clear();

        // A body starts where its entity sits, so every world matrix has to be
        // current before any of them is read.
        world.update();

        for (const entt::entity entity : world.registry().view<const RigidBody>()) {
            (void)create_body(world, entity);
        }

        ENGINE_LOG_INFO("Physics built {} bodies.", m_bodies.size());
    }

    bool Simulation::add_body(scene::World& world, entt::entity entity) {
        if (!world.registry().valid(entity) ||
            world.registry().try_get<const RigidBody>(entity) == nullptr) {
            return false;
        }
        if (has_body(entity)) {
            return false;
        }

        // Only this entity's matrix has to be current, but update() is what
        // knows which ones are stale. It does nothing when nothing moved.
        world.update();
        return create_body(world, entity);
    }

    bool Simulation::set_linear_velocity(entt::entity entity, const Vec3& velocity) {
        const auto found = m_bodies.find(entity);
        if (found == m_bodies.end()) {
            return false;
        }
        m_world.set_linear_velocity(found->second.id, velocity);
        return true;
    }

    bool Simulation::create_body(scene::World& world, entt::entity entity) {
        entt::registry& registry = world.registry();
        const RigidBody& description = registry.get<const RigidBody>(entity);

        if (registry.try_get<const BoxCollider>(entity) == nullptr &&
            registry.try_get<const SphereCollider>(entity) == nullptr) {
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
            return false;
        }

        Vec3 position{ 0.0F, 0.0F, 0.0F };
        Quat rotation{ 1.0F, 0.0F, 0.0F, 0.0F };
        decompose(world.world_matrix(entity), position, rotation);

        const BodyId body = m_world.add_body(description.type, position, rotation);
        const Vec3 scale = matrix_scale(world.world_matrix(entity));
        add_shapes(world, entity, body, scale);

        // Both poses start where the entity sits, so a frame drawn before the
        // first step blends two copies of the starting pose rather than reading
        // a rotation nobody set.
        m_bodies.emplace(entity, Body{ .id = body,
                                       .previous_position = position,
                                       .previous_rotation = rotation,
                                       .position = position,
                                       .rotation = rotation,
                                       .scale = scale });
        return true;
    }

    void Simulation::add_shapes(const scene::World& world, entt::entity entity, BodyId body,
                                const Vec3& scale) {
        const entt::registry& registry = world.registry();
        const RigidBody& description = registry.get<const RigidBody>(entity);
        const SurfaceMaterial material{ .density = description.density,
                                        .friction = description.friction,
                                        .restitution = description.restitution };

        // A box is axis aligned in the frame of its entity, so a scale of three
        // numbers lands exactly on three half extents. This is why a box takes
        // a non-uniform scale and a sphere does not.
        if (const auto* box = registry.try_get<const BoxCollider>(entity); box != nullptr) {
            m_world.add_box(body, box->half_extents * scale, material);
        }

        if (const auto* sphere = registry.try_get<const SphereCollider>(entity);
            sphere != nullptr) {
            if (!uniform(scale)) {
                ENGINE_LOG_WARN("{} scales its SphereCollider by {}, {}, {}, and a sphere holds "
                                "one radius. Taking the largest, so it collides bigger than it "
                                "draws rather than sinking through the floor. Use a BoxCollider "
                                "for a shape that is not the same on every axis.",
                                describe(world, entity), scale.x, scale.y, scale.z);
            }
            m_world.add_sphere(body, sphere->radius * sphere_scale(scale), material);
        }
    }

    void Simulation::rebuild_shapes(const scene::World& world, entt::entity entity, Body& body,
                                    const Vec3& scale) {
        // The body stays and only its shapes change. Destroying the body would
        // lose the velocity and the contacts, so a crate resized while it falls
        // would stop dead in the air.
        m_world.clear_shapes(body.id);
        add_shapes(world, entity, body.id, scale);
        m_world.apply_mass_from_shapes(body.id);
        body.scale = scale;
        ++m_shape_rebuilds;
    }

    void Simulation::step(scene::World& world, float delta_seconds, std::uint32_t substeps) {
        ENGINE_PROFILE_ZONE_N("physics simulation step");

        entt::registry& registry = world.registry();

        // The entity owns a static or kinematic body, so its transform goes in
        // before the solver runs. update() first, because an entity moved this
        // frame has a world matrix that is still the one from last frame.
        world.update();
        for (auto& [entity, body] : m_bodies) {
            const BodyType type = registry.get<const RigidBody>(entity).type;

            // Box3D fixes the size of a shape when it creates it, so a collider
            // that has to change size needs a new shape. Checked for every body
            // and not only the ones the entity owns, because the inspector can
            // resize a dynamic body while it falls. matrix_scale is three
            // lengths, so the frame that resizes nothing pays for those and the
            // compare. See issue #237.
            if (const Vec3 scale = matrix_scale(world.world_matrix(entity));
                scale_changed(body.scale, scale)) {
                rebuild_shapes(world, entity, body, scale);
            }

            if (type == BodyType::Dynamic) {
                // The pose the last step produced becomes the older half of the
                // pair, and the solver is about to write the newer half. This
                // is what a frame between two steps blends.
                body.previous_position = body.position;
                body.previous_rotation = body.rotation;
                continue;
            }

            Vec3 position{ 0.0F, 0.0F, 0.0F };
            Quat rotation{ 1.0F, 0.0F, 0.0F, 0.0F };
            decompose(world.world_matrix(entity), position, rotation);

            if (type == BodyType::Kinematic) {
                // A velocity rather than a teleport, so what rests on it is
                // carried rather than left behind.
                m_world.set_body_target(body.id, position, rotation, delta_seconds);
            } else {
                m_world.set_body_transform(body.id, position, rotation);
            }
        }

        m_world.step(delta_seconds, substeps);

        // The solver owns a dynamic body, so its transform comes back out. It
        // is recorded rather than written to the entity, because the frame that
        // draws may sit between this step and the next. interpolate() writes.
        for (auto& [entity, body] : m_bodies) {
            if (registry.get<const RigidBody>(entity).type != BodyType::Dynamic) {
                continue;
            }
            body.position = m_world.body_position(body.id);
            body.rotation = m_world.body_rotation(body.id);
        }
    }

    void Simulation::interpolate(scene::World& world, float alpha) {
        ENGINE_PROFILE_ZONE_N("physics interpolate");

        const float weight = std::clamp(alpha, 0.0F, 1.0F);
        entt::registry& registry = world.registry();

        // A dynamic body is at the root, which is what makes this a store
        // rather than a conversion out of world space.
        for (const auto& [entity, body] : m_bodies) {
            if (registry.get<const RigidBody>(entity).type != BodyType::Dynamic) {
                continue;
            }

            Transform local = world.local(entity);
            local.position = glm::mix(body.previous_position, body.position, weight);
            // slerp rather than a straight blend. Two quaternions read as two
            // points on a sphere, and mixing them moves at an uneven rate and
            // leaves a length that is not one. glm::slerp also flips the sign
            // when the pair points apart, so a turn takes the short way round.
            local.rotation = glm::slerp(body.previous_rotation, body.rotation, weight);

            // The scale is left as the scene set it. A rigid body has none to
            // give back.
            world.set_local(entity, local);
        }
    }

    std::size_t Simulation::body_count() const {
        return m_bodies.size();
    }

    std::size_t Simulation::shape_rebuild_count() const {
        return m_shape_rebuilds;
    }

    bool Simulation::has_body(entt::entity entity) const {
        return m_bodies.find(entity) != m_bodies.end();
    }

    World& Simulation::world() {
        return m_world;
    }

} // namespace engine::physics
