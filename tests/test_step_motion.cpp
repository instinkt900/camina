/**
 * @file
 * @brief Game logic on the fixed step, and the blend a frame draws.
 *
 * Issue #245. DESIGN.md section 9 says game logic runs on the fixed step,
 * which is what makes a run reproducible, and that what it moves is
 * interpolated so a faster display does not judder.
 *
 * Nothing here opens a device. `scene::StepMotion` records transforms and
 * names no renderer type, so the whole file runs on any machine.
 */

#include "core/timestep.h"
#include "scene/step_motion.h"
#include "scene/world.h"

#include "check.h"

#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/quaternion.hpp>

#include <cmath>
#include <cstdint>

namespace {

    namespace sc = engine::scene;
    using engine::Transform;
    using engine::Vec3;

    using test::check;
    using test::section;

    constexpr float kTwoPi = 6.2831853F;

    /// One turn every four seconds, so a step of 1/60 moves a visible amount.
    constexpr double kSecondsPerTurn = 4.0;

    /// Metres each simulated second, for the entity that also travels.
    constexpr float kSpeed = 3.0F;

    /// Loose enough for a blend, which is arithmetic the direct form never does.
    constexpr float kBlendTolerance = 1e-5F;

    /**
     * The game this test drives. It turns and it travels, both as a function of
     * simulated seconds, and it records what it moved.
     */
    void run_game(sc::World& world, entt::entity entity, double seconds, sc::StepMotion& motion) {
        Transform local = world.local(entity);
        local.position = Vec3{ kSpeed * static_cast<float>(seconds), 0.0F, 0.0F };
        local.rotation = glm::angleAxis(
            static_cast<float>(static_cast<double>(kTwoPi) * seconds / kSecondsPerTurn),
            Vec3{ 0.0F, 1.0F, 0.0F });
        world.set_local(entity, local);
        motion.record(world, entity);
    }

    /**
     * Runs a session at one frame rate until the clock has taken `target`
     * steps, and returns the pose the last step left.
     *
     * The step count is the target rather than the frame count, because the
     * two are not interchangeable. Summing 1/144 over 288 frames lands just
     * under two seconds in float, so that session takes 119 steps where a 30 Hz
     * one takes 120. That is the frame delta accumulating, not the step
     * drifting, and it is why "the same wall time" is the wrong thing to hold
     * fixed. What determinism means here is that the same number of steps gives
     * the same answer whatever the frames did in between.
     */
    Transform run_session(float frame_delta, std::uint64_t target) {
        sc::World world;
        const entt::entity entity = world.create();

        engine::FixedTimestep clock(60.0F);
        sc::StepMotion motion;
        double seconds = 0.0;

        while (clock.steps_taken() < target) {
            for (std::uint32_t left = clock.advance(frame_delta); left > 0; --left) {
                motion.begin_step(world);
                seconds += static_cast<double>(clock.step_seconds());
                run_game(world, entity, seconds, motion);
            }
            // The frame draws a blend, which the next step undoes. Running it
            // here is what proves that undo works over a whole session.
            motion.interpolate(world, clock.alpha());
        }

        // The pose the last step left, not the blend the last frame drew. Two
        // frame rates land on different alphas, so the drawn pose differs by up
        // to one step of motion. That difference is the interpolation doing its
        // job and not a disagreement about the simulation.
        motion.begin_step(world);
        return world.local(entity);
    }

    void the_same_input_at_two_frame_rates_agrees() {
        section("The same input at two frame rates gives the same result");

        // A display at 30 Hz and one at 144, with the step rate 60 either way.
        constexpr std::uint64_t kSteps = 120;
        const Transform slow = run_session(1.0F / 30.0F, kSteps);
        const Transform fast = run_session(1.0F / 144.0F, kSteps);

        // Exactly, not nearly. The game is a function of the step count alone,
        // so there is no arithmetic here that could differ between the two.
        // Reading the frame delta or the wall clock is what would break this,
        // and that is what #245 changed.
        check(slow.position.x == fast.position.x,
              "the travelled distance is the same number, not merely close");
        check(slow.rotation == fast.rotation, "and so is the rotation");

        // A guard on the test itself. Two sessions that both did nothing would
        // pass everything above.
        check(slow.position.x > 1.0F, "and the session actually moved the entity");
    }

    void a_frame_between_two_steps_blends() {
        section("A frame that falls between two steps draws a blend");

        sc::World world;
        const entt::entity entity = world.create();

        sc::StepMotion motion;

        // Two steps by hand, so the pair the blend runs over is known.
        motion.begin_step(world);
        run_game(world, entity, 1.0, motion);
        motion.begin_step(world);
        run_game(world, entity, 2.0, motion);

        motion.interpolate(world, 0.0F);
        const float at_start = world.local(entity).position.x;
        motion.interpolate(world, 1.0F);
        const float at_end = world.local(entity).position.x;
        motion.interpolate(world, 0.5F);
        const float halfway = world.local(entity).position.x;

        check(std::fabs(at_start - kSpeed) < kBlendTolerance, "alpha 0 draws the older step");
        check(std::fabs(at_end - (kSpeed * 2.0F)) < kBlendTolerance, "alpha 1 draws the newer one");
        check(std::fabs(halfway - (kSpeed * 1.5F)) < kBlendTolerance,
              "and alpha 0.5 draws the middle, which is what removes the judder");
    }

    void a_step_reads_the_pose_the_last_step_left() {
        section("A step reads the last step, not the blend the last frame drew");

        sc::World world;
        const entt::entity entity = world.create();
        sc::StepMotion motion;

        motion.begin_step(world);
        run_game(world, entity, 1.0, motion);
        motion.begin_step(world);
        run_game(world, entity, 2.0, motion);

        // A frame drew a blend into the transform. The next step must not read
        // it back, or the motion of that frame compounds into the simulation
        // and the run stops being reproducible.
        motion.interpolate(world, 0.5F);
        check(std::fabs(world.local(entity).position.x - (kSpeed * 1.5F)) < kBlendTolerance,
              "the frame drew the blend");

        motion.begin_step(world);
        check(std::fabs(world.local(entity).position.x - (kSpeed * 2.0F)) < kBlendTolerance,
              "and the next step starts from the newest step instead");
    }

    void a_first_sighting_does_not_swing_in_from_the_origin() {
        section("An entity seen for the first time blends against itself");

        sc::World world;
        const entt::entity entity = world.create();
        world.set_local(entity, Transform{ .position = Vec3{ 100.0F, 0.0F, 0.0F } });

        sc::StepMotion motion;
        motion.record(world, entity);

        // Both halves start on the same pose. Blending against a default
        // transform would draw the entity halfway to the origin on its first
        // frame, which reads as a body teleporting in.
        motion.interpolate(world, 0.0F);
        check(std::fabs(world.local(entity).position.x - 100.0F) < kBlendTolerance,
              "alpha 0 draws where it is and not the origin");
    }

    void a_dead_entity_is_dropped() {
        section("An entity the world no longer holds is forgotten");

        sc::World world;
        const entt::entity entity = world.create();
        world.set_local(entity, Transform{ .position = Vec3{ 5.0F, 0.0F, 0.0F } });

        sc::StepMotion motion;
        motion.record(world, entity);
        check(motion.tracked() == 1, "it is tracked");

        // A hot reload clears the registry and reads the scene again. EnTT then
        // hands the same numbers out with a new version, so a stale handle can
        // name an entity somebody else now owns. Writing a pose into that one
        // is silent and wrong, and set_local does not check.
        world.clear();
        const entt::entity replacement = world.create();
        world.set_local(replacement, Transform{ .position = Vec3{ -9.0F, 0.0F, 0.0F } });

        motion.begin_step(world);
        check(motion.tracked() == 0, "and it is gone once the world no longer holds it");
        check(std::fabs(world.local(replacement).position.x + 9.0F) < kBlendTolerance,
              "so the entity that took its number keeps its own pose");

        motion.interpolate(world, 1.0F);
        check(std::fabs(world.local(replacement).position.x + 9.0F) < kBlendTolerance,
              "and the frame does not draw the old pose over it either");
    }

    void forgetting_and_clearing_work() {
        section("A tracked entity can be dropped");

        sc::World world;
        const entt::entity one = world.create();
        const entt::entity two = world.create();

        sc::StepMotion motion;
        motion.record(world, one);
        motion.record(world, two);
        check(motion.tracked() == 2, "both are tracked");

        motion.forget(one);
        check(motion.tracked() == 1, "forgetting drops one");
        motion.forget(one);
        check(motion.tracked() == 1, "and forgetting it twice is harmless");

        // A reloaded scene invalidates every entity it held, so the poses of
        // the old one must not survive into the new one.
        motion.clear();
        check(motion.tracked() == 0, "clear drops the rest");
    }

} // namespace

int main() {
    the_same_input_at_two_frame_rates_agrees();
    a_frame_between_two_steps_blends();
    a_step_reads_the_pose_the_last_step_left();
    a_first_sighting_does_not_swing_in_from_the_origin();
    a_dead_entity_is_dropped();
    forgetting_and_clearing_work();
    return test::report();
}
