// The editor application. It is a second program over engine_core, not a mode
// of the runtime, which is what rule 4.3 in DESIGN.md asks for. The game module
// links into both, so this program knows the project types and can show them.
//
// M9.1 is the shell: a window, the ImGui overlay with docking on, a menu bar,
// and a layout that survives a restart. It draws no scene yet. M9.3 puts the
// scene in the central node of the dockspace, and M9.2 brings the panels that
// live in apps/runtime/main.cpp today.

#include "core/log.h"
#include "core/version.h"
#include "gfx/device.h"
#include "gfx/imgui.h"
#include "physics/components.h"
#include "platform/paths.h"
#include "platform/window.h"
#include "render/render_graph.h"
#include "sandbox/game.h"
#include "scene/component_registry.h"
#if defined(ENGINE_WITH_LUA)
#include "script/components.h"
#endif

#include <imgui.h>

#include <array>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <system_error>
#include <span>
#include <string>
#include <string_view>
#include <thread>

// The target defines this and nothing else does. It carries no code today, and
// reflect::EditorOnly is the first consumer. See issue #305. A build that lost
// the define would compile and quietly stop dropping editor-only code, so say
// so here rather than find out later.
#if !defined(ENGINE_WITH_EDITOR)
#error "The editor must be built with ENGINE_WITH_EDITOR. See apps/editor/CMakeLists.txt."
#endif

namespace {

    constexpr const char* kWindowTitle = "Camina Editor";

    /// The name under the user preferences directory. See platform paths.
    constexpr const char* kApplicationName = "editor";

    /// What ImGui calls its layout file. The path is built at start.
    constexpr const char* kLayoutFile = "imgui.ini";

    /// How long to wait before polling again while the window is minimized.
    constexpr int kMinimizedSleepMs = 16;

    /// The editor draws nothing behind the panels yet, so the frame clears to
    /// one flat color. M9.3 puts the scene here.
    constexpr engine::gfx::ColorRGBA kFrameClear{ 0.05F, 0.05F, 0.06F, 1.0F };

    /// What the command line asked for.
    struct Options {
        /// How many frames to draw before stopping. Zero runs until the user quits.
        std::uint64_t frames = 0;
        bool validation = true;       ///< The Khronos validation layer.
        bool sync_validation = false; ///< The barrier checks on top of it.
        bool vsync = true;
    };

    void print_usage() {
        ENGINE_LOG_INFO("Usage: editor [options]");
        ENGINE_LOG_INFO("  --frames <n>        Stop after n frames. 0 runs until you quit.");
        ENGINE_LOG_INFO("  --no-validation     Turn the Vulkan validation layer off.");
        ENGINE_LOG_INFO("  --sync-validation   Turn synchronization validation on.");
        ENGINE_LOG_INFO("  --no-vsync          Present without waiting for the display.");
        ENGINE_LOG_INFO("  --help              Print this and stop.");
    }

    /**
     * Reads a whole number, and reports whether the whole of it parsed.
     *
     * `std::from_chars` rather than strtoull, for the reason parse_count() in
     * apps/runtime/main.cpp gives: strtoull takes a leading sign and negates it,
     * so `--frames -1` would arrive as the largest unsigned value and read as a
     * run that never stops. It also stops at the first character it cannot use,
     * so `12bad` would parse as 12 and `nope` as 0, which is the value that
     * means "run until you quit".
     *
     * @param text The value given on the command line.
     * @param out Receives the count. Untouched unless the whole value parsed.
     * @return True when it parsed. On false the caller stops the program.
     */
    [[nodiscard]] bool parse_count(std::string_view text, std::uint64_t& out) {
        const char* last = text.data() + text.size();
        std::uint64_t value = 0;
        const std::from_chars_result parsed = std::from_chars(text.data(), last, value);
        if (parsed.ec != std::errc{} || parsed.ptr != last) {
            ENGINE_LOG_CRITICAL("--frames wants a whole number, and {} is not one.", text);
            return false;
        }
        out = value;
        return true;
    }

    /// Reads the command line. An unknown option stops the program, because a
    /// misspelled one that is ignored looks like an option that did nothing.
    [[nodiscard]] bool parse_options(int argc, char** argv, Options& out) {
        for (int i = 1; i < argc; ++i) {
            const std::string_view arg{ argv[i] };
            if (arg == "--help") {
                print_usage();
                return false;
            }
            if (arg == "--no-validation") {
                out.validation = false;
            } else if (arg == "--sync-validation") {
                out.sync_validation = true;
            } else if (arg == "--no-vsync") {
                out.vsync = false;
            } else if (arg == "--frames" && i + 1 < argc) {
                if (!parse_count(argv[++i], out.frames)) {
                    return false;
                }
            } else {
                ENGINE_LOG_CRITICAL("Unknown option: {}", arg);
                print_usage();
                return false;
            }
        }
        return true;
    }

    /// Which panels are open. ImGui saves this shape in the layout file, and
    /// these are the states a fresh installation starts from.
    struct Panels {
        bool components = true;
        bool demo = false;
        bool about = false;
    };

    /// Everything the editor owns for the whole run, in the order it is built.
    struct Editor {
        engine::platform::Window window;
        engine::gfx::Device* device = nullptr;
        /// Where ImGui saves the layout. ImGui keeps the pointer rather than a
        /// copy, so this string has to live as long as the overlay does.
        std::string layout_path;
        bool overlay = false; ///< True once ImGui owns resources on the device.
        /// What state each graph resource is in. Only the frame color is used
        /// today, and the array is the length derive_barriers() asks for.
        std::array<engine::gfx::ResourceState, engine::render::kFrameResourceCount> states{};
    };

    /**
     * Works out where the layout file goes.
     *
     * The preferences directory is the right home for it, because an installed
     * program usually cannot write beside its own executable. When the platform
     * will not say, the file goes next to the executable instead, which is
     * wrong for an installation and right for a build tree.
     */
    [[nodiscard]] std::string layout_path() {
        std::filesystem::path directory = engine::platform::preferences_directory(kApplicationName);
        if (directory.empty()) {
            directory = engine::platform::executable_directory();
        }
        return (directory / kLayoutFile).string();
    }

    [[nodiscard]] engine::gfx::Extent2D window_extent(const engine::platform::Window& window) {
        return engine::gfx::Extent2D{ static_cast<std::uint32_t>(window.size().x),
                                      static_cast<std::uint32_t>(window.size().y) };
    }

    [[nodiscard]] bool start(Editor& editor, const Options& options) {
        const engine::platform::WindowDesc window_desc{ .title = kWindowTitle };
        if (!editor.window.create(window_desc)) {
            return false;
        }

        const engine::gfx::DeviceDesc device_desc{
            .window = editor.window.native(),
            .app_name = "camina-editor",
            .enable_validation = options.validation,
            .enable_sync_validation = options.sync_validation,
            .vsync = options.vsync,
        };
        const engine::gfx::Result result = engine::gfx::create_device(device_desc, &editor.device);
        if (!engine::gfx::succeeded(result)) {
            ENGINE_LOG_CRITICAL("The renderer did not start: {}",
                                engine::gfx::result_name(result));
            return false;
        }

        // Docking and a layout file, which is what separates the editor overlay
        // from the runtime one. See DESIGN.md section 10, M9.
        editor.layout_path = layout_path();
        const engine::gfx::ImGuiDesc imgui_desc{
            .sdl_window = editor.window.native(),
            .docking = true,
            .ini_path = editor.layout_path.c_str(),
        };
        if (!engine::gfx::succeeded(engine::gfx::imgui_init(editor.device, imgui_desc))) {
            ENGINE_LOG_CRITICAL("The overlay did not start, so there is no editor.");
            return false;
        }
        editor.overlay = true;

        // ImGui reads every event, and the window still acts on the ones it owns.
        editor.window.set_event_hook(
            [](const void* event, void* /*user*/) { engine::gfx::imgui_process_event(event); },
            nullptr);
        return true;
    }

    /// Releases what start() built, in the opposite order. Safe after a partial start.
    void stop(Editor& editor) {
        if (editor.device != nullptr) {
            engine::gfx::device_wait_idle(editor.device);
        }
        editor.window.set_event_hook(nullptr, nullptr);
        if (editor.overlay) {
            engine::gfx::imgui_shutdown(editor.device);
        }
        if (editor.device != nullptr) {
            engine::gfx::destroy_device(editor.device);
        }
        editor.window.destroy();
    }

    /**
     * Draws the menu bar and reports whether the editor keeps running.
     *
     * It comes before the dockspace, because a main menu bar takes its height
     * out of the viewport work area and the dockspace fills what is left.
     */
    void draw_menu_bar(Panels& panels, bool& running) {
        if (!ImGui::BeginMainMenuBar()) {
            return;
        }
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("Exit", "Alt+F4")) {
                running = false;
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("View")) {
            ImGui::MenuItem("Components", nullptr, &panels.components);
            ImGui::Separator();
            ImGui::MenuItem("ImGui Demo", nullptr, &panels.demo);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Help")) {
            ImGui::MenuItem("About", nullptr, &panels.about);
            ImGui::EndMenu();
        }
        ImGui::EndMainMenuBar();
    }

    /**
     * Lists the component types the registry knows.
     *
     * This is the smallest honest use of the game module: the engine registers
     * what it defines, the game registers what it defines, and both appear
     * here. So the list is also the proof that one game compiles into two
     * applications.
     */
    void draw_components_window(bool& open) {
        if (!ImGui::Begin("Components", &open)) {
            ImGui::End();
            return;
        }

        const engine::scene::ComponentRegistry& registry = engine::scene::components();
        ImGui::Text("%zu component types are registered.", registry.size());
        ImGui::Separator();

        for (const engine::scene::ComponentOps& ops : registry.all()) {
            if (ImGui::TreeNode(ops.name)) {
                for (const char* field : ops.field_names) {
                    ImGui::BulletText("%s", field);
                }
                ImGui::TreePop();
            }
        }
        ImGui::End();
    }

    void draw_about_window(bool& open) {
        if (ImGui::Begin("About", &open, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("Camina Editor %s", engine::Version);
            ImGui::Separator();
            ImGui::TextUnformatted("Drag a panel by its tab to dock it anywhere.");
            ImGui::TextUnformatted("The layout is saved when the editor closes.");
        }
        ImGui::End();
    }

    /// Draws every panel of one frame, inside the open ImGui frame.
    void draw_ui(Panels& panels, bool& running) {
        draw_menu_bar(panels, running);

        // The whole work area is one dockspace, so a panel docks anywhere in
        // the window. The central node passes through to what the frame drew
        // behind it, which is the clear color today and the scene at M9.3.
        ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport(),
                                     ImGuiDockNodeFlags_PassthruCentralNode);

        if (panels.components) {
            draw_components_window(panels.components);
        }
        if (panels.about) {
            draw_about_window(panels.about);
        }
        if (panels.demo) {
            ImGui::ShowDemoWindow(&panels.demo);
        }
    }

    /// What one pass through the render loop achieved.
    enum class FrameOutcome {
        Drawn,   ///< The frame reached the presentation engine.
        Skipped, ///< The swapchain was stale and has been rebuilt. Try again.
        Failed,  ///< The device reported an error the loop cannot handle.
    };

    /**
     * Works out the barriers this frame needs.
     *
     * One pass, which writes the swapchain image. begin_frame() leaves that
     * image in ResourceState::Undefined and the graph is what moves it, the
     * same way the runtime does it. Going through the graph for one pass buys
     * nothing today and keeps the shape M9.3 needs.
     */
    [[nodiscard]] bool derive_frame_barriers(
        std::array<engine::gfx::ResourceState, engine::render::kFrameResourceCount>& states,
        engine::render::GraphSchedule& out) {
        const std::array writes{ engine::render::ResourceWrite{
            engine::render::kFrameColor, engine::gfx::ResourceState::ColorTarget } };
        const std::array passes{ engine::render::PassDesc{ .name = "editor overlay",
                                                           .reads = {},
                                                           .writes = writes } };

        // A new image on almost every acquire, so it carries no state from the
        // frame before.
        states[engine::render::kFrameColor.index] = engine::gfx::ResourceState::Undefined;
        if (!engine::render::derive_barriers(passes, states, out)) {
            ENGINE_LOG_CRITICAL("The frame declarations were refused, so no barrier is safe.");
            return false;
        }
        for (std::size_t i = 0; i < states.size(); ++i) {
            states[i] = out.final_states[i];
        }
        return true;
    }

    [[nodiscard]] FrameOutcome draw_frame(Editor& editor, Panels& panels, bool& running) {
        engine::gfx::FrameInfo info;
        engine::gfx::Result result = engine::gfx::begin_frame(editor.device, &info);
        if (result == engine::gfx::Result::OutOfDate) {
            result = engine::gfx::device_resize(editor.device, window_extent(editor.window));
            if (!engine::gfx::succeeded(result)) {
                ENGINE_LOG_CRITICAL("The swapchain did not rebuild: {}",
                                    engine::gfx::result_name(result));
                return FrameOutcome::Failed;
            }
            return FrameOutcome::Skipped;
        }
        if (!engine::gfx::succeeded(result)) {
            ENGINE_LOG_CRITICAL("begin_frame failed: {}", engine::gfx::result_name(result));
            return FrameOutcome::Failed;
        }

        // The overlay opens after the frame does, so a skipped frame never
        // leaves an ImGui frame half open.
        engine::gfx::imgui_new_frame();
        draw_ui(panels, running);

        engine::render::GraphSchedule schedule;
        if (!derive_frame_barriers(editor.states, schedule)) {
            return FrameOutcome::Failed;
        }
        for (const engine::render::GraphBarrier& barrier : schedule.passes[0].before) {
            engine::gfx::cmd_frame_barrier(info.commands, engine::gfx::FrameTarget::Color,
                                           barrier.before, barrier.after);
        }

        // No depth. Nothing draws geometry yet, and the overlay neither reads
        // nor writes it.
        engine::gfx::cmd_begin_rendering(info.commands, kFrameClear, false);
        engine::gfx::imgui_render(info.commands);
        engine::gfx::cmd_end_rendering(info.commands);

        result = engine::gfx::end_frame(editor.device);
        if (result == engine::gfx::Result::OutOfDate) {
            // The frame did present, and the swapchain is stale from here on.
            result = engine::gfx::device_resize(editor.device, window_extent(editor.window));
        }
        if (!engine::gfx::succeeded(result)) {
            ENGINE_LOG_CRITICAL("end_frame failed: {}", engine::gfx::result_name(result));
            return FrameOutcome::Failed;
        }
        return FrameOutcome::Drawn;
    }

    /// Runs frames until the user quits, the frame limit lands, or a frame fails.
    [[nodiscard]] bool run_frames(Editor& editor, const Options& options) {
        Panels panels;
        bool running = true;
        std::uint64_t frame = 0;
        engine::gfx::Extent2D last_extent = window_extent(editor.window);

        while (running && editor.window.poll()) {
            if (editor.window.minimized()) {
                // poll() does not block, so without this the loop pins one core
                // while there is nothing to draw.
                std::this_thread::sleep_for(std::chrono::milliseconds(kMinimizedSleepMs));
                continue;
            }

            const engine::gfx::Extent2D extent = window_extent(editor.window);
            if (extent.width != last_extent.width || extent.height != last_extent.height) {
                const engine::gfx::Result result =
                    engine::gfx::device_resize(editor.device, extent);
                if (!engine::gfx::succeeded(result)) {
                    ENGINE_LOG_CRITICAL("The swapchain did not rebuild: {}",
                                        engine::gfx::result_name(result));
                    return false;
                }
                last_extent = extent;
                // The swapchain was just rebuilt, so this frame has no image to
                // draw into. Drawing now would see OutOfDate and rebuild again.
                continue;
            }

            const FrameOutcome outcome = draw_frame(editor, panels, running);
            if (outcome == FrameOutcome::Failed) {
                return false;
            }
            if (outcome == FrameOutcome::Skipped) {
                continue;
            }

            ++frame;
            if (options.frames > 0 && frame >= options.frames) {
                ENGINE_LOG_INFO("Stopping after {} frames, as --frames asked.", frame);
                break;
            }
        }

        ENGINE_LOG_INFO("The editor stopped after {} frames.", frame);
        return true;
    }

} // namespace

int main(int argc, char** argv) {
    engine::log::init();

    Options options;
    if (!parse_options(argc, argv, options)) {
        engine::log::shutdown();
        return 1;
    }

    ENGINE_LOG_INFO("Camina Editor {} starting.", engine::Version);

    // The engine registers what it defines, then the game registers what it
    // defines. This is rule 4.3 made visible: the same game module the runtime
    // links tells the editor what its types are, so the editor can show them
    // without knowing one of them by name.
    //
    // The same four calls the runtime makes, in the same order. A registry that
    // held fewer types than the runtime's would drop a component from every
    // scene it saved.
    engine::scene::register_builtin_components();
    engine::physics::register_components();
#if defined(ENGINE_WITH_LUA)
    engine::script::register_components();
#endif
    sandbox::register_components();

    Editor editor;
    if (!start(editor, options)) {
        stop(editor);
        engine::log::shutdown();
        return 1;
    }

    const bool ok = run_frames(editor, options);

    stop(editor);
    engine::log::shutdown();
    return ok ? 0 : 1;
}
