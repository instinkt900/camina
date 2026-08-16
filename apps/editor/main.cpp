// The editor application. It is a second program over engine_core, not a mode
// of the runtime, which is what rule 4.3 in DESIGN.md asks for. The game module
// links into both, so this program knows the project types and can show them.
//
// M9.1 built the shell: a window, the ImGui overlay with docking on, a menu
// bar, and a layout that survives a restart. M9.2 opened the cooked content,
// read the scene, and docked the three panels out of src/editor/ around an
// empty middle.
//
// M9.3 fills that middle. The scene renders into an image of its own through
// render::SceneRenderer, the same passes the runtime draws, and the Viewport
// panel shows that image. The camera in the view panel is the camera it uses.

#include "assets/content.h"
#include "core/log.h"
#include "core/version.h"
#include "editor/panels.h"
#include "editor/view_settings.h"
#include "editor/viewport.h"
#include "gfx/device.h"
#include "gfx/imgui.h"
#include "physics/components.h"
#include "platform/paths.h"
#include "platform/window.h"
#include "render/scene_renderer.h"
#include "../screenshot.h"
#include "sandbox/game.h"
#include "reflect/json.h"
#include "scene/component_registry.h"
#include "scene/world.h"
#if defined(ENGINE_WITH_LUA)
#include "script/components.h"
#endif

#include <imgui.h>
// The dock builder is internal API. It is the only way to give a first run a
// layout, and every ImGui docking example uses it. See build_default_layout().
#include <imgui_internal.h>

#include <charconv>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <system_error>
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

    /// What the frame clears to behind the panels. The scene has its own image
    /// and its own clear color, so this shows through the gaps alone.
    constexpr engine::gfx::ColorRGBA kFrameClear{ 0.05F, 0.05F, 0.06F, 1.0F };

    /// What the command line asked for.
    struct Options {
        /// How many frames to draw before stopping. Zero runs until the user quits.
        std::uint64_t frames = 0;
        /// The cooked content to open. Empty takes the game's own directory.
        std::string content;
        /// Where to write the last frame as a PNG. Empty writes none.
        std::string screenshot;
        bool validation = true;       ///< The Khronos validation layer.
        bool sync_validation = false; ///< The barrier checks on top of it.
        bool vsync = true;
    };

    void print_usage() {
        ENGINE_LOG_INFO("Usage: editor [options]");
        ENGINE_LOG_INFO("  --content <dir>     Open this cooked content instead of the game's.");
        ENGINE_LOG_INFO("  --frames <n>        Stop after n frames. 0 runs until you quit.");
        ENGINE_LOG_INFO("  --screenshot <file> Write the last frame as a PNG and stop.");
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
            } else if (arg == "--content" && i + 1 < argc) {
                out.content = argv[++i];
            } else if (arg == "--screenshot" && i + 1 < argc) {
                out.screenshot = argv[++i];
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
        bool world = true;
        bool inspector = true;
        bool view = true;
        bool viewport = true;
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

        /// The engine's own cooked assets: the shaders and the split sum table.
        engine::assets::Content engine_content;
        /// The shadow, cull, mesh, and tonemap passes. The same ones the
        /// runtime draws, which is what keeps the two pictures the same.
        engine::render::SceneRenderer scene;
        /// The image the scene is tonemapped into, and what the panel shows.
        engine::editor::Viewport viewport;
        /// The size the Viewport panel reported on the last frame. The target
        /// follows it at the top of the next one, because an image cannot be
        /// rebuilt while a frame is recording.
        engine::gfx::Extent2D wanted_viewport{};

        /// The cooked content the scene reads. Open, or empty when there is none.
        engine::assets::Content content;
        /// The entities. Empty when no scene loaded, and the panels then say so.
        engine::scene::World world;
        /// The camera and the exposure, which the panel edits and the scene
        /// renders through.
        engine::editor::ViewSettings view;
        /// What the inspector shows, or entt::null for nothing.
        entt::entity selected = entt::null;
        /// The source scene the World panel saves to, or empty for no source tree.
        std::filesystem::path source_scene;
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

    /// What the device renders at, which is not always the size that was asked
    /// for. A surface can refuse a size, and the viewport target is clamped to
    /// this because the frame depth image is this size.
    [[nodiscard]] engine::gfx::Extent2D device_extent(engine::gfx::Device* device) {
        engine::gfx::Extent2D extent{};
        (void)engine::gfx::capture_frame(device, nullptr, 0, &extent);
        return extent;
    }

    /// The aspect ratio of an image, for engine::editor::view_projection().
    /// A zero height reads as square rather than dividing by zero.
    [[nodiscard]] float aspect_ratio(engine::gfx::Extent2D extent) {
        return extent.height == 0 ? 1.0F
                                  : static_cast<float>(extent.width) /
                                        static_cast<float>(extent.height);
    }

    [[nodiscard]] bool start(Editor& editor, const Options& options) {
        // The engine content tree holds the shaders, so it opens before the
        // device builds a pipeline out of them.
        if (!editor.engine_content.open(engine::platform::cooked_content_root() / "engine")) {
            ENGINE_LOG_CRITICAL("The engine content is missing. Build the cooker target.");
            return false;
        }

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

        // The scene passes, after the device. The image they tonemap into is
        // the size the swapchain settled on rather than the size asked for.
        const engine::gfx::Extent2D extent = device_extent(editor.device);
        if (!editor.scene.create(editor.device, editor.engine_content, extent)) {
            ENGINE_LOG_CRITICAL("The scene passes did not build, so nothing can draw.");
            return false;
        }

        // After the overlay, because the binding the panel draws through comes
        // out of the pool the overlay owns.
        if (!editor.viewport.create(editor.device, extent, extent)) {
            ENGINE_LOG_CRITICAL("The viewport target did not build.");
            return false;
        }
        editor.wanted_viewport = editor.viewport.extent();

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
        // Before the overlay goes, because the binding it holds is the
        // overlay's to return.
        editor.viewport.destroy();
        editor.scene.destroy();
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
            ImGui::MenuItem("Viewport", nullptr, &panels.viewport);
            ImGui::MenuItem("World", nullptr, &panels.world);
            ImGui::MenuItem("Inspector", nullptr, &panels.inspector);
            ImGui::MenuItem("View settings", nullptr, &panels.view);
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

    void draw_about_window(bool& open) {
        if (ImGui::Begin("About", &open, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("Camina Editor %s", engine::Version);
            ImGui::Separator();
            ImGui::TextUnformatted("Drag a panel by its tab to dock it anywhere.");
            ImGui::TextUnformatted("The layout is saved when the editor closes.");
        }
        ImGui::End();
    }

    /**
     * Puts each panel in its place, on a run that has no layout to restore.
     *
     * Without this every panel opens at the same spot and the last one drawn
     * buries the rest. The runtime overlay has the same problem and answers it
     * with fixed positions. The editor docks instead, so its answer is a
     * default layout.
     *
     * This runs once, on a first run alone. `DockBuilderGetNode` returns a node
     * as soon as the layout file holds one, so a person who moved a panel keeps
     * it there for every later run.
     *
     * The Viewport goes in the middle, which is what the two side splits leave.
     *
     * @param dockspace The dockspace to fill.
     */
    void build_default_layout(ImGuiID dockspace) {
        if (ImGui::DockBuilderGetNode(dockspace) != nullptr) {
            return;
        }

        constexpr float kSideColumn = 0.22F; ///< How much width each side takes.
        constexpr float kViewShare = 0.45F;  ///< How much of the left column the view takes.

        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::DockBuilderAddNode(dockspace, ImGuiDockNodeFlags_DockSpace);
        // The work area and not the whole viewport, or the top row of panels
        // sits over the main menu bar.
        ImGui::DockBuilderSetNodePos(dockspace, viewport->WorkPos);
        ImGui::DockBuilderSetNodeSize(dockspace, viewport->WorkSize);

        // Each split returns the node in the direction asked for and writes the
        // rest into the second output. Reading both matters: a node that has
        // been split is a parent, and docking a window into a parent rather
        // than into one of its leaves does not place the window.
        ImGuiID centre = dockspace;
        ImGuiID left = 0;
        ImGuiID right = 0;
        ImGui::DockBuilderSplitNode(centre, ImGuiDir_Left, kSideColumn, &left, &centre);
        ImGui::DockBuilderSplitNode(centre, ImGuiDir_Right, kSideColumn, &right, &centre);

        ImGuiID left_top = 0;
        ImGuiID left_bottom = 0;
        ImGui::DockBuilderSplitNode(left, ImGuiDir_Down, kViewShare, &left_bottom, &left_top);

        ImGui::DockBuilderDockWindow("World", left_top);
        ImGui::DockBuilderDockWindow("View", left_bottom);
        ImGui::DockBuilderDockWindow("Inspector", right);
        ImGui::DockBuilderDockWindow("Viewport", centre);
        ImGui::DockBuilderFinish(dockspace);
        ENGINE_LOG_INFO("No saved layout, so the editor built the default one.");
    }

    /// Draws every panel of one frame, inside the open ImGui frame.
    void draw_ui(Editor& editor, Panels& panels, bool& running) {
        draw_menu_bar(panels, running);

        // The whole work area is one dockspace, so a panel docks anywhere in
        // the window. The central node passes through to the clear color, which
        // shows wherever no panel covers it.
        //
        // The id is ours rather than the one DockSpaceOverViewport picks, so
        // that the builder below can name the same node.
        const ImGuiID dockspace = ImGui::GetID("CaminaEditorDockspace");
        build_default_layout(dockspace);
        ImGui::DockSpaceOverViewport(dockspace, ImGui::GetMainViewport(),
                                     ImGuiDockNodeFlags_PassthruCentralNode);

        // The panels, from src/editor/. Nothing here places them: the
        // dockspace does that, and the layout file remembers where the user put
        // each one.
        //
        // The viewport first, because what it reports is what the next frame
        // renders at, and the sooner that is known the shorter the mismatch.
        if (panels.viewport) {
            engine::editor::draw_viewport_panel(editor.viewport.picture(),
                                                editor.viewport.extent(),
                                                editor.wanted_viewport, &panels.viewport);
        }
        if (panels.world) {
            engine::editor::draw_world_panel(editor.world, editor.selected, editor.source_scene,
                                             editor.content, &panels.world);
        }
        if (panels.inspector) {
            engine::editor::draw_inspector_panel(editor.world, editor.selected,
                                                 &panels.inspector);
        }
        if (panels.view) {
            (void)engine::editor::draw_view_panel(editor.view, engine::editor::kViewSettingsFile,
                                                  &panels.view);
        }

        if (panels.about) {
            draw_about_window(panels.about);
        }
        if (panels.demo) {
            ImGui::ShowDemoWindow(&panels.demo);
        }

        // After the panels, because an edit in the inspector goes around
        // World::set_local() and leaves the matrices stale. The runtime does
        // the same thing in the same place and for the same reason.
        editor.world.update();
    }

    /// What one pass through the render loop achieved.
    enum class FrameOutcome {
        Drawn,   ///< The frame reached the presentation engine.
        Skipped, ///< The swapchain was stale and has been rebuilt. Try again.
        Failed,  ///< The device reported an error the loop cannot handle.
    };

    /**
     * Records and presents one frame.
     *
     * @param editor Everything the program owns.
     * @param panels Which panels are open.
     * @param running Cleared when the user asked to stop.
     * @param capture Whether to keep a copy of this frame, for --screenshot.
     * The request has to go in before end_frame() presents, because after that
     * the presentation engine owns the image.
     * @return What the frame achieved.
     */
    [[nodiscard]] FrameOutcome draw_frame(Editor& editor, Panels& panels, bool& running,
                                          bool capture) {
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

        // Resets the timestamp pool and reads what the frame before it wrote. A
        // pool that is written without a reset is a validation error, not a
        // wrong number.
        editor.scene.begin_frame(info.commands);

        // The overlay opens after the frame does, so a skipped frame never
        // leaves an ImGui frame half open.
        engine::gfx::imgui_new_frame();
        draw_ui(editor, panels, running);

        // The scene, into the image the panel shows. The same four passes the
        // runtime draws, and the same barriers, because both go through
        // render::SceneRenderer.
        //
        // The camera aspect comes from the target rather than from the window,
        // so what a person sees in the panel is the whole picture and not a
        // stretched one.
        const engine::gfx::Extent2D scene_extent = editor.viewport.extent();
        const engine::render::SceneView view{
            .clip_from_world =
                engine::editor::view_projection(editor.view, aspect_ratio(scene_extent)),
            .camera_position = editor.view.camera_position,
            .clear_color = { editor.view.clear_color.r, editor.view.clear_color.g,
                             editor.view.clear_color.b, 1.0F },
            .extent = scene_extent,
            .output = editor.viewport.target(),
        };
        if (!editor.scene.draw_scene(info.commands, editor.world, editor.content, view)) {
            return FrameOutcome::Failed;
        }

        // Black, because the full-screen triangle covers every pixel of the
        // target. The clear color a person picked belongs to the scene image
        // inside the renderer.
        // No depth, because the triangle neither reads nor writes it and the
        // tonemap pipeline declares no depth format. The frame depth image is
        // also the size of the window rather than of the panel.
        constexpr engine::gfx::ColorRGBA kSceneClear{ 0.0F, 0.0F, 0.0F, 1.0F };
        if (engine::gfx::cmd_begin_color_rendering(info.commands, editor.viewport.target(),
                                                   kSceneClear, false)) {
            editor.scene.draw_tonemap(info.commands, editor.view.exposure);
            engine::gfx::cmd_end_rendering(info.commands);
        }

        // Moves the picture to a shader read, so the overlay can sample it, and
        // the swapchain image to a color target.
        editor.scene.issue_output_barriers(info.commands);

        // No depth. The overlay draws flat, and the scene has its own depth
        // inside the passes above.
        engine::gfx::cmd_begin_rendering(info.commands, kFrameClear, false);
        engine::gfx::imgui_render(info.commands);
        engine::gfx::cmd_end_rendering(info.commands);

        if (capture) {
            engine::gfx::request_capture(editor.device);
        }

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

    /**
     * Makes the two scene images match what the Viewport panel asked for.
     *
     * Both are rebuilt together, because the passes render into the half float
     * one at the size the tonemap writes out. Sizing them apart would render
     * the scene at one aspect and show it at another.
     *
     * This belongs at the top of a frame and never in the middle of one. A
     * rebuild waits for the device, because a frame in flight may still be
     * reading the old image and the overlay may still hold its binding.
     *
     * @param editor Everything the program owns.
     * @return False when a rebuild failed, which leaves nothing to draw into.
     */
    [[nodiscard]] bool resize_scene_images(Editor& editor) {
        const engine::editor::ViewportChange change =
            editor.viewport.ensure(editor.wanted_viewport, device_extent(editor.device));
        if (change == engine::editor::ViewportChange::Failed) {
            return false;
        }
        if (change != engine::editor::ViewportChange::Rebuilt) {
            return true;
        }
        if (!editor.scene.resize(editor.viewport.extent())) {
            return false;
        }
        // A new image carries no history, so there is nothing for the next
        // frame's first barrier to order against.
        editor.scene.reset_output_state();
        return true;
    }

    /**
     * Rebuilds the swapchain when the window changed size.
     *
     * @param editor Everything the program owns.
     * @param last The size the swapchain was built at, updated when it changes.
     * @param out_rebuilt Set when the swapchain was replaced, which means this
     * frame has no image to draw into and the caller skips it.
     * @return False when the swapchain did not rebuild.
     */
    [[nodiscard]] bool follow_window_size(Editor& editor, engine::gfx::Extent2D& last,
                                          bool& out_rebuilt) {
        const engine::gfx::Extent2D extent = window_extent(editor.window);
        out_rebuilt = false;
        if (extent.width == last.width && extent.height == last.height) {
            return true;
        }
        const engine::gfx::Result result = engine::gfx::device_resize(editor.device, extent);
        if (!engine::gfx::succeeded(result)) {
            ENGINE_LOG_CRITICAL("The swapchain did not rebuild: {}",
                                engine::gfx::result_name(result));
            return false;
        }
        last = extent;
        out_rebuilt = true;
        return true;
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

            bool swapchain_rebuilt = false;
            if (!follow_window_size(editor, last_extent, swapchain_rebuilt)) {
                return false;
            }
            if (swapchain_rebuilt) {
                // This frame has no image to draw into. Drawing now would see
                // OutOfDate and rebuild again. The scene images follow the
                // panel rather than the window, and the next frame clamps the
                // panel to the new size.
                continue;
            }

            // Before the frame opens. The panel asked for this size on the
            // frame before, so one frame of a dragged edge shows the old
            // picture, which is invisible at a normal frame rate.
            if (!resize_scene_images(editor)) {
                return false;
            }

            // The frame after this one is the last, so this is where the
            // capture has to be asked for.
            const bool capture = !options.screenshot.empty() && options.frames > 0 &&
                                 frame + 1 >= options.frames;
            const FrameOutcome outcome = draw_frame(editor, panels, running, capture);
            if (outcome == FrameOutcome::Failed) {
                return false;
            }
            if (outcome == FrameOutcome::Skipped) {
                continue;
            }

            ++frame;
            if (options.frames > 0 && frame >= options.frames) {
                // Before the loop ends, because the copy this reads belongs to
                // the frame that just presented.
                if (!options.screenshot.empty()) {
                    (void)apps::write_screenshot(editor.device, options.screenshot);
                }
                ENGINE_LOG_INFO("Stopping after {} frames, as --frames asked.", frame);
                break;
            }
        }

        ENGINE_LOG_INFO("The editor stopped after {} frames.", frame);
        return true;
    }

    /**
     * Works out where the source scene is, so the World panel can save one.
     *
     * The cooked tree is not it. Writing there looks like it worked and the next
     * cook throws the file away, so the panel disables its button rather than
     * offering that. The path cannot be worked out from the running program, so
     * the build passes it in the same way it does for the runtime.
     *
     * @return The source content directory, or empty when it is not there. A
     * build moved away from its source tree gets the empty answer.
     */
    [[nodiscard]] std::filesystem::path game_source_directory() {
        const std::filesystem::path source{ ENGINE_GAME_CONTENT_SOURCE };
        std::error_code error;
        return std::filesystem::is_directory(source, error) ? source : std::filesystem::path{};
    }

    /**
     * Opens the cooked content and reads the opening scene into the world.
     *
     * A failure here is not fatal. The editor opens either way, and the panels
     * then show an empty world rather than nothing at all. That is the right
     * answer for a program somebody starts before they have cooked anything.
     */
    void load_world(Editor& editor, const Options& options) {
        const std::filesystem::path content = options.content.empty()
                                                  ? sandbox::default_content_directory()
                                                  : std::filesystem::path{ options.content };

        if (!editor.content.open(content)) {
            ENGINE_LOG_ERROR("No cooked content at {}. Build the cooker target. The editor "
                             "opens with an empty world.",
                             content.string());
            return;
        }

        if (!sandbox::load(content, &editor.content, editor.world)) {
            ENGINE_LOG_ERROR("The scene did not load, so the world is empty.");
            return;
        }

        const std::filesystem::path source = game_source_directory();
        if (!source.empty()) {
            editor.source_scene = source / sandbox::kSceneFile;
        }
        ENGINE_LOG_INFO("Opened {} with {} entities.", content.string(), editor.world.size());
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

    // After the components are registered, or the scene loses every component
    // nobody claimed. Nothing draws the world yet, so this needs no device.
    load_world(editor, options);

    // A view file next to the executable wins over the defaults, so the editor
    // opens where the last session left the camera. The runtime reads the same
    // file, which is what makes a camera set here the camera the game starts on.
    if (std::filesystem::exists(engine::editor::kViewSettingsFile) &&
        engine::reflect::load_json(engine::editor::kViewSettingsFile, editor.view)) {
        ENGINE_LOG_INFO("Read {}.", engine::editor::kViewSettingsFile);
    }

    const bool ok = run_frames(editor, options);

    stop(editor);
    engine::log::shutdown();
    return ok ? 0 : 1;
}
