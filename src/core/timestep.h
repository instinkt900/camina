#pragma once

/**
 * @file
 * @brief A fixed step clock, and the blend factor a render frame draws with.
 *
 * DESIGN.md section 9 settles the time model. Physics runs at a fixed rate, and
 * the render frame interpolates between the last two steps. This class is the
 * first half of that: it turns a variable frame delta into a whole number of
 * steps to run, and reports how far into the next step the frame sits.
 *
 * Nothing here knows about physics, and nothing here reads a clock. A caller
 * hands it a delta and it hands back a count, so a test drives it with no
 * timing at all.
 */

#include <cstdint>

namespace engine {

    /// @brief The step rate a FixedTimestep uses when nobody says otherwise.
    inline constexpr float kDefaultStepHz = 60.0F;

    /// @brief How many steps one frame runs by default before it drops time.
    inline constexpr std::uint32_t kDefaultMaxStepsPerFrame = 5;

    /// @brief The slowest step rate the class accepts, in hertz.
    inline constexpr float kSlowestStepHz = 1.0F;

    /// @brief The fastest step rate the class accepts, in hertz.
    inline constexpr float kFastestStepHz = 1000.0F;

    /**
     * @brief Turns a variable frame delta into whole fixed steps.
     *
     * A frame gives the class the time it took. The class adds that to an
     * accumulator and reports how many whole steps that time pays for. What is
     * left over stays in the accumulator, and alpha() reports it as a fraction
     * of one step. That fraction is what the renderer blends with.
     *
     * @warning **The step count has a ceiling, and the time past it is
     * dropped.** A frame that takes longer than one step leaves time owed.
     * Paying all of it back makes the next frame run several steps, which takes
     * longer still, which owes more time again. That is the spiral of death,
     * and it turns one slow frame into a stopped application. So a frame runs
     * at most max_steps() steps, and a frame that hits the ceiling throws away
     * every bit of time it did not run, down to and including the part shorter
     * than one step. The simulation then falls permanently behind wall time by
     * the amount dropped. dropped_seconds() reports the total, so a run can say
     * how much of it happened rather than leaving it silent.
     *
     * @code
     * engine::FixedTimestep clock(60.0F);
     * for (std::uint32_t step = clock.advance(frame_delta); step > 0; --step) {
     *     simulation.step(world, clock.step_seconds());
     * }
     * simulation.interpolate(world, clock.alpha());
     * @endcode
     */
    class FixedTimestep {
    public:
        /**
         * @brief Creates a clock at one rate with one ceiling.
         * @param rate_hz How many steps make one second. Clamped to
         *                kSlowestStepHz through kFastestStepHz.
         * @param max_steps The ceiling on steps in one frame. Clamped to at
         *                  least 1, because a ceiling of zero never advances.
         */
        explicit FixedTimestep(float rate_hz = kDefaultStepHz,
                               std::uint32_t max_steps = kDefaultMaxStepsPerFrame);

        /**
         * @brief Adds a frame of wall time and reports the steps it pays for.
         *
         * @param delta_seconds How long the frame took. A value at or below
         *                      zero adds nothing, because a clock that went
         *                      backwards would otherwise owe negative time.
         * @return How many steps to run now, never more than max_steps().
         */
        [[nodiscard]] std::uint32_t advance(float delta_seconds);

        /**
         * @brief How far the frame sits into the step that has not run yet.
         *
         * Call this after advance(). It is the weight the renderer gives the
         * newer of the two states it blends.
         *
         * @return A fraction from 0 to 1.
         */
        [[nodiscard]] float alpha() const;

        /// @brief How long one step covers, in seconds.
        /// @return The step length. This is what to pass the simulation.
        [[nodiscard]] float step_seconds() const { return step_seconds_; }

        /// @brief The step rate this clock runs at.
        /// @return The rate in hertz, after clamping.
        [[nodiscard]] float rate_hz() const;

        /// @brief The ceiling on steps in one frame.
        /// @return The ceiling, never below 1.
        [[nodiscard]] std::uint32_t max_steps() const { return max_steps_; }

        /**
         * @brief Changes the rate, keeping the time already accumulated.
         *
         * The leftover is kept as seconds rather than as a fraction of a step,
         * so a rate change moves alpha() but loses no time.
         *
         * @param rate_hz The new rate. Clamped the way the constructor clamps.
         */
        void set_rate_hz(float rate_hz);

        /// @brief Changes the ceiling on steps in one frame.
        /// @param max_steps The new ceiling. Clamped to at least 1.
        void set_max_steps(std::uint32_t max_steps);

        /// @brief How many steps this clock has reported since it was made.
        /// @return The running total.
        [[nodiscard]] std::uint64_t steps_taken() const { return steps_taken_; }

        /**
         * @brief How much owed time the ceiling has discarded.
         *
         * This is simulated time the application will never run. A run that
         * reports more than a few milliseconds here was too slow to keep up.
         *
         * @return The total in seconds.
         */
        [[nodiscard]] double dropped_seconds() const { return dropped_seconds_; }

        /// @brief How many frames hit the ceiling and dropped time.
        /// @return The count of such frames.
        [[nodiscard]] std::uint64_t drop_events() const { return drop_events_; }

        /**
         * @brief Throws away the leftover time and the drop record.
         *
         * Call this after a pause the simulation must not try to catch up on,
         * such as a load or a scene reload. The rate and the ceiling stay.
         */
        void reset();

    private:
        float step_seconds_ = 1.0F / kDefaultStepHz;
        std::uint32_t max_steps_ = kDefaultMaxStepsPerFrame;
        float accumulator_ = 0.0F;
        std::uint64_t steps_taken_ = 0;
        double dropped_seconds_ = 0.0;
        std::uint64_t drop_events_ = 0;
    };

} // namespace engine
