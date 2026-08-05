#include "core/frame_stats.h"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace engine {

    namespace {

        /**
         * Returns the sample at a rank, counting from the fastest frame.
         *
         * The rank is ceil(fraction * n), clamped into the array. So p95 of a
         * hundred samples is the ninety-fifth slowest, and p95 of one sample is
         * that sample. Rounding down instead would make the last percentile of
         * a short run unreachable, and a hitch is exactly what that percentile
         * exists to show.
         */
        double nearest_rank(const std::vector<double>& sorted, double fraction) {
            const auto count = static_cast<double>(sorted.size());
            const auto rank = static_cast<std::size_t>(std::ceil(fraction * count));
            const std::size_t index = std::clamp<std::size_t>(rank, 1, sorted.size()) - 1;
            return sorted[index];
        }

    } // namespace

    FrameStats::FrameStats(std::size_t warmup_frames)
        : warmup_(warmup_frames) {}

    void FrameStats::add(double milliseconds) {
        if (milliseconds < 0.0) {
            return;
        }
        if (dropped_ < warmup_) {
            ++dropped_;
            return;
        }
        samples_.push_back(milliseconds);
    }

    FrameSummary FrameStats::summarize() const {
        FrameSummary summary;
        if (samples_.empty()) {
            return summary;
        }

        std::vector<double> sorted = samples_;
        std::sort(sorted.begin(), sorted.end());

        summary.count = sorted.size();
        summary.mean_ms =
            std::accumulate(sorted.begin(), sorted.end(), 0.0) / static_cast<double>(sorted.size());
        // The fraction is the description of the field it fills, so naming each
        // one would give three constants that each appear once and would push
        // the number away from the thing it names.
        // NOLINTBEGIN(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)
        summary.median_ms = nearest_rank(sorted, 0.5);
        summary.p95_ms = nearest_rank(sorted, 0.95);
        summary.p99_ms = nearest_rank(sorted, 0.99);
        // NOLINTEND(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)
        summary.low_ms = sorted.front();
        summary.high_ms = sorted.back();
        return summary;
    }

} // namespace engine
