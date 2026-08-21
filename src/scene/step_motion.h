#pragma once

/**
 * @file
 * @brief Blends what the fixed step moved into the pose a frame draws.
 *
 * `physics::Simulation` does this for a rigid body: the solver owns the pose,
 * and a frame between two steps draws a blend of the last two. Game logic on
 * the fixed step has exactly the same problem and no solver to ask, so this is
 * the same mechanism with the game as the author instead.
 *
 * DESIGN.md section 9 holds the rule. Game logic runs on the fixed step, which
 * is what makes it reproducible, and what it moves is interpolated for the
 * render so a display faster than the step rate does not judder.
 *
 * This names no physics type and no game type. It records transforms.
 */

// core/entt.h points ENTT_ASSERT at ENGINE_ASSERT, and it has to come before any
// EnTT header. It fails the build with a message when the order is wrong.
#include "core/entt.h"
#include "math/transform.h"

#include <entt/entity/fwd.hpp>

#include <cstddef>
#include <unordered_map>

namespace engine::scene {

    class World;

    /**
     * @brief The poses of everything the fixed step moves, and the blend for a
     *        frame.
     *
     * The pose it records is the authoritative one. The transform on the entity
     * is whatever the last frame drew, which is a blend and not a step result.
     * So a step has to put the authoritative pose back before the game reads
     * it, and that is what begin_step() is for.
     *
     * Nothing here is written to a scene file. This is the state of a run, not
     * something a person authors, so it is a plain class rather than a
     * reflected component.
     *
     * @code
     * for (std::uint32_t left = clock.advance(delta); left > 0; --left) {
     *     motion.begin_step(world);
     *     seconds += clock.step_seconds();
     *     game::update(world, seconds, motion);  // calls record() on what it moves
     * }
     * motion.interpolate(world, clock.alpha());
     * @endcode
     */
    class StepMotion {
    public:
        /**
         * @brief Puts every tracked entity back on the pose the last step left.
         *
         * Call this before the game logic of a step runs. Without it the game
         * reads the blend the last frame drew, so the motion of a frame that
         * fell between two steps would be fed back in and compound.
         *
         * It also marks the step as running, which is what record() reads.
         *
         * @param world The scene to write.
         */
        void begin_step(World& world);

        /**
         * @brief Records where a step left one entity.
         *
         * Call it for each entity the step moved, after moving it. An entity
         * seen for the first time records the same pose twice, so the first
         * frame after it starts moving blends two copies rather than reading a
         * pose nobody set.
         *
         * @warning **A record outside a step writes both halves**, so the pose
         * is drawn rather than blended towards. A paused game runs no step, and
         * a pose written there is authoritative rather than the newer half of a
         * pair: blending it would draw the entity part way to where it was put,
         * and it would stay there for as long as the pause lasts.
         * `physics::Simulation::teleport` writes both halves for the same
         * reason. See issue #409.
         *
         * begin_step() and interpolate() are what mark the two, so nothing has
         * to tell this class whether the game is paused.
         *
         * @param world The scene to read the local transform from.
         * @param entity The entity that moved.
         */
        void record(const World& world, entt::entity entity);

        /**
         * @brief Writes the pose a frame should draw.
         *
         * The blend of the last two steps, the same way
         * `physics::Simulation::interpolate` works. Call it once for each
         * frame, after the steps that frame ran.
         *
         * It also marks the step as finished, which is what record() reads.
         *
         * @param world The scene to write.
         * @param alpha How far the frame sits into the step that has not run,
         *              from 0 to 1. `engine::FixedTimestep::alpha()` gives
         *              this. Pass 1 to draw the newest step with no blending.
         */
        void interpolate(World& world, float alpha);

        /**
         * @brief Stops tracking one entity.
         * @param entity The entity to forget. One that is not tracked is fine.
         */
        void forget(entt::entity entity);

        /// @brief Forgets everything, which is what a reloaded scene needs.
        void clear();

        /// @brief How many entities are tracked.
        /// @return The count.
        [[nodiscard]] std::size_t tracked() const;

    private:
        /**
         * Forgets every entity the world no longer holds.
         *
         * A reloaded scene clears the registry and EnTT hands the same numbers
         * out again, so a stale handle can name an entity somebody else now
         * owns. Writing a pose into that one is silent and wrong.
         *
         * @param world The scene to ask.
         */
        void drop_dead(const World& world);

        /// The two poses a frame blends between, for one entity.
        struct Pose {
            Transform previous; ///< Where the step before the last one left it.
            Transform current;  ///< Where the last step left it.
        };

        std::unordered_map<entt::entity, Pose> m_poses;

        /**
         * Whether a step is running, which decides what record() writes.
         *
         * begin_step() opens one and interpolate() closes it. So a paused frame,
         * which runs neither of them, leaves this false and a record there is
         * taken as authoritative. See record().
         */
        bool m_in_step = false;
    };

} // namespace engine::scene
