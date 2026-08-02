#include "core/jobs.h"

#include "core/assert.h"
#include "core/log.h"
#include "core/profile.h"

#include <enkiTS/TaskScheduler.h>

#include <array>
#include <cstdio>
#include <memory>

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

        ENGINE_LOG_INFO("Job system started with {} worker threads.", worker_count());
    }

    void shutdown() {
        if (g_scheduler != nullptr) {
            g_scheduler->WaitforAllAndShutdown();
            g_scheduler.reset();
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

} // namespace engine::jobs
