#pragma once

/**
 * @file
 * @brief The components a scene uses to describe physics.
 *
 * These are ordinary reflected components, like `scene::PointLight`. Nothing
 * about physics makes them special, which is the point of hard rule 4.5: the
 * inspector generates its widgets from the descriptors, the `.scene` format
 * reads and writes the fields, and a prefab instance can override one of them.
 * None of that needed code written for it.
 *
 * Nothing here names a Box3D type, and nothing here includes a Box3D header. A
 * component says what a body is, and `physics/simulation.h` turns that into a
 * body. So a scene, a tool, or a test can carry physics data with no simulation
 * anywhere near it.
 */

#include "math/conventions.h"
#include "reflect/attributes.h"
#include "reflect/reflect.h"

#include <cstdint>
#include <tuple>

/// @cond
// Forward declared rather than included. physics/world.h needs BodyType from
// this file and has no business pulling the scene registry in behind it.
namespace engine::scene {
    class ComponentRegistry;
}
/// @endcond

namespace engine::physics {

    /**
     * @brief What moves a body, and what a body moves.
     *
     * One axis with three values, which is why this is an enum rather than
     * three components. A body is exactly one of these at a time.
     *
     * **This also decides which way the transform data moves.** A dynamic body
     * takes its transform from the solver, and a static or kinematic one gives
     * its transform to the solver. DESIGN.md section 9 holds the rule and the
     * reason.
     *
     * @warning **A dynamic body has to sit at the root of the hierarchy, and the
     * engine refuses one that does not.** Parenting means the child goes where
     * the parent goes, and a dynamic body goes where the solver puts it. A
     * static or kinematic body under a parent is correct, because the entity
     * owns its transform in both cases.
     */
    enum class BodyType : std::uint8_t {
        /// @brief Never moves. The floor and the walls. It costs no solver time.
        Static = 0,
        /// @brief Gravity and contacts move it. A crate in a stack. It sits at the root.
        Dynamic = 1,
        /// @brief The game moves it, and it pushes dynamic bodies aside. A lift.
        Kinematic = 2,
    };

    /// @brief The friction Box3D uses when nothing says otherwise.
    inline constexpr float kDefaultFriction = 0.6F;

    /// @brief The density of water, in kilograms per cubic meter.
    inline constexpr float kDefaultDensity = 1000.0F;

    /**
     * @brief Makes an entity a rigid body.
     *
     * An entity needs a collider as well to touch anything. A rigid body with
     * no collider still falls, and nothing stops it.
     *
     * @note **The mass is not here, because it follows from the collider.** Mass
     * is density times the volume the colliders enclose, which is how Box3D
     * works and what keeps one crate twice the size of another four times as
     * hard to push without anybody editing a second number. A body that needs a
     * mass the shape does not give it is not something the sandbox has asked
     * for.
     */
    struct RigidBody {
        /// @brief What moves it. See BodyType.
        BodyType type = BodyType::Dynamic;
        /// @brief Kilograms per cubic meter. Water is 1000, and oak is about 700.
        float density = kDefaultDensity;
        /// @brief How hard it is to slide. 0 is ice.
        float friction = kDefaultFriction;
        /// @brief How much it bounces. 0 absorbs the energy, and 1 gives it back.
        float restitution = 0.0F;
    };

    /// @brief Half the size of a default collider, in meters.
    inline constexpr float kDefaultColliderHalfSize = 0.5F;

    /**
     * @brief A box that collides, centered on the entity.
     *
     * The half extents are in meters, before the scale of the entity. The
     * simulation multiplies the two, so a prefab instance scaled to twice its
     * size collides at twice its size. A scale that differs on each axis lands
     * exactly, because a box is axis aligned in the frame of its entity.
     *
     * @note **A scale changed after the body exists rebuilds the shape.** Box3D
     * fixes the size of a shape when it creates it, so the next step replaces
     * it. The body survives, which keeps the velocity and the contacts.
     */
    struct BoxCollider {
        /// @brief Half the size along each axis, in meters, before the entity scale.
        Vec3 half_extents{ kDefaultColliderHalfSize, kDefaultColliderHalfSize,
                           kDefaultColliderHalfSize };

        /**
         * @brief Whether it reports what passes through instead of stopping it.
         *
         * A trigger takes part in no contact resolution. Something moves
         * through it and the simulation reports that it did, which is what a
         * goal volume or a pickup is.
         */
        bool is_trigger = false;
    };

    /**
     * @brief A sphere that collides, centered on the entity.
     *
     * The radius is in meters, before the scale of the entity, the same way
     * BoxCollider works.
     *
     * @warning **A sphere holds one radius, so a scale that differs on each
     * axis cannot land exactly.** The simulation takes the largest of the
     * three and says so in the log, naming the entity. That collides bigger
     * than it draws, which holds the body up. Taking the smallest would let it
     * sink through a floor instead. Use a BoxCollider for a shape that is not
     * the same on every axis.
     */
    struct SphereCollider {
        /// @brief How far it reaches, in meters, before the entity scale.
        float radius = kDefaultColliderHalfSize;

        /// @brief Whether it reports what passes through instead of stopping it.
        /// @see BoxCollider::is_trigger
        bool is_trigger = false;
    };

    /**
     * @brief Registers the physics components with a scene registry.
     *
     * The engine registers its own built-ins in
     * `scene::register_builtin_components()`, and this is the same call for the
     * physics subsystem. It stays separate so that `scene/` needs no physics
     * header, the same way a game module registers its own components.
     *
     * @param registry Where to register them.
     */
    void register_components(scene::ComponentRegistry& registry);

    /**
     * @brief Registers the physics components with the global scene registry.
     *
     * This is an overload rather than a default argument, so that naming the
     * global registry does not force this header to include it.
     */
    void register_components();

} // namespace engine::physics

/// @brief Describes what moves a body, so the inspector draws a drop-down.
template <>
struct engine::reflect::Describe<engine::physics::BodyType> {
    static constexpr const char* name = "BodyType"; ///< The name a scene file stores.
    /// @brief The three values.
    /// @return A tuple of enumerator descriptors.
    static constexpr auto enumerators() {
        return std::make_tuple(ENGINE_ENUMERATOR(engine::physics::BodyType, Static),
                               ENGINE_ENUMERATOR(engine::physics::BodyType, Dynamic),
                               ENGINE_ENUMERATOR(engine::physics::BodyType, Kinematic));
    }
};

/// @brief Field descriptors for a rigid body.
template <>
struct engine::reflect::Describe<engine::physics::RigidBody> {
    static constexpr const char* name = "RigidBody"; ///< The name a scene file stores.
    /// @brief The four fields. The mass follows from the collider.
    /// @return A tuple of field descriptors.
    static constexpr auto fields() {
        return std::make_tuple(
            ENGINE_FIELD(engine::physics::RigidBody, type,
                         engine::reflect::Tooltip{ "What moves it, and what it moves." }),
            ENGINE_FIELD(engine::physics::RigidBody, density,
                         engine::reflect::Range{ 1.0, 20000.0, 1.0 },
                         engine::reflect::Tooltip{
                             "Kilograms per cubic meter. The mass follows from this and "
                             "the collider size." }),
            ENGINE_FIELD(engine::physics::RigidBody, friction,
                         engine::reflect::Range{ 0.0, 2.0, 0.01 },
                         engine::reflect::Tooltip{ "How hard it is to slide. 0 is ice." }),
            ENGINE_FIELD(engine::physics::RigidBody, restitution,
                         engine::reflect::Range{ 0.0, 1.0, 0.01 },
                         engine::reflect::Tooltip{ "How much it bounces." }));
    }
};

/// @brief Field descriptors for a box collider.
template <>
struct engine::reflect::Describe<engine::physics::BoxCollider> {
    static constexpr const char* name = "BoxCollider"; ///< The name a scene file stores.
    /// @brief The two fields.
    /// @return A tuple of field descriptors.
    static constexpr auto fields() {
        return std::make_tuple(
            ENGINE_FIELD(engine::physics::BoxCollider, half_extents,
                         engine::reflect::Tooltip{
                             "Half the size along each axis, in meters. The entity scale "
                             "multiplies this." }),
            ENGINE_FIELD(engine::physics::BoxCollider, is_trigger,
                         engine::reflect::Version{ 2 },
                         engine::reflect::Tooltip{
                             "Reports what passes through instead of stopping it." }));
    }
};

/// @brief Field descriptors for a sphere collider.
template <>
struct engine::reflect::Describe<engine::physics::SphereCollider> {
    static constexpr const char* name = "SphereCollider"; ///< The name a scene file stores.
    /// @brief The two fields.
    /// @return A tuple of field descriptors.
    static constexpr auto fields() {
        return std::make_tuple(
            ENGINE_FIELD(engine::physics::SphereCollider, radius,
                         engine::reflect::Range{ 0.001, 100.0, 0.01 },
                         engine::reflect::Tooltip{
                             "How far it reaches, in meters. The entity scale "
                             "multiplies this, and a sphere takes the largest axis." }),
            ENGINE_FIELD(engine::physics::SphereCollider, is_trigger,
                         engine::reflect::Version{ 2 },
                         engine::reflect::Tooltip{
                             "Reports what passes through instead of stopping it." }));
    }
};
