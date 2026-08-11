#include "core/jobs.h"

#include "core/assert.h"
#include "core/log.h"
#include "core/profile.h"

#include <enkiTS/TaskScheduler.h>

#include <array>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>

namespace engine::jobs {

    namespace {

        std::unique_ptr<enki::TaskScheduler> g_scheduler;

        /// Names each worker thread so that the profiler shows real names instead of
        /// thread ids. Tracy copies the string, and the buffer is thread local, so the
        /// pointer stays valid either way.
        constexpr std::size_t kThreadNameSize = 32;

        void on_thread_start(std::uint32_t thread_index) {
            static thread_local std::array<char, kThreadNameSize> name{};
            std::snprintf(name.data(), name.size(), "worker %u", thread_index);
            ENGINE_PROFILE_THREAD(name.data());
        }

        /// How many tasks can be live at once. Box3D is the caller that sets this
        /// number: it queues at most B3_MAX_TASKS, which is 256, in one world step,
        /// and it says so in its own header for exactly this reason. A fixed pool
        /// then needs no allocation and no growth.
        constexpr std::size_t kTaskPoolSize = 256;

        /// A free list over a fixed pool. The lock is held only long enough to move
        /// one index, and never while the task runs.
        std::mutex g_task_mutex;
        std::vector<std::uint32_t> g_free_tasks;

    } // namespace

    /**
     * One enqueued task.
     *
     * enkiTS owns the run, so this only carries the work and the slot it came
     * from. The slot index is what wait() gives back to the free list, which
     * saves working the address back into an index.
     */
    struct Task : enki::ITaskSet {
        TaskFn fn = nullptr;
        void* context = nullptr;
        const char* name = nullptr;
        std::uint32_t slot = 0;

        void ExecuteRange(enki::TaskSetPartition /*range*/, std::uint32_t /*thread*/) override {
            ENGINE_PROFILE_ZONE_N("task");
            if (name != nullptr) {
                ENGINE_PROFILE_ZONE_TEXT(name, std::strlen(name));
            }
            fn(context);
        }
    };

    namespace {

        /// The pool itself. It sits below Task because it holds Task by value, and
        /// it is a pointer so that shutdown() can drop it with the scheduler.
        std::unique_ptr<std::array<Task, kTaskPoolSize>> g_tasks;

    } // namespace

    void init(std::uint32_t thread_count) {
        ENGINE_CHECK(g_scheduler == nullptr, "The job system is already running.");

        enki::TaskSchedulerConfig config;
        if (thread_count != 0) {
            config.numTaskThreadsToCreate = thread_count;
        }
        config.profilerCallbacks.threadStart = on_thread_start;

        g_scheduler = std::make_unique<enki::TaskScheduler>();
        g_scheduler->Initialize(config);

        g_tasks = std::make_unique<std::array<Task, kTaskPoolSize>>();
        g_free_tasks.clear();
        g_free_tasks.reserve(kTaskPoolSize);
        for (std::size_t index = 0; index < kTaskPoolSize; ++index) {
            // Every task runs one item. Box3D splits its own work and hands over
            // one piece at a time, so a range here would only be a range of one.
            (*g_tasks)[index].m_SetSize = 1;
            (*g_tasks)[index].slot = static_cast<std::uint32_t>(index);
            g_free_tasks.push_back(static_cast<std::uint32_t>(kTaskPoolSize - 1 - index));
        }

        ENGINE_LOG_INFO("Job system started with {} worker threads.", worker_count());
    }

    void shutdown() {
        if (g_scheduler != nullptr) {
            g_scheduler->WaitforAllAndShutdown();
            g_scheduler.reset();

            // After the scheduler stops, no task can still be running, so the pool
            // goes with it.
            g_tasks.reset();
            g_free_tasks.clear();

            ENGINE_LOG_INFO("Job system stopped.");
        }
    }

    std::uint32_t worker_count() {
        return g_scheduler != nullptr ? g_scheduler->GetNumTaskThreads() : 0U;
    }

    void parallel_for(std::uint32_t item_count, std::uint32_t min_grain, const ParallelForFn& fn) {
        ENGINE_CHECK(g_scheduler != nullptr, "Call jobs::init before parallel_for.");

        if (item_count == 0) {
            return;
        }

        enki::TaskSet task(item_count,
                           [&fn](enki::TaskSetPartition range, std::uint32_t thread_index) {
                               ENGINE_PROFILE_ZONE_N("parallel_for partition");
                               fn(range.start, range.end, thread_index);
                           });
        task.m_MinRange = min_grain;

        g_scheduler->AddTaskSetToPipe(&task);
        g_scheduler->WaitforTask(&task);
    }

    Task* enqueue(TaskFn fn, void* context, const char* name) {
        ENGINE_CHECK(g_scheduler != nullptr, "Call jobs::init before enqueue.");
        ENGINE_CHECK(fn != nullptr, "enqueue needs work to run.");

        Task* task = nullptr;
        {
            const std::scoped_lock lock(g_task_mutex);
            if (!g_free_tasks.empty()) {
                task = &(*g_tasks)[g_free_tasks.back()];
                g_free_tasks.pop_back();
            }
        }

        // Every slot is live. Run the work here rather than refusing it or
        // allocating under the caller. The null return says so, and wait()
        // accepts it. See the warning on enqueue() in the header.
        if (task == nullptr) {
            ENGINE_PROFILE_ZONE_N("task inline");
            fn(context);
            return nullptr;
        }

        task->fn = fn;
        task->context = context;
        task->name = name;

        g_scheduler->AddTaskSetToPipe(task);
        return task;
    }

    void wait(Task* task) {
        if (task == nullptr) {
            return;
        }

        ENGINE_CHECK(g_scheduler != nullptr, "The job system stopped while a task was live.");

        // This runs other pending tasks while it blocks. That is what lets a task
        // wait on tasks it started itself without stalling its own worker.
        g_scheduler->WaitforTask(task);

        const std::scoped_lock lock(g_task_mutex);
        g_free_tasks.push_back(task->slot);
    }

    std::uint32_t task_capacity() {
        return static_cast<std::uint32_t>(kTaskPoolSize);
    }

} // namespace engine::jobs
