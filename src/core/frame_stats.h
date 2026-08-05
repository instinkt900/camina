#pragma once

/**
 * @file
 * @brief Collects frame periods and reduces them to a few comparable numbers.
 */

#include <cstddef>
#include <vector>

namespace engine {

    /**
     * @brief What a run of frames cost, in milliseconds.
     *
     * Every value except the mean is a sample that really happened. See
     * FrameStats::summarize() for why that matters.
     */
    struct FrameSummary {
        std::size_t count = 0;  ///< How many samples the summary reduced.
        double mean_ms = 0.0;   ///< The average period.
        double median_ms = 0.0; ///< The middle period. Compare runs with this one.
        double p95_ms = 0.0;    ///< The period 95 percent of frames stayed under.
        double p99_ms = 0.0;    ///< The period 99 percent of frames stayed under.
        double low_ms = 0.0;    ///< The fastest frame.
        double high_ms = 0.0;   ///< The slowest frame, which is usually a hitch.
    };

    /**
     * @brief Records the period of each frame and reports the shape of the run.
     *
     * The first frames of a program are not the program. They build pipelines,
     * upload meshes, and fill the caches, so they run several times longer than
     * a settled frame. A mean over them measures the startup rather than the
     * renderer. This class drops a fixed count of leading samples for that
     * reason, and reports how many it dropped.
     *
     * @warning A period is wall time between the start of one drawn frame and
     *          the start of the next. It is not GPU time and it names no pass.
     *          With vsync on it measures the refresh rate and nothing else. See
     *          issue #131.
     *
     * @code
     * engine::FrameStats stats(60);
     * for (...) {
     *     stats.add(period_ms);
     * }
     * const engine::FrameSummary run = stats.summarize();
     * @endcode
     */
    class FrameStats {
    public:
        /**
         * @brief Creates a collector that ignores the first frames.
         * @param warmup_frames How many leading samples to drop. Zero keeps all
         *                      of them.
         */
        explicit FrameStats(std::size_t warmup_frames);

        /**
         * @brief Records one frame period.
         * @param milliseconds The wall time the frame took. A value below zero
         *                     is dropped, because a clock that went backwards
         *                     would drag the mean down and never be noticed.
         */
        void add(double milliseconds);

        /**
         * @brief Reduces the samples to a summary.
         *
         * A percentile here is the nearest rank, so the median of an even count
         * is the lower of the two middle samples rather than their average.
         * Every reported value except the mean is then a frame that the run
         * really produced. An average of two frames is a number that describes
         * neither, which is a poor thing to compare a change against.
         *
         * @return The summary. A run with no samples returns zeros.
         */
        [[nodiscard]] FrameSummary summarize() const;

        /// @brief How many samples the summary will reduce.
        /// @return The count of samples kept after the warm-up.
        [[nodiscard]] std::size_t counted() const { return samples_.size(); }

        /// @brief How many leading samples the warm-up dropped.
        /// @return The count of dropped samples, never more than the warm-up.
        [[nodiscard]] std::size_t dropped() const { return dropped_; }

    private:
        std::vector<double> samples_;
        std::size_t warmup_ = 0;
        std::size_t dropped_ = 0;
    };

} // namespace engine
