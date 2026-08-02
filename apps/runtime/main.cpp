#include "core/arena.h"
#include "core/assert.h"
#include "core/jobs.h"
#include "core/log.h"
#include "core/profile.h"
#include "core/version.h"
#include "math/conventions.h"
#include "platform/window.h"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <string_view>

namespace {

    constexpr std::size_t kFrameArenaBytes = 4U * 1024U * 1024U;
    constexpr std::uint32_t kWorkItems = 1U << 16U;
    constexpr std::uint32_t kJobGrain = 1024U;
    constexpr int kDecimalBase = 10;
    /// An arbitrary factor for the smoke test. Any value works. This one makes the
    /// expected sum easy to state in closed form.
    constexpr float kSmokeTestFactor = 0.5F;

    struct Options {
        std::uint64_t max_frames = 0; // 0 means run until the user quits.
    };

    Options parse_options(int argc, char** argv) {
        Options options;
        for (int i = 1; i < argc; ++i) {
            const std::string_view arg{ argv[i] };
            if (arg == "--frames" && i + 1 < argc) {
                options.max_frames = std::strtoull(argv[i + 1], nullptr, kDecimalBase);
                ++i;
            }
        }
        return options;
    }

    /// Proves that the job system runs work across the worker threads, and that the
    /// frame arena hands out scratch memory. M1 replaces this with real render work.
    void run_job_smoke_test(engine::Arena& arena) {
        ENGINE_PROFILE_ZONE_N("job smoke test");

        float* data = arena.allocate_n<float>(kWorkItems);
        ENGINE_CHECK(data != nullptr, "The frame arena is too small for the smoke test.");

        engine::jobs::parallel_for(
            kWorkItems, kJobGrain,
            [data](std::uint32_t begin, std::uint32_t end, std::uint32_t /*thread_index*/) {
                for (std::uint32_t i = begin; i < end; ++i) {
                    data[i] = static_cast<float>(i) * kSmokeTestFactor;
                }
            });

        double sum = 0.0;
        for (std::uint32_t i = 0; i < kWorkItems; ++i) {
            sum += static_cast<double>(data[i]);
        }

        // The sum of factor * i over [0, n) equals factor * n * (n - 1) / 2.
        const double expected = static_cast<double>(kSmokeTestFactor) *
                                static_cast<double>(kWorkItems) *
                                static_cast<double>(kWorkItems - 1U) / 2.0;
        ENGINE_CHECK(sum == expected, "The parallel_for result does not match the serial result.");
    }

} // namespace

int main(int argc, char** argv) {
    const Options options = parse_options(argc, argv);

    engine::log::init();
    ENGINE_LOG_INFO("Camina Engine {} starting.", engine::Version);

    engine::jobs::init();

    engine::Arena frame_arena(kFrameArenaBytes);

    engine::platform::Window window;
    if (!window.create({ .title = "Camina Engine (M0)" })) {
        engine::jobs::shutdown();
        engine::log::shutdown();
        return 1;
    }

    // Confirms that the projection convention compiles and produces the expected
    // reverse-Z layout. M1 sends this to the GPU.
    const engine::Mat4 projection =
        engine::perspective_reverse_z(glm::radians(60.0F), 16.0F / 9.0F, 0.1F);
    ENGINE_LOG_INFO("Reverse-Z projection built. Near plane maps to depth {}.", projection[3][2]);

    std::uint64_t frame = 0;
    auto last_report = std::chrono::steady_clock::now();

    while (window.poll()) {
        ENGINE_PROFILE_ZONE_N("frame");

        frame_arena.reset();
        run_job_smoke_test(frame_arena);

        ++frame;

        const auto now = std::chrono::steady_clock::now();
        if (now - last_report >= std::chrono::seconds(1)) {
            ENGINE_LOG_INFO("frame {} | window {}x{} | arena high water {} bytes | workers {}",
                            frame, window.size().x, window.size().y, frame_arena.high_water(),
                            engine::jobs::worker_count());
            last_report = now;
        }

        if (options.max_frames != 0 && frame >= options.max_frames) {
            ENGINE_LOG_INFO("Frame limit of {} reached. Exiting.", options.max_frames);
            break;
        }

        ENGINE_PROFILE_FRAME();
    }

    window.destroy();
    engine::jobs::shutdown();
    ENGINE_LOG_INFO("Camina Engine stopped after {} frames.", frame);
    engine::log::shutdown();
    return 0;
}
