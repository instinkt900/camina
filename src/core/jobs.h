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

    /**
     * @brief How many threads can run tasks, counting the one that called init().
     *
     * The thread that starts the scheduler is one of the workers, because
     * parallel_for() and wait() both run work on the caller rather than leaving
     * it idle. So this is one more than the number of threads init() created.
     *
     * @return The worker count, or 0 when the system is not running.
     */
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

    /// @brief One enqueued task. The type is opaque, and only enqueue() makes one.
    struct Task;

    /// @brief The work enqueue() runs. It takes the context it was given.
    using TaskFn = void (*)(void* context);

    /**
     * @brief Starts one task on a worker and returns without waiting.
     *
     * This is the half of the scheduler that parallel_for() cannot express. A
     * caller that must start several pieces of work and only then block needs
     * the start and the wait to be separate calls. Box3D is the first such
     * caller, because its solver hands out tasks and joins them itself.
     *
     * A task takes a plain function pointer and a context rather than a
     * std::function, so starting one allocates nothing at all.
     *
     * @param fn The work to run. It must not be null.
     * @param context Passed to fn. This may be null, and the caller owns whatever
     *                it points at until wait() returns.
     * @param name Names the task in the profiler, or null for no name. The
     *             string is copied, so it does not have to outlive the call. A
     *             name longer than task_name_capacity() is cut.
     * @return A handle to pass to wait(), or null when the task already ran on
     *         the calling thread. See the warning below.
     *
     * @warning **A null return is a success, not a failure.** The pool holds a
     * fixed number of tasks. When every one of them is live, enqueue() runs the
     * work on the calling thread and returns null, because refusing the work or
     * allocating under the caller has no good failure path here. So every return
     * value is safe to pass to wait(), and none may be ignored.
     *
     * @warning Do not call this from inside a task and then wait on the result
     * from a thread that cannot run other work meanwhile. wait() does run other
     * tasks while it blocks, which is what keeps that case from deadlocking.
     */
    [[nodiscard]] Task* enqueue(TaskFn fn, void* context, const char* name);

    /**
     * @brief Waits for one enqueued task and releases it.
     *
     * The calling thread runs other pending tasks while it waits, rather than
     * idling. That is what lets a task wait on tasks it started itself.
     *
     * @param task The handle enqueue() returned. Null does nothing, because the
     *             work already ran.
     */
    void wait(Task* task);

    /// @brief How many tasks enqueue() can hold at once before it runs work inline.
    /// @return The size of the task pool.
    [[nodiscard]] std::uint32_t task_capacity();

    /**
     * @brief How many characters of a task name a slot keeps, without the terminator.
     *
     * enqueue() copies the name into the slot, so the caller may build it on the
     * stack. A longer name is cut to this length.
     *
     * @return The largest name enqueue() keeps whole.
     */
    [[nodiscard]] std::uint32_t task_name_capacity();

    /**
     * @brief The name a task kept.
     *
     * This is what makes the copy checkable. enqueue() keeps its own copy rather
     * than the caller's pointer, and a test can only tell the two apart by
     * overwriting the caller's buffer and asking the task what it holds.
     *
     * @param task The handle enqueue() returned. Null gives an empty string,
     *             because the work already ran and no slot holds a name.
     * @return The stored name, always null terminated. It stays valid until
     *         wait() releases the task.
     */
    [[nodiscard]] const char* task_name(const Task* task);

} // namespace engine::jobs
