#pragma once

/**
 * @file
 * @brief A Box3D world, stepped on the engine job system.
 *
 * This is the seam between Box3D and `engine::jobs`. Box3D takes an enqueue
 * callback and a finish callback so a host can run the solver on its own pool,
 * and DESIGN.md section 5.1 settles that enkiTS owns every worker thread in the
 * engine. Two pools on one machine oversubscribe the cores, and then each one
 * measures the other as contention.
 *
 * The world holds no components yet. M7.3 adds the reflected `RigidBody` and
 * `Collider` and writes transforms back. What is here is the smallest world
 * that can carry a load, because a scheduler with nothing to schedule cannot be
 * measured.
 *
 * Nothing in this header names a Box3D type, so a caller needs no Box3D headers.
 * scripts/check-box3d-containment.sh keeps it that way.
 */

#include "math/conventions.h"
#include "physics/components.h"
#include "physics/debug_draw.h"

#include <cstdint>
#include <vector>

namespace engine::physics {

    /// @brief Identifies one body inside a World. Zero is no body.
    using BodyId = std::uint64_t;

    /**
     * @brief What a shape is besides its size and its material.
     *
     * @p user is opaque here on purpose. This class wraps Box3D and names no
     * entity, so `physics::Simulation` puts the entity in it and reads it back
     * out of an event. Box3D reports an event by shape, and a caller needs to
     * know which of its own things that shape belonged to.
     */
    struct ShapeOptions {
        /**
         * @brief Whether the shape reports overlaps instead of pushing.
         *
         * A trigger takes part in no contact resolution. Something moves
         * through it and the world reports that it did.
         */
        bool is_trigger = false;

        /// @brief Whatever the caller wants back when this shape reports an event.
        std::uint64_t user = 0;
    };

    /**
     * @brief One shape starting or stopping touching another.
     *
     * The same shape serves a trigger overlap and a contact, because both are
     * two shapes and a direction. @ref a is the trigger for an overlap and the
     * first shape for a contact.
     *
     * @warning **A shape reports an end event when it is destroyed**, so an
     * entity that has gone away still appears here once. A reader has to cope
     * with a value naming something it can no longer find.
     */
    struct TouchEvent {
        /// @brief The ShapeOptions::user of the trigger, or of the first shape.
        std::uint64_t a = 0;
        /// @brief The ShapeOptions::user of the visitor, or of the second shape.
        std::uint64_t b = 0;
        /// @brief True when the two began touching, false when they stopped.
        bool began = false;
    };

    /**
     * @brief What a shape is made of.
     *
     * These sit on the shape rather than on the body, which is how Box3D works.
     * A body with two shapes can be wood on one side and steel on the other,
     * and its mass is the sum of what each shape encloses.
     */
    struct SurfaceMaterial {
        /// @brief Kilograms per cubic meter. The mass follows from this.
        float density = kDefaultDensity;
        /// @brief How hard it is to slide. 0 is ice.
        float friction = kDefaultFriction;
        /// @brief How much it bounces. 0 absorbs the energy, and 1 gives it back.
        float restitution = 0.0F;
    };

    /**
     * @brief How many tasks Box3D has handed to the job system since the start.
     *
     * This is the proof that the solver runs where DESIGN.md section 5.1 says it
     * does. Box3D takes its own scheduler and starts its own threads whenever a
     * world definition leaves either callback null, and a step that ran that way
     * would look the same from outside except for this count. See the branch at
     * `b3CreateWorld` in `third_party/box3d/src/physics_world.c`.
     *
     * The count covers every world, because the callbacks reach one global job
     * system and carry no per-world context.
     *
     * @return The running total. It never resets.
     */
    [[nodiscard]] std::uint64_t tasks_enqueued();

    /**
     * @brief A rigid body world that runs its solver on the engine job system.
     *
     * @warning **Step this from a thread that owns itself, normally the main
     * thread.** Box3D blocks inside the finish callback while the solver joins
     * the tasks it started, so the stack of step() is held across every fork and
     * join. jobs::wait() runs other pending work while it blocks, which is what
     * keeps that from deadlocking. Stepping from inside another task that cannot
     * do the same would deadlock instead.
     *
     * @code
     * physics::World world;
     * world.add_static_box({ 0.0F, -1.0F, 0.0F }, { 50.0F, 1.0F, 50.0F });
     * const physics::BodyId crate = world.add_dynamic_box({ 0.0F, 4.0F, 0.0F },
     *                                                     { 0.5F, 0.5F, 0.5F });
     * world.step(1.0F / 60.0F);
     * const Vec3 where = world.body_position(crate);
     * @endcode
     */
    class World {
    public:
        /**
         * @brief Creates the world and points Box3D at the job system.
         *
         * @param worker_count How many workers to tell Box3D about, or 0 to use
         *                     what the job system actually has. Pass 1 to run the
         *                     solver on the calling thread alone.
         *
         * @warning A count above what jobs::worker_count() reports does not make
         * the solver faster. Box3D splits its work that many ways and the pool
         * runs the pieces as it can, so the extra pieces only add overhead.
         *
         * @note **Below about 130 bodies, one worker is cheaper than the pool.**
         * The scheduling costs more than the work it splits. The default stays on
         * the pool anyway, because the loss there is 0.014 ms in a frame of 16.7,
         * and the win above the crossover is 2.7 times. DESIGN.md section 5.1
         * holds the measurement.
         */
        explicit World(std::uint32_t worker_count = 0);

        /// @brief Destroys the world and every body in it.
        ~World();

        World(const World&) = delete;
        World& operator=(const World&) = delete;
        World(World&&) = delete;
        World& operator=(World&&) = delete;

        /**
         * @brief Advances the simulation by one step.
         *
         * @param delta_seconds How far to advance. A fixed value gives a
         *                      repeatable simulation, which is what M7.4 builds on.
         * @param substeps How many solver iterations to run inside the step. More
         *                 substeps make a stack settle harder and cost time.
         */
        void step(float delta_seconds, std::uint32_t substeps = 4);

        /**
         * @brief Adds a body with no shape on it yet.
         *
         * A body with no shape still falls, and nothing stops it. Add a shape
         * with add_box() or add_sphere().
         *
         * @param type What moves it. See BodyType.
         * @param position Where it starts, in world space.
         * @param rotation How it is turned, in world space.
         * @return The new body.
         */
        BodyId add_body(BodyType type, const Vec3& position, const Quat& rotation);

        /**
         * @brief Puts a box on a body, centered on it.
         * @param body The body to add it to.
         * @param half_extents Half the size along each axis, in meters.
         * @param material The density, the friction, and the restitution.
         * @param options Whether it is a trigger, and what it reports as.
         */
        void add_box(BodyId body, const Vec3& half_extents, const SurfaceMaterial& material,
                     const ShapeOptions& options = {});

        /**
         * @brief Puts a sphere on a body, centered on it.
         * @param body The body to add it to.
         * @param radius How far it reaches, in meters.
         * @param material The density, the friction, and the restitution.
         * @param options Whether it is a trigger, and what it reports as.
         */
        void add_sphere(BodyId body, float radius, const SurfaceMaterial& material,
                        const ShapeOptions& options = {});

        /**
         * @brief Takes every shape off a body and leaves the body in place.
         *
         * This is what a resize needs. Box3D fixes the size of a shape when it
         * creates it, so a collider that has to change size becomes a new shape
         * on the same body. Destroying the body instead would lose the velocity
         * and the contacts, so a crate resized in the inspector would stop dead
         * and drop.
         *
         * The mass is not recomputed here, because a body with no shape has
         * none to compute. Add the new shapes and then call
         * apply_mass_from_shapes().
         *
         * @param body The body to strip.
         */
        void clear_shapes(BodyId body);

        /**
         * @brief Works the mass out from the shapes a body now carries.
         *
         * add_box() and add_sphere() each update the mass as they go, so this
         * is only needed after clear_shapes() has emptied a body and the new
         * shapes have gone on.
         *
         * @param body The body to recompute.
         */
        void apply_mass_from_shapes(BodyId body);

        /**
         * @brief Moves a body, whatever its type.
         *
         * This teleports rather than pushes. Contacts are worked out afresh
         * where the body lands, so a body moved into another one resolves the
         * overlap rather than sweeping through what lies between.
         *
         * @param body The body to move.
         * @param position Where to put it, in world space.
         * @param rotation How to turn it, in world space.
         */
        void set_body_transform(BodyId body, const Vec3& position, const Quat& rotation);

        /**
         * @brief Gives a kinematic body the velocity that reaches a transform.
         *
         * A kinematic body pushes dynamic bodies aside, and it needs a velocity
         * to do that with. Teleporting one leaves it with no velocity, so a
         * crate resting on a lift would be left behind rather than carried.
         *
         * @param body The body to move.
         * @param position Where it should arrive, in world space.
         * @param rotation How it should be turned on arrival.
         * @param delta_seconds The step it has to get there in.
         */
        void set_body_target(BodyId body, const Vec3& position, const Quat& rotation,
                             float delta_seconds);

        /**
         * @brief Sets how fast a body is moving.
         *
         * This replaces the velocity rather than adding to it, so a body given
         * one twice moves at the second one. Use it to launch a body that was
         * just created, which is what throwing something is.
         *
         * A velocity that is not zero wakes the body, so a throw disturbs a
         * stack that has gone to sleep.
         *
         * @warning **A velocity of zero on a sleeping body does nothing.** A
         * sleeping body holds no velocity state to write, and Box3D wakes one
         * only for a velocity that is not zero. The body is already still, so
         * the result is what the caller wanted, but a caller expecting the
         * write itself to happen is expecting something that did not.
         *
         * @param body The body to move. A static body ignores this.
         * @param velocity Meters each second, in world space.
         */
        void set_linear_velocity(BodyId body, const Vec3& velocity);

        /**
         * @brief Sets how fast a body is turning.
         *
         * This replaces the spin rather than adding to it. A reset needs it:
         * a teleport that left the body turning would put a crate back in its
         * place and leave it rolling out of it.
         *
         * @warning **The zero case carries the same trap as
         * set_linear_velocity().** A sleeping body holds no velocity state, so
         * writing zero to one does nothing. Wake it first when the write itself
         * has to happen.
         *
         * @param body The body to turn. A static body ignores this.
         * @param velocity Radians each second, about each world axis.
         */
        void set_angular_velocity(BodyId body, const Vec3& velocity);

        /**
         * @brief How fast a body is moving now.
         *
         * A sleeping body reads as zero, which is what it is doing. So this
         * says whether a body is moving, and it never says whether it is awake.
         *
         * @param body The body to read.
         * @return Meters each second, in world space.
         */
        [[nodiscard]] Vec3 linear_velocity(BodyId body) const;

        /**
         * @brief Adds momentum to a body at its center of mass.
         *
         * An impulse is a change of momentum rather than a speed, so a heavy
         * body moves less than a light one for the same push. That is what a
         * game means by a hit or a kick, where set_linear_velocity() is what it
         * means by a throw.
         *
         * @param body The body to push. A static body ignores this.
         * @param impulse Kilogram meters each second, in world space.
         * @param wake Whether to wake a sleeping body first. A sleeping body
         *             ignores an impulse otherwise, and says nothing.
         */
        void apply_linear_impulse(BodyId body, const Vec3& impulse, bool wake = true);

        /**
         * @brief Whether the solver is still working on this body.
         *
         * A body that has come to rest goes to sleep and costs no solver time
         * until something disturbs it.
         *
         * @param body The body to ask about.
         * @return True while it is awake.
         */
        [[nodiscard]] bool is_awake(BodyId body) const;

        /**
         * @brief Wakes a sleeping body, or puts an awake one to sleep.
         *
         * @warning **A sleeping body ignores a velocity of zero, silently.**
         * `b3Body_SetLinearVelocity` wakes a body before it writes, but only
         * when the velocity is not zero, so a zero on a sleeping body does
         * nothing at all. A caller that means to stop a body it may have sent
         * to sleep has to wake it first. See DESIGN.md section 5.
         *
         * @param body The body to change.
         * @param awake True to wake it, false to put it to sleep.
         */
        void set_awake(BodyId body, bool awake);

        /**
         * @brief Every trigger overlap that began or ended in the last step.
         *
         * Box3D buffers these inside the step and hands them over afterwards,
         * so one step can report several and this reports all of them. The
         * event data is transient, which is why this copies rather than
         * handing back a view.
         *
         * @param out Receives the events. Cleared first.
         */
        void sensor_events(std::vector<TouchEvent>& out) const;

        /**
         * @brief Every contact that began or ended in the last step.
         *
         * @warning **Every shape this class creates reports contacts, whether
         * anything reads them or not.** Box3D buffers a begin and an end event
         * for each pair that touches, so a settling stack pays for events
         * nobody asks for. There is no opt-in, and that was measured rather
         * than assumed: on a stack of 2881 bodies the flags cost less than the
         * run-to-run spread of one configuration. The numbers are next to the
         * flags, in `to_box3d` in `world.cpp`.
         *
         * @param out Receives the events. Cleared first.
         */
        void contact_events(std::vector<TouchEvent>& out) const;

        /// @brief Destroys one body and every shape on it.
        /// @param body The body to destroy.
        void destroy_body(BodyId body);

        /**
         * @brief Where a body is now.
         * @param body The body to read.
         * @return Its center, in world space.
         */
        [[nodiscard]] Vec3 body_position(BodyId body) const;

        /**
         * @brief How a body is turned now.
         * @param body The body to read.
         * @return Its rotation, in world space.
         */
        [[nodiscard]] Quat body_rotation(BodyId body) const;

        /**
         * @brief Collects the wireframe of every collider in the world.
         *
         * The shapes come from Box3D, so this draws what the solver is actually
         * simulating. A collider that does not match its mesh shows up here as
         * a wireframe in the wrong place, which is the whole reason to have it.
         *
         * **A sleeping body draws too, and in its own color.** Box3D reads the
         * broad phase rather than the list of moving bodies, and it picks the
         * color from the body state. A body that settled in the wrong place is
         * the one most worth seeing, so this would be worth little without it.
         *
         * The wireframe of each shape is built once, the first time that shape
         * is drawn, and kept until the shape changes or goes away. So a frame
         * that draws costs the transform of points that already exist.
         *
         * @param out Receives the lines. Cleared first, so a caller can hand
         *            the same vector back each frame and keep its memory.
         *
         * @warning Nothing outside kDebugDrawReach of the origin is reported.
         */
        void debug_lines(std::vector<DebugLine>& out) const;

        /// @brief How many bodies the world holds.
        /// @return The count of static and dynamic bodies together.
        [[nodiscard]] std::uint32_t body_count() const;

        /// @brief The worker count Box3D was given, after its own clamping.
        /// @return The worker count, which is never below 1 or above 32.
        [[nodiscard]] std::uint32_t worker_count() const;

    private:
        /// The Box3D world id, kept as an integer so this header names no Box3D
        /// type. world.cpp turns it back into a b3WorldId.
        std::uint64_t m_world = 0;
        std::uint32_t m_worker_count = 0;
        std::uint32_t m_body_count = 0;
    };

} // namespace engine::physics
