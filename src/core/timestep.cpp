#include "core/timestep.h"

#include <algorithm>
#include <cmath>

namespace engine {

    namespace {

        /// Keeps a rate inside what the class can represent. A rate of zero
        /// gives a step of infinity, and a negative one runs the simulation
        /// backwards. Both arrive from a settings file a person edits, so this
        /// clamps rather than asserts.
        [[nodiscard]] float clamp_rate(float rate_hz) {
            if (!std::isfinite(rate_hz)) {
                return kDefaultStepHz;
            }
            return std::clamp(rate_hz, kSlowestStepHz, kFastestStepHz);
        }

    } // namespace

    FixedTimestep::FixedTimestep(float rate_hz, std::uint32_t max_steps)
        : step_seconds_(1.0F / clamp_rate(rate_hz))
        , max_steps_(std::max(max_steps, 1U)) {}

    std::uint32_t FixedTimestep::advance(float delta_seconds) {
        if (std::isfinite(delta_seconds) && delta_seconds > 0.0F) {
            accumulator_ += delta_seconds;
        }

        // Subtracting rather than dividing. A step is 1/60 of a second, which no
        // float holds exactly, so one whole second divided by it gives 59.99998
        // and truncates to 59. The loop cannot lose a step that way, and the
        // ceiling bounds it, so it never runs more than max_steps_ times.
        std::uint32_t steps = 0;
        while (steps < max_steps_ && accumulator_ >= step_seconds_) {
            accumulator_ -= step_seconds_;
            ++steps;
        }

        if (accumulator_ >= step_seconds_) {
            // The ceiling. What is left is thrown away rather than carried, so
            // the simulation falls behind instead of trying to catch up and
            // falling further behind still. All of it goes, including the part
            // shorter than a step, because a fraction of time the frame already
            // abandoned is not a position to blend towards.
            dropped_seconds_ += static_cast<double>(accumulator_);
            ++drop_events_;
            accumulator_ = 0.0F;
        }

        steps_taken_ += steps;
        return steps;
    }

    float FixedTimestep::alpha() const {
        // advance() leaves less than one step behind, so the clamp only catches
        // what float rounding puts a hair outside the range.
        return std::clamp(accumulator_ / step_seconds_, 0.0F, 1.0F);
    }

    float FixedTimestep::rate_hz() const {
        return 1.0F / step_seconds_;
    }

    void FixedTimestep::set_rate_hz(float rate_hz) {
        step_seconds_ = 1.0F / clamp_rate(rate_hz);
    }

    void FixedTimestep::set_max_steps(std::uint32_t max_steps) {
        max_steps_ = std::max(max_steps, 1U);
    }

    void FixedTimestep::reset() {
        accumulator_ = 0.0F;
        dropped_seconds_ = 0.0;
        drop_events_ = 0;
    }

} // namespace engine
