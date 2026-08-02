#include "core/arena.h"
#include "core/jobs.h"
#include "core/log.h"
#include "core/profile.h"
#include "core/version.h"
#include "gfx/device.h"
#include "math/conventions.h"
#include "platform/window.h"
#include "render/triangle_pass.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <string_view>
#include <thread>

namespace {

    constexpr std::size_t kFrameArenaBytes = 4U * 1024U * 1024U;
    constexpr int kDecimalBase = 10;
    /// About one frame at 60 Hz. Long enough to idle, short enough to wake fast.
    constexpr int kMinimizedSleepMs = 16;

    /// The clear color cycles so that a still frame still shows the loop is live.
    constexpr float kColorPeriodSeconds = 4.0F;
    constexpr float kColorCenter = 0.25F;
    constexpr float kColorSwing = 0.2F;
    constexpr float kTwoPi = 6.2831853F;
    /// Green trails red by a third of a turn, and blue trails green by the same.
    constexpr float kChannelPhaseStep = kTwoPi / 3.0F;

    struct Options {
        std::uint64_t max_frames = 0; ///< 0 means run until the user quits.
        bool validation = true;
    };

    Options parse_options(int argc, char** argv) {
        Options options;
        for (int i = 1; i < argc; ++i) {
            const std::string_view arg{ argv[i] };
            if (arg == "--frames" && i + 1 < argc) {
                options.max_frames = std::strtoull(argv[i + 1], nullptr, kDecimalBase);
                ++i;
            } else if (arg == "--no-validation") {
                options.validation = false;
            }
        }
        return options;
    }

    engine::gfx::ColorRGBA clear_color_at(float seconds) {
        const float phase = kTwoPi * seconds / kColorPeriodSeconds;
        return engine::gfx::ColorRGBA{
            kColorCenter + (kColorSwing * std::sin(phase)),
            kColorCenter + (kColorSwing * std::sin(phase + kChannelPhaseStep)),
            kColorCenter + (kColorSwing * std::sin(phase + kChannelPhaseStep + kChannelPhaseStep)),
            1.0F,
        };
    }

    engine::gfx::Extent2D window_extent(const engine::platform::Window& window) {
        return engine::gfx::Extent2D{ static_cast<std::uint32_t>(window.size().x),
                                      static_cast<std::uint32_t>(window.size().y) };
    }

    /// @return True when the swapchain now matches @p extent.
    bool rebuild_swapchain(engine::gfx::Device* device, engine::gfx::Extent2D extent) {
        const engine::gfx::Result result = engine::gfx::device_resize(device, extent);
        if (!engine::gfx::succeeded(result)) {
            ENGINE_LOG_CRITICAL("The swapchain did not rebuild: {}",
                                engine::gfx::result_name(result));
            return false;
        }
        return true;
    }

    /// What one pass through the render loop achieved.
    enum class FrameOutcome {
        Drawn,   ///< The frame reached the presentation engine.
        Skipped, ///< The swapchain was stale and has been rebuilt. Try again.
        Failed,  ///< The device reported an error the loop cannot handle.
    };

    FrameOutcome draw_frame(engine::gfx::Device* device, const engine::render::TrianglePass& pass,
                            engine::gfx::Extent2D extent, float seconds,
                            engine::gfx::Extent2D& out_extent) {
        engine::gfx::FrameInfo info;
        engine::gfx::Result result = engine::gfx::begin_frame(device, &info);

        if (result == engine::gfx::Result::OutOfDate) {
            return rebuild_swapchain(device, extent) ? FrameOutcome::Skipped
                                                     : FrameOutcome::Failed;
        }
        if (!engine::gfx::succeeded(result)) {
            ENGINE_LOG_CRITICAL("begin_frame failed: {}", engine::gfx::result_name(result));
            return FrameOutcome::Failed;
        }

        engine::gfx::cmd_begin_rendering(info.commands, clear_color_at(seconds));
        pass.draw(info.commands);
        engine::gfx::cmd_end_rendering(info.commands);

        result = engine::gfx::end_frame(device);
        if (result == engine::gfx::Result::OutOfDate) {
            // The frame did present. The swapchain is now stale or suboptimal, and
            // the window size alone does not report that, so rebuild here.
            if (!rebuild_swapchain(device, extent)) {
                return FrameOutcome::Failed;
            }
        } else if (!engine::gfx::succeeded(result)) {
            ENGINE_LOG_CRITICAL("end_frame failed: {}", engine::gfx::result_name(result));
            return FrameOutcome::Failed;
        }

        out_extent = info.extent;
        return FrameOutcome::Drawn;
    }

} // namespace

int main(int argc, char** argv) {
    const Options options = parse_options(argc, argv);

    engine::log::init();
    ENGINE_LOG_INFO("Camina Engine {} starting.", engine::Version);

    engine::jobs::init();
    engine::Arena frame_arena(kFrameArenaBytes);

    engine::platform::Window window;
    if (!window.create({ .title = "Camina Engine (M1)" })) {
        engine::jobs::shutdown();
        engine::log::shutdown();
        return 1;
    }

    engine::gfx::Device* device = nullptr;
    const engine::gfx::DeviceDesc device_desc{
        .window = window.native(),
        .app_name = "camina",
        .enable_validation = options.validation,
        .vsync = true,
    };

    engine::gfx::Result result = engine::gfx::create_device(device_desc, &device);
    if (!engine::gfx::succeeded(result)) {
        ENGINE_LOG_CRITICAL("The renderer did not start: {}", engine::gfx::result_name(result));
        window.destroy();
        engine::jobs::shutdown();
        engine::log::shutdown();
        return 1;
    }

    engine::render::TrianglePass triangle;
    if (!triangle.create(device)) {
        engine::gfx::destroy_device(device);
        window.destroy();
        engine::jobs::shutdown();
        engine::log::shutdown();
        return 1;
    }

    std::uint64_t frame = 0;
    bool failed = false;
    const auto started = std::chrono::steady_clock::now();
    auto last_report = started;
    engine::gfx::Extent2D last_extent = window_extent(window);

    while (window.poll()) {
        ENGINE_PROFILE_ZONE_N("frame");
        frame_arena.reset();

        if (window.minimized()) {
            // poll() does not block, so without this the loop pins one core while
            // the window is minimized and there is nothing to draw.
            std::this_thread::sleep_for(std::chrono::milliseconds(kMinimizedSleepMs));
            continue;
        }

        // The window reports its new size before the swapchain knows about it.
        const engine::gfx::Extent2D extent = window_extent(window);
        if (extent.width != last_extent.width || extent.height != last_extent.height) {
            if (!rebuild_swapchain(device, extent)) {
                failed = true;
                break;
            }
            last_extent = extent;
        }

        const auto now = std::chrono::steady_clock::now();
        const float seconds = std::chrono::duration<float>(now - started).count();

        engine::gfx::Extent2D drawn_extent{};
        const FrameOutcome outcome = draw_frame(device, triangle, extent, seconds, drawn_extent);
        if (outcome == FrameOutcome::Failed) {
            failed = true;
            break;
        }
        if (outcome == FrameOutcome::Skipped) {
            continue;
        }

        ++frame;

        if (now - last_report >= std::chrono::seconds(1)) {
            ENGINE_LOG_INFO("frame {} | {}x{} | arena high water {} bytes | workers {}", frame,
                            drawn_extent.width, drawn_extent.height, frame_arena.high_water(),
                            engine::jobs::worker_count());
            last_report = now;
        }

        if (options.max_frames != 0 && frame >= options.max_frames) {
            ENGINE_LOG_INFO("Frame limit of {} reached. Exiting.", options.max_frames);
            break;
        }

        ENGINE_PROFILE_FRAME();
    }

    // The pipeline must go before the device that owns it.
    engine::gfx::device_wait_idle(device);
    triangle.destroy();
    engine::gfx::destroy_device(device);
    window.destroy();
    engine::jobs::shutdown();
    ENGINE_LOG_INFO("Camina Engine stopped after {} frames.", frame);
    engine::log::shutdown();
    return failed ? 1 : 0;
}
