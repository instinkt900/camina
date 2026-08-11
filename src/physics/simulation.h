#pragma once

/**
 * @file
 * @brief Turns the physics components into bodies, and keeps the two in step.
 *
 * `physics/components.h` says what a body is and `physics/world.h` runs one.
 * This is what joins them, and it is the only file that knows about both the
 * entity registry and the solver.
 *
 * DESIGN.md section 9 holds the rule this file implements. `BodyType` decides
 * which way the transform data moves: the solver owns a dynamic body, and the
 * entity owns a static or kinematic one.
 */

// core/entt.h points ENTT_ASSERT at ENGINE_ASSERT, and it has to come before any
// EnTT header. It fails the build with a message when the order is wrong.
#include "core/entt.h"
#include "math/conventions.h"
#include "physics/world.h"

#include <entt/entity/fwd.hpp>

#include <cstddef>
#include <unordered_map>

namespace engine::scene {
    class World;
}

namespace engine::physics {

    /**
     * @brief The bodies of one scene, and the world that steps them.
     *
     * @code
     * physics::Simulation simulation;
     * simulation.build(scene_world);
     * simulation.step(scene_world, 1.0F / 60.0F);
     * @endcode
     */
    class Simulation {
    public:
        /**
         * @brief Builds the world the bodies will live in.
         * @param worker_count Passed to World. 0 takes what the job system has.
         */
        explicit Simulation(std::uint32_t worker_count = 0);

        /**
         * @brief Creates a body for every entity that describes one.
         *
         * An entity needs a RigidBody and one collider. It gets its starting
         * transform from where it sits in the world, so a scene places its
         * bodies by placing its entities.
         *
         * Calling this again throws away every body and builds them afresh,
         * which is what a reloaded scene needs.
         *
         * @param world The scene to read. This calls update() on it first, so
         *              the world matrices are the ones the entities describe.
         *
         * @warning **A dynamic body on an entity with a parent is refused, and
         * the entity gets no body at all.** The message names the entity. See
         * DESIGN.md section 9 for why a dynamic body sits at the root.
         */
        void build(scene::World& world);

        /**
         * @brief Advances the simulation and moves the entities that follow it.
         *
         * Three things happen in order. Every static and kinematic body takes
         * the transform of its entity. The solver runs. Every dynamic entity
         * takes the transform of its body.
         *
         * @param world The scene to read from and write to.
         * @param delta_seconds How far to advance.
         * @param substeps How many solver iterations to run inside the step.
         */
        void step(scene::World& world, float delta_seconds, std::uint32_t substeps = 4);

        /// @brief How many entities have a body.
        /// @return The body count.
        [[nodiscard]] std::size_t body_count() const;

        /**
         * @brief Whether one entity has a body.
         * @param entity The entity to ask about.
         * @return True when build() gave it one.
         */
        [[nodiscard]] bool has_body(entt::entity entity) const;

        /// @brief The world underneath, for anything this class does not cover.
        /// @return The Box3D world.
        [[nodiscard]] World& world();

    private:
        World m_world;
        std::unordered_map<entt::entity, BodyId> m_bodies;
    };

} // namespace engine::physics
