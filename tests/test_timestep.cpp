// M7.4 tests for the fixed step clock.
//
// This class decides how many times the simulation runs and what the render
// frame blends with. Both are easy to get subtly wrong and hard to see: a step
// count that drifts shows up as a simulation that runs slow on one machine, and
// an alpha that is wrong shows up as judder that reads like a rendering fault.
//
// Nothing here reads a clock. The test hands the class a delta and checks the
// count, so a run on a slow machine gives the same answer as a run on a fast
// one.

#include "check.h"
#include "core/timestep.h"

#include <cmath>
#include <cstdint>
#include <limits>

namespace {

    using engine::FixedTimestep;
    using test::check;
    using test::section;

    constexpr float kSixty = 60.0F;
    constexpr float kStep = 1.0F / kSixty;

    /// A tenth of a percent of one step. Float addition over a few hundred
    /// frames drifts by less than this, and every error the tests look for is
    /// larger by orders of magnitude.
    constexpr float kTolerance = 1e-3F;

    void a_frame_of_one_step_runs_one_step() {
        section("A frame worth exactly one step runs one step");

        FixedTimestep clock(kSixty);
        check(clock.step_seconds() == kStep, "the step is one sixtieth of a second");
        check(std::fabs(clock.rate_hz() - kSixty) < kTolerance, "and the rate reads back");

        check(clock.advance(kStep) == 1, "one step of time runs one step");
        check(clock.alpha() < kTolerance, "and nothing is left over");
    }

    void a_short_frame_runs_nothing_and_moves_alpha() {
        section("A frame shorter than one step runs nothing");

        FixedTimestep clock(kSixty);

        // This is the case the interpolation exists for. A display faster than
        // the step rate draws frames that run no step at all, and without a
        // blend every one of them draws the same pose.
        check(clock.advance(kStep * 0.25F) == 0, "a quarter step runs no step");
        check(std::fabs(clock.alpha() - 0.25F) < kTolerance, "alpha is a quarter of the way");

        check(clock.advance(kStep * 0.25F) == 0, "another quarter still runs none");
        check(std::fabs(clock.alpha() - 0.5F) < kTolerance, "alpha is halfway");

        check(clock.advance(kStep * 0.6F) == 1, "the frame that crosses the step runs it");
        check(std::fabs(clock.alpha() - 0.1F) < kTolerance, "and the remainder carries over");
    }

    void a_long_frame_runs_several_steps() {
        section("A frame longer than one step runs several");

        FixedTimestep clock(kSixty);
        check(clock.advance(kStep * 3.0F) == 3, "three steps of time run three steps");
        check(clock.alpha() < kTolerance, "with nothing left over");
    }

    void a_whole_second_runs_the_whole_rate() {
        section("One second of time runs exactly the rate in steps");

        // A step of 1/60 is not a number a float holds, so one second divided
        // by it gives 59.99998. Working the count out by division truncates
        // that to 59, and then a simulation loses a step of every second it
        // runs. The ceiling is raised here so nothing else can hide it.
        FixedTimestep clock(kSixty, 1000);
        check(clock.advance(1.0F) == 60, "a one second frame runs 60 steps and not 59");
        check(clock.drop_events() == 0, "and nothing was dropped");
    }

    void the_rate_does_not_follow_the_frame_rate() {
        section("The step count follows the time, not the frame count");

        // The same second of wall time, delivered in two very different frame
        // patterns. A simulation whose rate depended on the frame rate would
        // report two different counts here, and then the same scene would
        // behave differently on two machines.
        FixedTimestep fast(kSixty);
        for (std::uint32_t frame = 0; frame < 240; ++frame) {
            (void)fast.advance(1.0F / 240.0F);
        }

        FixedTimestep slow(kSixty);
        for (std::uint32_t frame = 0; frame < 30; ++frame) {
            (void)slow.advance(1.0F / 30.0F);
        }

        check(fast.steps_taken() == 60, "240 frames of a second ran 60 steps");
        check(slow.steps_taken() == 60, "and so did 30 frames of the same second");
        check(fast.dropped_seconds() == 0.0, "neither dropped anything");
        check(slow.dropped_seconds() == 0.0, "at either frame rate");
    }

    void the_ceiling_stops_the_spiral() {
        section("The ceiling drops owed time rather than paying it back");

        FixedTimestep clock(kSixty, 5);

        // One frame that took a whole second. Without a ceiling this owes 60
        // steps, running them takes longer than a second, and the next frame
        // owes more than this one did. That is the spiral of death.
        check(clock.advance(1.0F) == 5, "a one second frame runs the ceiling and no more");
        check(clock.drop_events() == 1, "and it counted as one drop");

        // The dropped time is gone rather than owed, so the next frame is
        // normal. A ceiling that only deferred the work would run five more
        // steps here, and five more after that, until the owed time ran out.
        check(clock.advance(kStep) == 1, "the frame after it runs one step");
        check(clock.drop_events() == 1, "and drops nothing");

        // 55 steps of the 60 the second asked for.
        const double expected = 55.0 / static_cast<double>(kSixty);
        check(std::fabs(clock.dropped_seconds() - expected) < static_cast<double>(kTolerance),
              "the report names the simulated time the run will never make up");
    }

    void a_reset_forgets_the_pause() {
        section("A reset throws away time the simulation must not catch up on");

        FixedTimestep clock(kSixty);
        (void)clock.advance(kStep * 0.5F);
        (void)clock.advance(1.0F);

        clock.reset();
        check(clock.alpha() == 0.0F, "the leftover is gone");
        check(clock.dropped_seconds() == 0.0, "and so is the drop record");

        // A reset forgets a pause. It does not forget how the clock is set up,
        // and it does not forget what the run has already done. The one second
        // frame above hit the ceiling, so the count is the ceiling.
        check(clock.steps_taken() == engine::kDefaultMaxStepsPerFrame,
              "the lifetime step count survives the reset");
        check(clock.max_steps() == engine::kDefaultMaxStepsPerFrame, "and so does the ceiling");
        check(std::fabs(clock.rate_hz() - kSixty) < kTolerance, "and so does the rate");

        check(clock.advance(kStep) == 1, "the clock still runs");
    }

    void a_rate_change_keeps_the_time() {
        section("Changing the rate keeps the time already accumulated");

        FixedTimestep clock(kSixty);
        (void)clock.advance(kStep * 0.5F);

        // Half of a sixtieth is a whole thirtieth-of-a-second's worth of nothing
        // yet, so the leftover seconds are unchanged and alpha halves.
        clock.set_rate_hz(30.0F);
        check(std::fabs(clock.step_seconds() - (1.0F / 30.0F)) < kTolerance,
              "the step doubles");
        check(std::fabs(clock.alpha() - 0.25F) < kTolerance,
              "and the leftover is a quarter of the longer step");
    }

    void a_bad_setting_is_clamped_rather_than_fatal() {
        section("A rate out of range is clamped");

        // These arrive from a settings file a person edits. A rate of zero
        // gives a step of infinity and a negative one runs the simulation
        // backwards, and neither should stop the application.
        FixedTimestep zero(0.0F);
        check(zero.step_seconds() > 0.0F, "a rate of zero gives a step that can advance");
        check(zero.advance(2.0F) >= 1, "and the clock still runs");

        FixedTimestep negative(-60.0F);
        check(negative.step_seconds() > 0.0F, "a negative rate gives a forward step");

        FixedTimestep no_ceiling(kSixty, 0);
        check(no_ceiling.max_steps() >= 1, "a ceiling of zero is raised to one");
        check(no_ceiling.advance(1.0F) == 1, "so the simulation still advances");
    }

    void a_non_finite_value_does_not_stop_the_clock() {
        section("A rate or a delta that is not a number is refused");

        const float not_a_number = std::numeric_limits<float>::quiet_NaN();
        const float infinity = std::numeric_limits<float>::infinity();

        // std::clamp compares, and every comparison against a NaN is false, so
        // it hands a NaN straight back. A NaN step, or a NaN in the
        // accumulator, then fails the "is a step owed" test for ever, and the
        // simulation stops with no message at all.
        //
        // The two guards catch different values. A NaN delta already fails the
        // "greater than zero" test, so the one in advance() is what refuses an
        // infinite delta. Deleting either guard fails a check below.
        FixedTimestep bad_rate(not_a_number);
        check(std::fabs(bad_rate.rate_hz() - engine::kDefaultStepHz) < kTolerance,
              "a rate that is not a number falls back to the default");
        check(bad_rate.advance(1.0F) >= 1, "and that clock advances");

        FixedTimestep clock(kSixty);
        (void)clock.advance(kStep * 0.5F);
        const float before = clock.alpha();

        check(clock.advance(not_a_number) == 0, "a delta that is not a number runs no step");
        check(clock.advance(infinity) == 0, "and neither does an infinite one");
        check(clock.alpha() == before, "the accumulator did not move");
        check(clock.advance(kStep * 0.5F) == 1, "and the clock still runs afterwards");
    }

    void a_backwards_clock_owes_nothing() {
        section("A delta at or below zero adds nothing");

        FixedTimestep clock(kSixty);
        (void)clock.advance(kStep * 0.5F);
        const float before = clock.alpha();

        check(clock.advance(0.0F) == 0, "a zero delta runs no step");
        check(clock.advance(-1.0F) == 0, "and neither does a negative one");
        check(clock.alpha() == before, "the accumulator did not move");
    }

} // namespace

int main() {
    a_frame_of_one_step_runs_one_step();
    a_short_frame_runs_nothing_and_moves_alpha();
    a_long_frame_runs_several_steps();
    a_whole_second_runs_the_whole_rate();
    the_rate_does_not_follow_the_frame_rate();
    the_ceiling_stops_the_spiral();
    a_reset_forgets_the_pause();
    a_rate_change_keeps_the_time();
    a_bad_setting_is_clamped_rather_than_fatal();
    a_non_finite_value_does_not_stop_the_clock();
    a_backwards_clock_owes_nothing();

    return test::report();
}
