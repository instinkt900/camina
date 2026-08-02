#include "core/arena.h"
#include "core/jobs.h"
#include "core/log.h"
#include "core/profile.h"
#include "core/version.h"
#include "gfx/device.h"
#include "gfx/imgui.h"
#include "math/conventions.h"
#include "platform/window.h"
#include "reflect/inspector.h"
#include "reflect/json.h"
#include "reflect/registry.h"
#include "render/cube_pass.h"

#include <imgui.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>
#include <thread>

namespace {

    constexpr std::size_t kFrameArenaBytes = 4U * 1024U * 1024U;
    constexpr int kDecimalBase = 10;
    /// About one frame at 60 Hz. Long enough to idle, short enough to wake fast.
    constexpr int kMinimizedSleepMs = 16;

    constexpr float kNearPlane = 0.1F;
    constexpr float kTwoPi = 6.2831853F;

    /// Where the scene file goes. The working directory, so a run is easy to redo.
    constexpr const char* kScenePath = "scene.json";

    struct Options {
        std::uint64_t max_frames = 0; ///< 0 means run until the user quits.
        bool validation = true;
    };

    /**
     * Everything the M2 demo lets the user change.
     *
     * This is the struct both reflection consumers read. The inspector builds
     * its widgets from the descriptors below, and the serializer writes the same
     * fields to scene.json. Neither one names a field by hand.
     */
    struct Scene {
        std::string name = "M2 demo scene";
        engine::Vec3 clear_color{ 0.25F, 0.25F, 0.3F };

        bool spin = true;
        float spin_seconds = 5.0F;

        float camera_distance = 3.0F;
        float camera_height = 1.2F;
        float fov_degrees = 60.0F;
        float orbit_seconds = 8.0F;

        std::uint64_t frames_drawn = 0;
    };

} // namespace

// The description sits outside the anonymous namespace, because a template
// specialization has to live in the namespace of the template it specializes.
// The numbers in a Range are the description. Naming each slider bound would
// give twelve constants that each appear once, and would push the number away
// from the field it belongs to.
// NOLINTBEGIN(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)
template <>
struct engine::reflect::Describe<Scene> {
    static constexpr const char* name = "Scene";
    static constexpr auto fields() {
        return std::make_tuple(
            ENGINE_FIELD(Scene, name, Tooltip{ "Saved with the scene" }),
            ENGINE_FIELD(Scene, clear_color, Range{ 0.0, 1.0, 0.01 },
                         Tooltip{ "Linear, not sRGB" }),
            ENGINE_FIELD(Scene, spin, Category{ "Cube" }),
            ENGINE_FIELD(Scene, spin_seconds, Range{ 0.5, 30.0, 0.1 }, Category{ "Cube" },
                         Tooltip{ "Seconds for one full turn" }),
            ENGINE_FIELD(Scene, camera_distance, Range{ 1.0, 12.0, 0.05 }, Category{ "Camera" }),
            ENGINE_FIELD(Scene, camera_height, Range{ -4.0, 4.0, 0.05 }, Category{ "Camera" }),
            ENGINE_FIELD(Scene, fov_degrees, Range{ 20.0, 120.0, 0.5 }, Category{ "Camera" }),
            ENGINE_FIELD(Scene, orbit_seconds, Range{ 1.0, 60.0, 0.1 }, Category{ "Camera" },
                         Tooltip{ "Seconds for one lap around the cube" }),
            // ReadOnly keeps the editor from changing it. Transient keeps it out
            // of the file. The two attributes are read by different consumers,
            // and neither consumer knows about the other.
            ENGINE_FIELD(Scene, frames_drawn, ReadOnly{}, Transient{}, Category{ "Debug" }));
    }
};
// NOLINTEND(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)

namespace {

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

    /**
     * Builds the matrix the cube shader reads.
     *
     * The camera orbits the origin and the cube turns on its own axis, so every
     * face passes the camera. Reverse-Z means the near plane maps to depth 1, and
     * perspective_reverse_z already negates the Y row for Vulkan clip space.
     */
    engine::Mat4 cube_mvp(const Scene& scene, float seconds, engine::gfx::Extent2D extent) {
        const float aspect = extent.height == 0
                                 ? 1.0F
                                 : static_cast<float>(extent.width) /
                                       static_cast<float>(extent.height);
        const engine::Mat4 projection = engine::perspective_reverse_z(
            glm::radians(scene.fov_degrees), aspect, kNearPlane);

        const float orbit = kTwoPi * seconds / scene.orbit_seconds;
        const engine::Vec3 eye{ scene.camera_distance * std::sin(orbit), scene.camera_height,
                                scene.camera_distance * std::cos(orbit) };
        const engine::Mat4 view =
            glm::lookAt(eye, engine::Vec3{ 0.0F, 0.0F, 0.0F }, engine::world_up);

        const float turn = scene.spin ? kTwoPi * seconds / scene.spin_seconds : 0.0F;
        const engine::Mat4 model =
            glm::rotate(engine::Mat4{ 1.0F }, turn, engine::Vec3{ 0.0F, 1.0F, 0.0F });

        return projection * view * model;
    }

    /**
     * Draws the M2 window: the generated inspector, the save and load buttons,
     * and the registry contents.
     *
     * Nothing here names a field of Scene. Add a field to the struct and to its
     * description, and it appears in this window and in the file.
     */
    void draw_ui(Scene& scene) {
        ENGINE_PROFILE_ZONE_N("draw_ui");

        if (ImGui::Begin("Scene")) {
            if (engine::reflect::inspect(scene)) {
                // A real editor marks the document dirty here. The demo only
                // needs to show that the inspector reports a change.
                ENGINE_LOG_TRACE("The user changed the scene.");
            }

            ImGui::Separator();

            if (ImGui::Button("Save")) {
                if (engine::reflect::save_json(kScenePath, scene)) {
                    ENGINE_LOG_INFO("Wrote {}.", kScenePath);
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Load")) {
                if (engine::reflect::load_json(kScenePath, scene)) {
                    ENGINE_LOG_INFO("Read {}.", kScenePath);
                }
            }
            ImGui::SameLine();
            ImGui::TextDisabled("%s", kScenePath);

            ImGui::Separator();
            ImGui::Text("Registered types: %zu", engine::reflect::registry().size());
            for (const engine::reflect::TypeInfo& type : engine::reflect::registry().types()) {
                ImGui::BulletText("%s: %zu fields, %zu bytes", type.name, type.field_count,
                                  type.size);
            }
        }
        ImGui::End();
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

    /// What draw_frame() needs that does not change from one frame to the next.
    struct FrameContext {
        engine::gfx::Device* device = nullptr;
        const engine::render::CubePass* pass = nullptr;
        Scene* scene = nullptr;
    };

    FrameOutcome draw_frame(const FrameContext& context, engine::gfx::Extent2D extent,
                            float seconds, engine::gfx::Extent2D& out_extent) {
        engine::gfx::Device* device = context.device;
        Scene& scene = *context.scene;

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

        // The overlay opens after the frame does, so a skipped frame never
        // leaves an ImGui frame half open.
        engine::gfx::imgui_new_frame();
        draw_ui(scene);

        const engine::gfx::ColorRGBA clear{ scene.clear_color.r, scene.clear_color.g,
                                            scene.clear_color.b, 1.0F };
        engine::gfx::cmd_begin_rendering(info.commands, clear);
        context.pass->draw(info.commands, cube_mvp(scene, seconds, info.extent));
        engine::gfx::imgui_render(info.commands);
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
    if (!window.create({ .title = "Camina Engine (M2)" })) {
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

    engine::render::CubePass cube;
    if (!cube.create(device)) {
        engine::gfx::destroy_device(device);
        window.destroy();
        engine::jobs::shutdown();
        engine::log::shutdown();
        return 1;
    }

    result = engine::gfx::imgui_init(device, window.native());
    if (!engine::gfx::succeeded(result)) {
        ENGINE_LOG_CRITICAL("The overlay did not start: {}", engine::gfx::result_name(result));
        cube.destroy();
        engine::gfx::destroy_device(device);
        window.destroy();
        engine::jobs::shutdown();
        engine::log::shutdown();
        return 1;
    }

    // ImGui reads every event, and the window still acts on the ones it owns.
    window.set_event_hook(
        [](const void* event, void* /*user*/) { engine::gfx::imgui_process_event(event); },
        nullptr);

    Scene scene;
    engine::reflect::registry().add<Scene>();
    // A scene file next to the executable wins over the defaults, so a run
    // continues where the last one stopped.
    if (std::filesystem::exists(kScenePath) && engine::reflect::load_json(kScenePath, scene)) {
        ENGINE_LOG_INFO("Read {}.", kScenePath);
    }

    const FrameContext context{ .device = device, .pass = &cube, .scene = &scene };

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
        const FrameOutcome outcome = draw_frame(context, extent, seconds, drawn_extent);
        if (outcome == FrameOutcome::Failed) {
            failed = true;
            break;
        }
        if (outcome == FrameOutcome::Skipped) {
            continue;
        }

        ++frame;
        scene.frames_drawn = frame;

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

    // The resources must go before the device that owns them.
    engine::gfx::device_wait_idle(device);
    window.set_event_hook(nullptr, nullptr);
    engine::gfx::imgui_shutdown(device);
    cube.destroy();
    engine::gfx::destroy_device(device);
    window.destroy();
    engine::jobs::shutdown();
    ENGINE_LOG_INFO("Camina Engine stopped after {} frames.", frame);
    engine::log::shutdown();
    return failed ? 1 : 0;
}
