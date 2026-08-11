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

#include <cstdint>

namespace engine::physics {

    /// @brief Identifies one body inside a World. Zero is no body.
    using BodyId = std::uint64_t;

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
         */
        void add_box(BodyId body, const Vec3& half_extents, const SurfaceMaterial& material);

        /**
         * @brief Puts a sphere on a body, centered on it.
         * @param body The body to add it to.
         * @param radius How far it reaches, in meters.
         * @param material The density, the friction, and the restitution.
         */
        void add_sphere(BodyId body, float radius, const SurfaceMaterial& material);

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
