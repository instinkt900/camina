#pragma once

/**
 * @file
 * @brief The engine job system, wrapping enkiTS.
 *
 * This is the only scheduler in the engine. Box3D routes its task callbacks here
 * from M7, so physics and game logic share one worker pool instead of competing
 * for cores. See DESIGN.md section 5.1.
 */

#include <cstdint>
#include <functional>

/// @brief Parallel work scheduling.
namespace engine::jobs {

    /**
     * @brief The callback parallel_for() runs on each partition.
     *
     * The range is half open, so it covers [begin, end). The thread index
     * identifies the worker, which lets the callback write into a per-thread slot
     * with no locking.
     */
    using ParallelForFn =
        std::function<void(std::uint32_t begin, std::uint32_t end, std::uint32_t thread_index)>;

    /**
     * @brief Starts the scheduler and its worker threads.
     * @param thread_count Number of workers, or 0 for one per hardware core.
     */
    void init(std::uint32_t thread_count = 0);

    /// @brief Stops the workers and waits for them to finish.
    void shutdown();

    /// @brief The number of worker threads, which excludes the calling thread.
    /// @return The worker count, or 0 when the system is not running.
    [[nodiscard]] std::uint32_t worker_count();

    /**
     * @brief Splits work across the workers and blocks until every partition ends.
     *
     * The calling thread also takes a share of the work rather than idling.
     *
     * @param item_count Total number of items. Zero returns at once.
     * @param min_grain The smallest partition size. This stops the scheduler from
     *                  splitting cheap work into more pieces than it is worth.
     * @param fn The callback to run on each partition.
     */
    void parallel_for(std::uint32_t item_count, std::uint32_t min_grain, const ParallelForFn& fn);

    /**
     * @brief Splits work across the workers with no minimum partition size.
     * @param item_count Total number of items. Zero returns at once.
     * @param fn The callback to run on each partition.
     */
    inline void parallel_for(std::uint32_t item_count, const ParallelForFn& fn) {
        parallel_for(item_count, 1, fn);
    }

} // namespace engine::jobs
