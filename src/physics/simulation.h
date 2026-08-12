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

#include <entt/entity/entity.hpp>
#include <entt/entity/fwd.hpp>

#include <cstddef>
#include <span>
#include <unordered_map>
#include <vector>

namespace engine::scene {
    class World;
}

namespace engine::physics {

    /**
     * @brief The bodies of one scene, and the world that steps them.
     *
     * A frame runs whole steps and then draws once, so the two halves are
     * separate calls. step() advances the solver and records the pose, and
     * interpolate() writes the pose the frame should draw. See
     * engine::FixedTimestep, which gives both the count and the blend factor.
     *
     * @code
     * physics::Simulation simulation;
     * simulation.build(scene_world);
     *
     * engine::FixedTimestep clock(60.0F);
     * for (std::uint32_t left = clock.advance(frame_delta); left > 0; --left) {
     *     simulation.step(scene_world, clock.step_seconds());
     * }
     * simulation.interpolate(scene_world, clock.alpha());
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
         * @brief Gives one entity a body, leaving every other body alone.
         *
         * This is what adds something to a world that is already running, such
         * as a projectile somebody threw. build() would answer the same
         * question by destroying every body and reading the scene again, which
         * loses the velocity of everything already moving and puts a settled
         * stack back where the scene file put it.
         *
         * The entity needs a RigidBody, and it follows the same rules build()
         * follows. A dynamic body under a parent is refused here too.
         *
         * @param world The scene holding the entity.
         * @param entity The entity to give a body to.
         * @return True when it got one. False when the entity is not valid, has
         *         no RigidBody, already has a body, or is a parented dynamic
         *         body. The last one logs why.
         */
        bool add_body(scene::World& world, entt::entity entity);

        /**
         * @brief Sets how fast one entity's body is moving.
         *
         * A throw is this and nothing else: create the body where the thrower
         * is, then give it a velocity. A velocity that is not zero also wakes
         * the body, so it disturbs a stack that has settled.
         *
         * @param entity The entity to move. One with no body is ignored.
         * @param velocity Meters each second, in world space.
         * @return True when the entity had a body to move.
         */
        bool set_linear_velocity(entt::entity entity, const Vec3& velocity);

        /**
         * @brief Two entities that started or stopped touching.
         *
         * The same shape serves a trigger overlap and a contact. For an
         * overlap @ref a is the trigger and @ref b is what passed through it.
         * For a contact the order is whatever Box3D reported.
         *
         * @warning **An entity here may already be gone.** Box3D reports an end
         * event when a shape is destroyed, so a reader has to check the world
         * before it acts on either one.
         */
        struct Touch {
            entt::entity a = entt::null; ///< The trigger, or the first body.
            entt::entity b = entt::null; ///< The visitor, or the second body.
            bool began = false;          ///< True on entering, false on leaving.
        };

        /**
         * @brief How fast one entity's body is moving.
         *
         * A body that has gone to sleep reads as zero, which is what it is
         * doing. Use is_awake() to tell a resting body from a moving one.
         *
         * @param entity The entity to read.
         * @param out Receives meters each second, in world space.
         * @return True when the entity had a body to read.
         */
        [[nodiscard]] bool linear_velocity(entt::entity entity, Vec3& out) const;

        /**
         * @brief Adds momentum to one entity's body at its center of mass.
         *
         * This wakes a sleeping body, because an impulse a body ignores is the
         * kind of nothing that takes an afternoon to find.
         *
         * @param entity The entity to push. One with no body is ignored.
         * @param impulse Kilogram meters each second, in world space.
         * @return True when the entity had a body to push.
         */
        bool apply_linear_impulse(entt::entity entity, const Vec3& impulse);

        /**
         * @brief Whether the solver is still working on one entity's body.
         *
         * @param entity The entity to ask about.
         * @return True while its body is awake. False when it sleeps, and false
         *         when it has no body at all.
         */
        [[nodiscard]] bool is_awake(entt::entity entity) const;

        /**
         * @brief Wakes one entity's body, or puts it to sleep.
         *
         * @warning **A sleeping body ignores a velocity of zero, silently.** So
         * a caller that means to stop a body which may have settled has to wake
         * it first. See DESIGN.md section 5.
         *
         * @param entity The entity to change. One with no body is ignored.
         * @param awake True to wake it, false to put it to sleep.
         * @return True when the entity had a body to change.
         */
        bool set_awake(entt::entity entity, bool awake);

        /**
         * @brief Advances the simulation by one fixed step.
         *
         * Three things happen in order. Every static and kinematic body takes
         * the transform of its entity. The solver runs. Every dynamic body
         * records where it now is, and where it was before the step.
         *
         * @warning **This does not move any entity. interpolate() does that.**
         * A caller that steps and never interpolates leaves every dynamic
         * entity where the scene put it, and the picture never changes. The two
         * are split because a frame runs zero, one, or several steps and then
         * draws once, so writing the scene inside the step would write it
         * several times and still draw the wrong pose. See DESIGN.md section 9.
         *
         * @param world The scene to read the static and kinematic bodies from.
         * @param delta_seconds How far to advance. Keep this fixed. A step that
         *                      follows the frame rate makes the simulation
         *                      depend on the machine it runs on.
         * @param substeps How many solver iterations to run inside the step.
         */
        void step(scene::World& world, float delta_seconds, std::uint32_t substeps = 4);

        /**
         * @brief Moves every dynamic entity to the pose the frame should draw.
         *
         * The pose is a blend of the last two steps. A display refreshing at a
         * rate that is not a multiple of the step rate otherwise draws the same
         * pose twice and then jumps, which reads as judder and looks like a
         * rendering fault rather than a timing one.
         *
         * Call this once for each frame, after the steps that frame ran.
         *
         * @note **The drawn pose is up to one step behind the solver.** This
         * blends two states that have both already happened, so it never
         * invents one. Extrapolating past the newest state would remove the
         * lag and would overshoot every collision, because the newest state is
         * exactly where the solver has not yet decided what happens next.
         *
         * @param world The scene to write the dynamic entities into.
         * @param alpha How far the frame sits into the step that has not run,
         *              from 0 to 1. engine::FixedTimestep::alpha() gives this.
         *              Pass 1 to draw the newest step with no blending, which
         *              is what a caller with no clock wants.
         */
        void interpolate(scene::World& world, float alpha);

        /// @brief How many entities have a body.
        /// @return The body count.
        [[nodiscard]] std::size_t body_count() const;

        /**
         * @brief Whether one entity has a body.
         * @param entity The entity to ask about.
         * @return True when build() gave it one.
         */
        [[nodiscard]] bool has_body(entt::entity entity) const;

        /**
         * @brief The trigger overlaps the last step() reported.
         *
         * Collected inside step(), because Box3D calls the event data transient
         * and the next step throws it away. One step can report several and
         * this holds all of them, which is what a puzzle needs: an event lost
         * because two things happened at once is the kind nobody reproduces.
         *
         * @return The overlaps, valid until the next step().
         */
        [[nodiscard]] std::span<const Touch> trigger_events() const;

        /**
         * @brief The contacts the last step() reported.
         *
         * @return The contacts, valid until the next step().
         */
        [[nodiscard]] std::span<const Touch> contact_events() const;

        /// @brief The world underneath, for anything this class does not cover.
        /// @return The Box3D world.
        [[nodiscard]] World& world();

        /**
         * @brief How many times a collider has been rebuilt at a new scale.
         *
         * A rebuild costs a shape and throws the contacts of that body away, so
         * a scene that resizes nothing must never pay for one. This is what
         * measures that rather than asserting it. See issue #237.
         *
         * @return The count since this Simulation was built.
         */
        [[nodiscard]] std::size_t shape_rebuild_count() const;

    private:
        /**
         * One body, and the two poses a frame blends between.
         *
         * A dynamic body carries both. A static or kinematic one leaves them
         * alone, because nothing interpolates an entity that owns its own
         * transform.
         */
        struct Body {
            BodyId id = 0;                                    ///< The body in the World.
            Vec3 previous_position{ 0.0F, 0.0F, 0.0F };       ///< Where it was before the last step.
            Quat previous_rotation{ 1.0F, 0.0F, 0.0F, 0.0F }; ///< How it was turned then.
            Vec3 position{ 0.0F, 0.0F, 0.0F };                ///< Where the last step left it.
            Quat rotation{ 1.0F, 0.0F, 0.0F, 0.0F };          ///< How the last step left it turned.
            Vec3 scale{ 1.0F, 1.0F, 1.0F };                   ///< The scale its shapes were built at.
        };

        /**
         * Puts the colliders an entity describes onto a body, at a scale.
         *
         * create_body() and rebuild_shapes() share this, so the scale reaches a
         * collider the same way whether the body is new or resized.
         *
         * @param world The scene holding the entity.
         * @param entity The entity to read the colliders from.
         * @param body The body to put them on.
         * @param scale The world scale of the entity.
         */
        void add_shapes(const scene::World& world, entt::entity entity, BodyId body,
                        const Vec3& scale);

        /**
         * Replaces the shapes of a body with ones built at a new scale.
         *
         * The body itself stays, so the velocity and the contacts survive.
         *
         * @param world The scene holding the entity.
         * @param entity The entity that was resized.
         * @param body The record to rebuild and to record the new scale in.
         * @param scale The world scale to build at.
         */
        void rebuild_shapes(const scene::World& world, entt::entity entity, Body& body,
                            const Vec3& scale);

        /**
         * Builds one body from what an entity describes.
         *
         * build() and add_body() share this, so the rules about colliders and
         * about a parented dynamic body are written once. Two copies would
         * drift, and the one that drifted would be the incremental path that
         * runs less often.
         *
         * @param world The scene to read. Its matrices must be current.
         * @param entity The entity to read, which must have a RigidBody.
         * @return True when a body was created.
         */
        bool create_body(scene::World& world, entt::entity entity);

        World m_world;
        std::unordered_map<entt::entity, Body> m_bodies;

        /// Turns the shape events of the last step into entity events.
        void collect_events();

        /// What the last step reported. Rebuilt on each step, never grown.
        std::vector<Touch> m_trigger_events;
        std::vector<Touch> m_contact_events;
        /// Reused so a step allocates nothing after the first one.
        std::vector<TouchEvent> m_shape_events;
        std::size_t m_shape_rebuilds = 0;
    };

} // namespace engine::physics
