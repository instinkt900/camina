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
#include "sandbox/game.h"
#include "scene/component_registry.h"
#include "scene/components.h"
#include "scene/world.h"

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

    /// Where the view settings go. The working directory, so a run is easy to redo.
    /// The scene itself lives in the sandbox content directory, not here.
    constexpr const char* kViewPath = "view.json";

    struct Options {
        std::uint64_t max_frames = 0; ///< 0 means run until the user quits.
        bool validation = true;
        /// Where the game reads its content. Empty means the compiled-in default.
        std::string content;
    };

    /**
     * How the runtime looks at the world, and nothing about the world itself.
     *
     * The entities live in a scene::World that the sandbox loads. This struct
     * holds only the camera and the clear color, and it stays because it is the
     * M2 demonstration: the inspector builds its widgets from the descriptors
     * below, and the serializer writes the same fields to view.json. Neither one
     * names a field by hand.
     */
    struct ViewSettings {
        std::string name = "M3 sandbox view";
        engine::Vec3 clear_color{ 0.25F, 0.25F, 0.3F };

        float camera_distance = 7.0F;
        float camera_height = 2.5F;
        float fov_degrees = 60.0F;
        float orbit_seconds = 16.0F;

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
struct engine::reflect::Describe<ViewSettings> {
    static constexpr const char* name = "ViewSettings";
    static constexpr auto fields() {
        return std::make_tuple(
            ENGINE_FIELD(ViewSettings, name, Tooltip{ "Saved with the view settings" }),
            ENGINE_FIELD(ViewSettings, clear_color, Range{ 0.0, 1.0, 0.01 },
                         Tooltip{ "Linear, not sRGB" }),
            ENGINE_FIELD(ViewSettings, camera_distance, Range{ 1.0, 30.0, 0.05 }, Category{ "Camera" }),
            ENGINE_FIELD(ViewSettings, camera_height, Range{ -4.0, 4.0, 0.05 }, Category{ "Camera" }),
            ENGINE_FIELD(ViewSettings, fov_degrees, Range{ 20.0, 120.0, 0.5 }, Category{ "Camera" }),
            ENGINE_FIELD(ViewSettings, orbit_seconds, Range{ 1.0, 60.0, 0.1 }, Category{ "Camera" },
                         Tooltip{ "Seconds for one lap around the world" }),
            // ReadOnly keeps the editor from changing it. Transient keeps it out
            // of the file. The two attributes are read by different consumers,
            // and neither consumer knows about the other.
            ENGINE_FIELD(ViewSettings, frames_drawn, ReadOnly{}, Transient{}, Category{ "Debug" }));
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
            } else if (arg == "--content" && i + 1 < argc) {
                options.content = argv[i + 1];
                ++i;
            } else if (arg == "--no-validation") {
                options.validation = false;
            }
        }
        return options;
    }

    /**
     * Builds the matrix that turns a world position into clip space.
     *
     * The camera orbits the origin, so every side of the scene passes it. A
     * model matrix is not part of this: each entity supplies its own, and
     * scene::World has already composed it. Reverse-Z means the near plane maps
     * to depth 1, and perspective_reverse_z already negates the Y row for
     * Vulkan clip space.
     */
    engine::Mat4 view_projection(const ViewSettings& settings, float seconds,
                                 engine::gfx::Extent2D extent) {
        const float aspect = extent.height == 0
                                 ? 1.0F
                                 : static_cast<float>(extent.width) /
                                       static_cast<float>(extent.height);
        const engine::Mat4 projection = engine::perspective_reverse_z(
            glm::radians(settings.fov_degrees), aspect, kNearPlane);

        const float orbit = kTwoPi * seconds / settings.orbit_seconds;
        const engine::Vec3 eye{ settings.camera_distance * std::sin(orbit),
                                settings.camera_height,
                                settings.camera_distance * std::cos(orbit) };
        const engine::Mat4 view =
            glm::lookAt(eye, engine::Vec3{ 0.0F, 0.0F, 0.0F }, engine::world_up);

        return projection * view;
    }

    /**
     * Draws the view window: the generated inspector and the save and load
     * buttons.
     *
     * Nothing here names a field of ViewSettings. Add a field to the struct and
     * to its description, and it appears in this window and in the file.
     */
    void draw_view_window(ViewSettings& settings) {
        ENGINE_PROFILE_ZONE_N("draw_view_window");

        if (ImGui::Begin("View")) {
            if (engine::reflect::inspect(settings)) {
                // A real editor marks the document dirty here. The demo only
                // needs to show that the inspector reports a change.
                ENGINE_LOG_TRACE("The user changed the view.");
            }

            ImGui::Separator();

            if (ImGui::Button("Save")) {
                if (engine::reflect::save_json(kViewPath, settings)) {
                    ENGINE_LOG_INFO("Wrote {}.", kViewPath);
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Load")) {
                if (engine::reflect::load_json(kViewPath, settings)) {
                    ENGINE_LOG_INFO("Read {}.", kViewPath);
                }
            }
            ImGui::SameLine();
            ImGui::TextDisabled("%s", kViewPath);
        }
        ImGui::End();
    }

    /**
     * Draws what the world holds.
     *
     * This window reads the world the sandbox loaded. It names no game type, so
     * a different game in the same runtime shows the same list. Selecting an
     * entity and editing its components arrives with the rest of M3.3.
     */
    void draw_world_window(const engine::scene::World& world) {
        ENGINE_PROFILE_ZONE_N("draw_world_window");

        if (ImGui::Begin("World")) {
            ImGui::Text("Entities: %zu", world.size());
            ImGui::Text("Matrices rebuilt last frame: %zu", world.rebuilt_last_update());

            ImGui::Separator();
            ImGui::TextDisabled("Hierarchy");

            const entt::registry& entities = world.registry();
            for (const auto [entity, node] : entities.view<const engine::scene::Hierarchy>().each()) {
                if (node.parent != entt::null) {
                    continue;
                }
                const auto* named = entities.try_get<engine::scene::Name>(entity);
                ImGui::BulletText("%s (%zu children)",
                                  named != nullptr ? named->value.c_str() : "unnamed",
                                  node.child_count);
            }

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
        ViewSettings* settings = nullptr;
        const engine::scene::World* world = nullptr;
    };

    FrameOutcome draw_frame(const FrameContext& context, engine::gfx::Extent2D extent,
                            float seconds, engine::gfx::Extent2D& out_extent) {
        engine::gfx::Device* device = context.device;
        ViewSettings& settings = *context.settings;
        const engine::scene::World& world = *context.world;

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
        draw_view_window(settings);
        draw_world_window(world);

        const engine::gfx::ColorRGBA clear{ settings.clear_color.r, settings.clear_color.g,
                                            settings.clear_color.b, 1.0F };
        engine::gfx::cmd_begin_rendering(info.commands, clear);

        // One cube for each entity, at the matrix World composed for it. Until
        // M4 brings meshes, every entity is a cube, and that is the whole
        // renderer. Nothing here asks what a component means.
        const engine::Mat4 clip_from_world = view_projection(settings, seconds, info.extent);
        for (const auto [entity, placed] :
             world.registry().view<const engine::scene::WorldTransform>().each()) {
            context.pass->draw(info.commands, clip_from_world * placed.matrix);
        }

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
    if (!window.create({ .title = "Camina Engine (M3 sandbox)" })) {
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

    ViewSettings settings;
    engine::reflect::registry().add<ViewSettings>();
    // A view file next to the executable wins over the defaults, so a run
    // continues where the last one stopped.
    if (std::filesystem::exists(kViewPath) && engine::reflect::load_json(kViewPath, settings)) {
        ENGINE_LOG_INFO("Read {}.", kViewPath);
    }

    // The engine registers what it defines, then the game registers what it
    // defines. A scene loaded before this loses every component nobody claimed.
    engine::scene::register_builtin_components();
    sandbox::register_components();

    const std::filesystem::path content =
        options.content.empty() ? sandbox::default_content_directory()
                                : std::filesystem::path{ options.content };

    engine::scene::World world;
    if (!sandbox::load(content, world)) {
        ENGINE_LOG_CRITICAL("The game did not load. There is nothing to draw.");
        window.set_event_hook(nullptr, nullptr);
        engine::gfx::imgui_shutdown(device);
        cube.destroy();
        engine::gfx::destroy_device(device);
        window.destroy();
        engine::jobs::shutdown();
        engine::log::shutdown();
        return 1;
    }

    const FrameContext context{
        .device = device, .pass = &cube, .settings = &settings, .world = &world
    };

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

        // The game moves things, then the world composes the matrices, then the
        // frame draws them. Reversing the first two would draw a frame behind.
        sandbox::update(world, seconds);
        world.update();

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
        settings.frames_drawn = frame;

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
