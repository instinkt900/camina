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
#include "assets/reference.h"
#include "assets/manifest.h"
#include "core/log.h"
#include "core/version.h"
#include "editor/camera_lines.h"
#include "editor/edits.h"
#include "editor/fly_camera.h"
#include "editor/history.h"
#include "editor/interaction.h"
#include "gizmo.h"
#include "editor/panels.h"
#include "editor/picking.h"
#include "editor/placement.h"
#include "editor/play_mode.h"
#include "editor/view_settings.h"
#include "editor/viewport.h"
#include "gfx/device.h"
#include "gfx/imgui.h"
#include "physics/components.h"
#include "platform/input.h"
#include "platform/paths.h"
#include "platform/process.h"
#include "platform/window.h"
#include "math/ray.h"
#include "render/debug_line_pass.h"
#include "render/scene_renderer.h"
#include "../screenshot.h"
#include "sandbox/game.h"
#include "core/jobs.h"
#include "reflect/json.h"
#include "scene/camera.h"
#include "scene/component_registry.h"
#include "scene/components.h"
#include "scene/prefab.h"
#include "scene/world.h"
#if defined(ENGINE_WITH_LUA)
#include "script/components.h"
#endif

#include <imgui.h>
// The dock builder is internal API. It is the only way to give a first run a
// layout, and every ImGui docking example uses it. See build_default_layout().
#include <imgui_internal.h>

#include <algorithm>
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

    /// What the saved editor view is called, beside the layout file.
    constexpr const char* kCameraFile = "camera.json";

    /// What ImGui calls its layout file. The path is built at start.
    constexpr const char* kLayoutFile = "imgui.ini";

    /// How long to wait before polling again while the window is minimized.
    constexpr int kMinimizedSleepMs = 16;

    /// The largest step one frame may hand the session, in seconds. A stall or
    /// a debugger break must not run a burst of steps when it ends.
    constexpr float kMaxFrameDelta = 0.25F;

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
        /// Start a play session on the first frame, as if somebody had clicked
        /// Play. A run with --frames can then check the session rather than
        /// only the empty editor.
        bool play = false;
        /// Put every floating panel in an OS window of its own, rather than
        /// waiting for somebody to drag one out. Nothing else can exercise the
        /// multi-viewport path without a mouse.
        bool own_windows = false;
        /// Select the entity with this name at startup, as if somebody had
        /// clicked it in the World panel. A run with --frames can then capture
        /// the gizmo, which otherwise needs a hand on the mouse.
        std::string select;
        /// Which handles to start on, for the same reason.
        engine::editor::GizmoControls gizmo;
    };

    void print_usage() {
        ENGINE_LOG_INFO("Usage: editor [options]");
        ENGINE_LOG_INFO("  --content <dir>     Open this cooked content instead of the game's.");
        ENGINE_LOG_INFO("  --frames <n>        Stop after n frames. 0 runs until you quit.");
        ENGINE_LOG_INFO("  --play              Start a play session on the first frame.");
        ENGINE_LOG_INFO("  --own-windows       Give every floating panel an OS window of its own.");
        ENGINE_LOG_INFO("  --select <name>     Select the entity with this name at startup.");
        ENGINE_LOG_INFO("  --gizmo <mode>      Start on move, turn, or size handles.");
        ENGINE_LOG_INFO("  --gizmo-local       Line the handles up with the entity.");
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

    /// Reads which handles --gizmo asked for.
    /// @param text The value given on the command line.
    /// @param out Receives the operation. Untouched when the name is unknown.
    /// @return True when the name is one of the three.
    [[nodiscard]] bool parse_gizmo(std::string_view text,
                                   engine::editor::GizmoOperation& out) {
        if (text == "move") {
            out = engine::editor::GizmoOperation::Translate;
        } else if (text == "turn") {
            out = engine::editor::GizmoOperation::Rotate;
        } else if (text == "size") {
            out = engine::editor::GizmoOperation::Scale;
        } else {
            ENGINE_LOG_CRITICAL("--gizmo wants move, turn, or size, and {} is none of them.",
                                text);
            return false;
        }
        return true;
    }

    /// Reads the command line. An unknown option stops the program, because a
    /// misspelled one that is ignored looks like an option that did nothing.
    /// Reads an option that carries no value.
    /// @param arg The option, as given.
    /// @param out Where the answer goes.
    /// @return True when this was one of them.
    [[nodiscard]] bool parse_flag(std::string_view arg, Options& out) {
        if (arg == "--own-windows") {
            out.own_windows = true;
        } else if (arg == "--gizmo-local") {
            out.gizmo.space = engine::editor::GizmoSpace::Local;
        } else if (arg == "--play") {
            out.play = true;
        } else if (arg == "--no-validation") {
            out.validation = false;
        } else if (arg == "--sync-validation") {
            out.sync_validation = true;
        } else if (arg == "--no-vsync") {
            out.vsync = false;
        } else {
            return false;
        }
        return true;
    }

    /**
     * Reads an option that carries a value.
     *
     * @param arg The option, as given.
     * @param value What followed it.
     * @param out Where the answer goes.
     * @param ok Cleared when the value would not parse, which stops the program.
     * @return True when this was one of them, whatever the value was.
     */
    [[nodiscard]] bool parse_value(std::string_view arg, const char* value, Options& out,
                                   bool& ok) {
        if (arg == "--frames") {
            ok = parse_count(value, out.frames);
        } else if (arg == "--gizmo") {
            ok = parse_gizmo(value, out.gizmo.operation);
        } else if (arg == "--select") {
            out.select = value;
        } else if (arg == "--content") {
            out.content = value;
        } else if (arg == "--screenshot") {
            out.screenshot = value;
        } else {
            return false;
        }
        return true;
    }

    [[nodiscard]] bool parse_options(int argc, char** argv, Options& out) {
        for (int i = 1; i < argc; ++i) {
            const std::string_view arg{ argv[i] };
            if (arg == "--help") {
                print_usage();
                return false;
            }
            if (parse_flag(arg, out)) {
                continue;
            }

            bool ok = true;
            if (i + 1 < argc && parse_value(arg, argv[i + 1], out, ok)) {
                ++i;
                if (!ok) {
                    return false;
                }
                continue;
            }

            ENGINE_LOG_CRITICAL("Unknown option: {}", arg);
            print_usage();
            return false;
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
        /// The wireframe of the camera the game plays through. On by default,
        /// because the editor draws through its own view and nothing else says
        /// where that camera is.
        bool camera_lines = true;
        /// M9.6. What the cooker made, and where an asset field is filled from.
        bool assets = true;
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
        /// Draws the wireframe of the scene camera over the picture. It costs
        /// nothing on a frame with no lines to draw.
        engine::render::DebugLinePass debug_lines;
        /// The lines of that wireframe. Held between frames so a frame with the
        /// wireframe on allocates nothing after the first one.
        std::vector<engine::physics::DebugLine> camera_line_buffer;

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
        /// Every edit made to the scene, and the way back through them.
        engine::editor::History history;
        /**
         * The handle being dragged, which has to outlive the frame because the
         * two edges of one drag are frames apart.
         *
         * The inspector keeps its own rather than sharing this one. Two panels
         * over one interaction can close each other's edit, and then a drag
         * records nothing while a field records a transform nobody moved.
         */
        engine::editor::Interaction gizmo_drag;
        /// The inspector widget being held. See gizmo_drag for why it is separate.
        engine::editor::Interaction field_edit;
        /// Whether a handle was being dragged on the last frame, so the two
        /// edges of one drag can be told apart.
        bool was_dragging = false;
        /**
         * The camera the game plays through, or entt::null when the scene
         * carries none.
         *
         * The viewport does not draw through this. M9.5a made it a component so
         * a level could ship its own viewpoint, and M9.5b gave the editor the
         * free view below. A script that acts along the line of sight reads
         * this one, because that is the eye the player will have.
         */
        entt::entity camera = entt::null;

        /**
         * Where the person is standing while they work.
         *
         * It belongs to the person rather than to the scene, so it is saved
         * beside `imgui.ini` and never in the project. The viewport always
         * draws through it, a running session included, which is what lets
         * somebody fly around a game while it plays.
         */
        engine::editor::FlyCamera view_camera = engine::editor::fallback_fly_camera();

        /// Which handles the gizmo shows, and which axes they line up with.
        /// The viewport bar edits this and apps/editor/gizmo.cpp reads it.
        engine::editor::GizmoControls gizmo;

        /// Keyboard and mouse on the frame clock, which is what flies the view.
        /// The game reads a second one on the fixed step, inside PlayMode.
        engine::platform::Input input;

        /// Where the view is saved. Held because it is built once at start.
        std::string camera_path;

        /// What the user typed in the asset browser filter. Held here because a
        /// panel keeps no state of its own.
        std::string asset_filter;
        /// M9.4. The play session, and the authored world it goes back to.
        engine::editor::PlayMode play;
        /// Whether the Viewport panel held the focus on the last frame. A
        /// running session reads the keyboard only while it did.
        bool viewport_focused = false;

        /// Whether the pointer was over the Viewport panel on the last frame.
        /// The camera takes the mouse from the interface while it is.
        bool viewport_hovered = false;
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

    /**
     * Works out where the saved view goes.
     *
     * Beside the layout, and for the same reason: where a person stands while
     * they work is theirs, not the project's. A scene carries the camera the
     * game plays through, and that one lives in the scene file.
     *
     * @param file The file name under the preferences directory.
     * @return The path, or the file beside the executable when the platform
     * will not say where preferences go.
     */
    [[nodiscard]] std::string preferences_path(const char* file) {
        std::filesystem::path directory = engine::platform::preferences_directory(kApplicationName);
        if (directory.empty()) {
            directory = engine::platform::executable_directory();
        }
        return (directory / file).string();
    }

    /// @return Where ImGui saves the layout.
    [[nodiscard]] std::string layout_path() { return preferences_path(kLayoutFile); }

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

    /**
     * Points the editor at the camera of its world.
     *
     * Call it after anything replaces the entities: the first load, and the
     * stop of a play session, which reads the world back from a snapshot.
     *
     * @param editor Everything the program owns.
     */
    void bind_camera(Editor& editor) { editor.camera = engine::scene::primary_camera(editor.world); }

    /**
     * Drops a camera entity that no longer exists, and finds another.
     *
     * A session runs the game, and a script can destroy any entity. A destroyed
     * one leaves a handle that names nothing, and reading a component off it
     * asserts in this build and reads freed storage in a release one.
     *
     * @param editor Everything the program owns.
     */
    void keep_camera_live(Editor& editor) {
        if (editor.camera != entt::null && !editor.world.registry().valid(editor.camera)) {
            ENGINE_LOG_WARN("The camera entity was destroyed while the session ran.");
            bind_camera(editor);
        }
    }

    /// How far ahead a dropped prefab lands when the pointer is on nothing.
    constexpr float kDropAhead = 6.0F;

    /// The aspect ratio of an image, for the camera matrix.
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
            // A panel dragged off the window becomes an OS window of its own.
            // The runtime overlay asks for neither this nor docking, so a run
            // looks the same every time. See DESIGN.md section 10, M9.
            .viewports = true,
            .merge_viewports = !options.own_windows,
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

        if (!editor.debug_lines.create(editor.device, editor.engine_content)) {
            ENGINE_LOG_CRITICAL("The debug line pass did not build.");
            return false;
        }

        // After the overlay, because the binding the panel draws through comes
        // out of the pool the overlay owns.
        if (!editor.viewport.create(editor.device, extent, extent)) {
            ENGINE_LOG_CRITICAL("The viewport target did not build.");
            return false;
        }
        editor.wanted_viewport = editor.viewport.extent();

        // The camera keys. The game's own actions are bound on the session
        // input when a session starts, which is sandbox::bind_actions.
        engine::editor::bind_fly_actions(editor.input);

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
        editor.debug_lines.destroy();
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
     * Undoes one entry, and keeps the selection pointing at something real.
     *
     * An undo can destroy the selected entity, which is what undoing a create
     * does. Nothing may hold an entity that has gone, so the selection is read
     * back through its identity afterwards.
     *
     * @param editor Everything the program owns.
     */
    void undo_one(Editor& editor) {
        const engine::Guid was_selected = editor.world.identity(editor.selected);
        (void)editor.history.undo(editor.world);
        editor.world.update();
        editor.selected = was_selected.valid() ? editor.world.find(was_selected) : entt::null;
    }

    /// Redoes one entry. See undo_one for why the selection is read back.
    /// @param editor Everything the program owns.
    void redo_one(Editor& editor) {
        const engine::Guid was_selected = editor.world.identity(editor.selected);
        (void)editor.history.redo(editor.world);
        editor.world.update();
        editor.selected = was_selected.valid() ? editor.world.find(was_selected) : entt::null;
    }

    /**
     * Reads the undo and redo keys, wherever the pointer is in the editor.
     *
     * **Not through `editor.input`.** That one is gated on the Viewport panel
     * holding the pointer or the focus, because it flies the camera. A menu
     * shortcut has to work with the pointer over the Inspector as well, so it
     * reads ImGui, which sees the keys for the whole window.
     *
     * A text box gets the keys first. ImGui gives an InputText its own undo on
     * the same chord, and stealing it would make typing in a Name field worse
     * than it was before undo existed.
     *
     * **`ImGui::Shortcut` is the wrong tool here.** It works out its owner from
     * `CurrentFocusScopeId`, which is zero outside a window, and
     * `SetShortcutRouting` asserts on a zero owner. This runs after the menu
     * bar has closed, so there is no window. `IsKeyPressed` reads the key state
     * and asks nothing about routing, which is what a chord with no owner
     * wants. RelWithDebInfo compiles that assert out, so the wrong version
     * would have looked like it worked here and stopped a Debug build.
     *
     * @param editor Everything the program owns.
     */
    void read_undo_keys(Editor& editor) {
        const ImGuiIO& io = ImGui::GetIO();
        if (io.WantTextInput || !io.KeyCtrl) {
            return;
        }
        if (!ImGui::IsKeyPressed(ImGuiKey_Z, false)) {
            return;
        }

        const engine::editor::UndoMenu menu =
            engine::editor::undo_menu(editor.history, editor.play.running());
        if (io.KeyShift) {
            if (menu.can_redo) {
                redo_one(editor);
            }
            return;
        }
        if (menu.can_undo) {
            undo_one(editor);
        }
    }

    /**
     * Draws the menu bar and reports whether the editor keeps running.
     *
     * It comes before the dockspace, because a main menu bar takes its height
     * out of the viewport work area and the dockspace fills what is left.
     */
    void draw_menu_bar(Editor& editor, Panels& panels, bool& running) {
        if (!ImGui::BeginMainMenuBar()) {
            return;
        }
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("Exit", "Alt+F4")) {
                running = false;
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Edit")) {
            const engine::editor::UndoMenu menu =
                engine::editor::undo_menu(editor.history, editor.play.running());
            if (ImGui::MenuItem(menu.undo_label.c_str(), "Ctrl+Z", false, menu.can_undo)) {
                undo_one(editor);
            }
            if (ImGui::MenuItem(menu.redo_label.c_str(), "Ctrl+Shift+Z", false, menu.can_redo)) {
                redo_one(editor);
            }
            if (editor.play.running()) {
                ImGui::Separator();
                ImGui::TextDisabled("Stop the session to edit the scene.");
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("View")) {
            ImGui::MenuItem("Viewport", nullptr, &panels.viewport);
            ImGui::MenuItem("World", nullptr, &panels.world);
            ImGui::MenuItem("Inspector", nullptr, &panels.inspector);
            ImGui::MenuItem("View settings", nullptr, &panels.view);
            ImGui::MenuItem("Assets", nullptr, &panels.assets);
            ImGui::Separator();
            ImGui::MenuItem("Scene camera wireframe", nullptr, &panels.camera_lines);
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
        constexpr float kAssetShare = 0.28F; ///< How much of the middle the assets take.

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

        // The asset browser takes a strip under the picture. Read both outputs:
        // the node that was split is a parent now, and docking into a parent
        // leaves the window floating.
        ImGuiID bottom = 0;
        ImGui::DockBuilderSplitNode(centre, ImGuiDir_Down, kAssetShare, &bottom, &centre);

        ImGui::DockBuilderDockWindow("World", left_top);
        ImGui::DockBuilderDockWindow("View", left_bottom);
        ImGui::DockBuilderDockWindow("Inspector", right);
        ImGui::DockBuilderDockWindow("Viewport", centre);
        // Under the picture, where every editor puts it, and where a drag onto
        // the inspector is a short trip across the window.
        ImGui::DockBuilderDockWindow("Assets", bottom);
        ImGui::DockBuilderFinish(dockspace);
        ENGINE_LOG_INFO("No saved layout, so the editor built the default one.");
    }

    /**
     * Acts on what the play bar asked for.
     *
     * **A stop replaces every entity and a play does not.** Play snapshots the
     * world and runs the entities that are already there, so a handle survives
     * it. Stop reads the snapshot into an empty world, and EnTT hands the same
     * numbers out again, so a handle kept across that line names whoever took
     * its number. That is why the camera is bound again on a stop alone.
     *
     * **The selection is kept across both ends, by identity.** Pressing play to
     * see whether a change works, then stopping and carrying on with the thing
     * you were looking at, is the loop M12 is for. A play clears nothing, so
     * the entity is still the same one. A stop rebuilds every entity, so the
     * identity is read before it and looked up after it. Holding the number
     * across that line would point the inspector at whoever took it.
     *
     * A session that destroys the selected entity leaves nothing to go back
     * to, and the lookup then answers with nothing selected.
     *
     * @param editor Everything the program owns.
     * @param request What the user clicked.
     */
    void apply_play_request(Editor& editor, engine::editor::PlayRequest request) {
        using engine::editor::PlayRequest;
        switch (request) {
        case PlayRequest::Play: {
            const engine::editor::PlayDesc desc{ .content = &editor.content,
                                                 .bind_actions = &sandbox::bind_actions };
            // The selection stays. A play snapshots the world and runs it in
            // place, so the entity somebody was looking at is still that entity.
            (void)editor.play.play(editor.world, desc);
            break;
        }
        case PlayRequest::Pause:
            editor.play.pause();
            break;
        case PlayRequest::Resume:
            editor.play.resume();
            break;
        case PlayRequest::Stop: {
            // Read before the stop and looked up after it. The snapshot carries
            // the identity of every entity, so the one somebody was looking at
            // answers to the same name once it is built again.
            const engine::Guid was_selected = editor.world.identity(editor.selected);
            editor.play.stop(editor.world);
            editor.selected =
                was_selected.valid() ? editor.world.find(was_selected) : entt::null;
            // The snapshot built new entities, so the camera of the world
            // before the session is not the camera of the world after it.
            bind_camera(editor);
            break;
        }
        case PlayRequest::None:
            break;
        }
    }

    /**
     * Draws the gizmo over the picture, from inside the viewport window.
     *
     * The panel calls this between its `Begin` and its `End`, which is the only
     * place the window draw list and the picture rectangle are both known. See
     * `editor::ViewportOverlay`.
     *
     * Nothing is drawn without a selection, which is the usual case.
     *
     * @param user The Editor, because a function pointer carries no state.
     * @param x Left edge of the picture, in screen coordinates.
     * @param y Top edge of the picture.
     * @param width Width of the picture, in pixels.
     * @param height Height of the picture.
     */
    /**
     * Selects whatever the pointer is over, or nothing.
     *
     * The bounds come from the mesh cache, which already holds every mesh the
     * scene drew. A mesh that has not loaded answers false and is not pickable,
     * which is the right answer: it is not on screen either.
     *
     * @param editor Everything the program owns.
     * @param x Left edge of the picture, in screen coordinates.
     * @param y Top edge of the picture.
     * @param width Width of the picture, in pixels.
     * @param height Height of the picture.
     */
    void pick_at_pointer(Editor& editor, float x, float y, float width, float height) {
        if (width <= 0.0F || height <= 0.0F) {
            return;
        }

        // Normalized device coordinates, where -1 is the top. Vulkan's Y runs
        // down and the projection already accounts for it, so a pixel measured
        // from the top of the picture needs no flip. See math/ray.h.
        const ImVec2 pointer = ImGui::GetIO().MousePos;
        const engine::Vec2 ndc =
            engine::ndc_from_pixel(pointer.x, pointer.y, x, y, width, height);

        const engine::Mat4 clip_from_world = engine::editor::fly_clip_from_world(
            editor.view_camera, width / height, engine::editor::kFallbackFov,
            engine::kDefaultNearPlane);
        const engine::Ray ray = engine::ray_through_ndc(
            glm::inverse(clip_from_world), editor.view_camera.position, ndc.x, ndc.y);

        const engine::editor::BoundsLookup bounds = [&editor](engine::Guid mesh,
                                                              engine::Vec3& min,
                                                              engine::Vec3& max) {
            const engine::render::GpuMesh* found =
                editor.scene.mesh().meshes().get(editor.device, editor.content, mesh);
            if (found == nullptr) {
                return false;
            }
            min = found->min;
            max = found->max;
            return true;
        };

        // Empty space clears the selection, which is what every editor does and
        // what makes the gizmo go away when somebody is done with it.
        editor.selected = engine::editor::pick_entity(editor.world, ray, bounds);
    }

    void draw_gizmo_overlay(void* user, float x, float y, float width, float height) {
        Editor& editor = *static_cast<Editor*>(user);
        if (editor.selected == entt::null || !editor.world.registry().valid(editor.selected)) {
            // The entity went away while a handle was held. Close the drag
            // here, or was_dragging stays true and the next drag on another
            // entity never opens one: it would record nothing, and its release
            // would close this stale interaction instead.
            //
            // Ended rather than cancelled. The entity did move before it went,
            // so an entry is the honest answer, and end() records nothing at
            // all when the entity is no longer there to read.
            if (editor.was_dragging) {
                (void)editor.gizmo_drag.end(editor.world, editor.history);
                editor.was_dragging = false;
            }
            return;
        }

        const float aspect = height <= 0.0F ? 1.0F : width / height;
        const apps::GizmoDesc desc{
            .view = engine::editor::fly_view(editor.view_camera),
            .projection = apps::gizmo_projection(engine::editor::kFallbackFov, aspect,
                                                 engine::kDefaultNearPlane),
            .controls = editor.gizmo,
            .x = x,
            .y = y,
            .width = width,
            .height = height,
        };

        // The world matrix, because a gizmo works in world space. World::update
        // has already composed it this frame.
        engine::Mat4 world_matrix = editor.world.world_matrix(editor.selected);
        if (apps::draw_gizmo(desc, world_matrix)) {
            engine::editor::place_entity(editor.world, editor.selected, world_matrix);
        }

        // One drag is one entry. The transform is written on every frame the
        // handle moves, and none of those frames is an edit on its own, so the
        // value is kept when the mouse goes down and the entry is pushed when
        // it comes up. A drag that ends where it started pushes nothing.
        const bool dragging = apps::gizmo_is_dragging();
        if (dragging && !editor.was_dragging) {
            (void)editor.gizmo_drag.begin(editor.world, editor.selected, "Transform");
        } else if (!dragging && editor.was_dragging) {
            (void)editor.gizmo_drag.end(editor.world, editor.history);
        }
        editor.was_dragging = dragging;
    }

    /**
     * Draws the gizmo, then picks, over the picture and inside its window.
     *
     * The order is what keeps the two apart. The gizmo reports whether the
     * pointer is on a handle, and a click on a handle belongs to the drag rather
     * than to a selection. Picking first would select whatever sits behind the
     * arrow somebody just grabbed.
     *
     * @param user The Editor, because a function pointer carries no state.
     * @param x Left edge of the picture, in screen coordinates.
     * @param y Top edge of the picture.
     * @param width Width of the picture, in pixels.
     * @param height Height of the picture.
     */
    /**
     * Creates an instance of a dropped prefab where the pointer is.
     *
     * The payload is an identity, and the prefab library is keyed by the name
     * `assets::prefab_name` builds from the cooked path. So the manifest is what
     * joins the two, and an identity that names no prefab is ignored: dropping a
     * texture on the viewport should do nothing rather than something strange.
     *
     * @param editor Everything the program owns.
     * @param identity The dropped identity, as text.
     * @param x Left edge of the picture, in screen coordinates.
     * @param y Top edge of the picture.
     * @param width Width of the picture, in pixels.
     * @param height Height of the picture.
     */
    void drop_asset(Editor& editor, std::string_view identity, float x, float y, float width,
                    float height) {
        engine::Guid dropped;
        if (!engine::from_text(identity, dropped)) {
            return;
        }

        // Which cooked file that identity is, and what the library calls it.
        std::string name;
        for (const engine::assets::ManifestEntry& entry : editor.content.manifest().entries) {
            for (const engine::assets::ManifestOutput& output : entry.outputs) {
                if (output.guid == dropped &&
                    std::string_view{ output.cooked }.ends_with(
                        engine::assets::kPrefabExtension)) {
                    name = engine::assets::prefab_name(entry.source, output.cooked);
                }
            }
        }
        if (name.empty()) {
            ENGINE_LOG_INFO("That asset is not a prefab, so there is nothing to place.");
            return;
        }

        const engine::scene::Prefab* prefab = engine::scene::prefabs().find(name);
        if (prefab == nullptr) {
            ENGINE_LOG_ERROR("{} is cooked and the library does not hold it.", name);
            return;
        }

        // Where the pointer is pointing. The ground is where a person means, and
        // a drag over the sky puts it a few metres ahead instead, because a drop
        // that goes nowhere is worse than one in a reasonable place.
        const engine::Vec2 ndc =
            engine::ndc_from_pixel(ImGui::GetIO().MousePos.x, ImGui::GetIO().MousePos.y, x, y,
                                   width, height);
        const engine::Mat4 clip_from_world = engine::editor::fly_clip_from_world(
            editor.view_camera, width / height, engine::editor::kFallbackFov,
            engine::kDefaultNearPlane);
        const engine::Ray ray = engine::ray_through_ndc(glm::inverse(clip_from_world),
                                                        editor.view_camera.position, ndc.x, ndc.y);

        float distance = kDropAhead;
        (void)engine::ray_hits_plane(ray, engine::Vec3{ 0.0F, 0.0F, 0.0F },
                                     engine::Vec3{ 0.0F, 1.0F, 0.0F }, distance);
        const engine::Vec3 at = ray.origin + (ray.direction * distance);

        const entt::entity root = engine::editor::drop_prefab(editor.world, *prefab, at);
        if (root == entt::null) {
            ENGINE_LOG_ERROR("{} would not instance.", name);
            return;
        }

        // After the instance is built, because the entry keeps what the new
        // entity holds rather than what it was asked for. A redo then builds
        // the same thing rather than instancing the prefab again.
        editor.history.record(engine::editor::entity_created(editor.world, root));

        // Selected, so the gizmo is on the new thing and a person can move it
        // straight away. That is what every editor does after a drop.
        editor.selected = root;
        ENGINE_LOG_INFO("Placed {} at {} {} {}.", name, at.x, at.y, at.z);
    }

    void draw_viewport_overlay(void* user, float x, float y, float width, float height) {
        Editor& editor = *static_cast<Editor*>(user);
        draw_gizmo_overlay(user, x, y, width, height);

        // The picture is a drop target. The item drawn last is the image, so
        // this attaches to it.
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* dropped =
                    ImGui::AcceptDragDropPayload(engine::reflect::kAssetPayload)) {
                drop_asset(editor,
                           std::string_view{ static_cast<const char*>(dropped->Data),
                                             static_cast<std::size_t>(dropped->DataSize) },
                           x, y, width, height);
            }
            ImGui::EndDragDropTarget();
        }

        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && ImGui::IsWindowHovered() &&
            !apps::gizmo_has_mouse()) {
            pick_at_pointer(editor, x, y, width, height);
        }
    }

    /**
     * Selects the first entity with this name, for --select.
     *
     * @param editor Everything the program owns.
     * @param name The Name component to look for.
     */
    void select_by_name(Editor& editor, const std::string& name) {
        for (const auto [entity, named] :
             editor.world.registry().view<const engine::scene::Name>().each()) {
            if (named.value == name) {
                editor.selected = entity;
                ENGINE_LOG_INFO("Selected {}.", name);
                return;
            }
        }
        ENGINE_LOG_WARN("No entity is named {}, so nothing is selected.", name);
    }

    /// The cooker that ships beside this executable.
    [[nodiscard]] std::filesystem::path cooker_path() {
#if defined(_WIN32)
        constexpr const char* kCookerName = "cooker.exe";
#else
        constexpr const char* kCookerName = "cooker";
#endif
        return engine::platform::executable_directory() / kCookerName;
    }

    /**
     * Cooks the source tree, so what was saved is what the next start reads.
     *
     * **The editor writes the source scene and reads the cooked one.** Between
     * those two sits the cooker, and without this the file on disk is right
     * while the program keeps reading the one from before the save. Every edit
     * then looks lost, and nothing reports an error. That was issue #341.
     *
     * The cooker walks the tree and skips what the manifest already holds, so a
     * save costs one scene file rather than a whole tree.
     *
     * @param editor Everything the program owns.
     * @param source The source content tree the scene was written into.
     */
    void cook_after_save(Editor& editor, const std::filesystem::path& source) {
        const std::vector<std::string> arguments{ "--content", source.string(), "--out",
                                                  editor.content.root().string() };
        const engine::platform::ProcessResult result =
            engine::platform::run_process(cooker_path(), arguments);

        if (!result.ran) {
            ENGINE_LOG_ERROR("The scene was saved and the cooker would not run, so the cooked "
                             "content still holds the scene from before it.");
            return;
        }
        if (result.exit_code != 0) {
            ENGINE_LOG_ERROR("The scene was saved and the cook returned {}, so the cooked "
                             "content and the source no longer agree.",
                             result.exit_code);
            return;
        }
        ENGINE_LOG_INFO("The scene was saved and cooked.");
    }

    /// Draws every panel of one frame, inside the open ImGui frame.
    void draw_ui(Editor& editor, Panels& panels, bool& running) {
        draw_menu_bar(editor, panels, running);

        // After the menu bar, so a click on the item and the chord cannot both
        // fire on one frame.
        read_undo_keys(editor);

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
        engine::editor::ViewportReport viewport{};
        if (panels.viewport) {
            const engine::editor::ViewportOverlay overlay{ .draw = &draw_viewport_overlay,
                                                           .user = &editor };
            viewport = engine::editor::draw_viewport_panel(
                editor.viewport.picture(), editor.viewport.extent(), editor.wanted_viewport,
                editor.play.state(), editor.gizmo, overlay, &panels.viewport);
        }
        editor.viewport_focused = viewport.focused;
        editor.viewport_hovered = viewport.hovered;

        if (panels.world) {
            // The save button writes the world as it stands, and while a
            // session runs that world is a game part way through a step rather
            // than the scene a person authored. Saving it would write the
            // wreckage of a play over the source file.
            const char* blocked =
                editor.play.running() ? "a session is running, so this is not your scene" : nullptr;
            if (engine::editor::draw_world_panel(editor.world, editor.selected,
                                                 editor.source_scene, editor.content,
                                                 &panels.world, blocked, &editor.history)) {
                // The panel wrote the source scene. The cooked tree is what this
                // program reads, so it has to be brought up to date now.
                cook_after_save(editor, editor.source_scene.parent_path());
            }
        }
        if (panels.inspector) {
            engine::editor::draw_inspector_panel(editor.world, editor.selected,
                                                 &panels.inspector, &editor.history,
                                                 &editor.field_edit);
        }
        if (panels.assets) {
            engine::editor::draw_assets_panel(editor.content, editor.asset_filter,
                                              &panels.assets);
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

        // After the panels are drawn and before the scene is composed. A stop
        // replaces every entity, so acting on it here means this frame draws
        // the world that comes back rather than the one that just went.
        apply_play_request(editor, viewport.request);

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

        // After the ImGui frame opens and before any panel draws, because the
        // gizmo reads the mouse state this call latches.
        apps::begin_gizmo_frame();

        // Before the panels, because two of them read world matrices: the gizmo
        // draws at the pose of the selected entity, and a click tests the bounds
        // of every entity. A session moved things this frame through
        // Simulation::interpolate, which writes local transforms, so without
        // this both would work from the pose of the frame before. The update
        // after the panels stays, for what an inspector edit or a drag changed.
        editor.world.update();

        draw_ui(editor, panels, running);

        // The scene, into the image the panel shows. The same four passes the
        // runtime draws, and the same barriers, because both go through
        // render::SceneRenderer.
        //
        // The camera aspect comes from the target rather than from the window,
        // so what a person sees in the panel is the whole picture and not a
        // stretched one.
        const engine::gfx::Extent2D scene_extent = editor.viewport.extent();
        const float aspect = aspect_ratio(scene_extent);

        // Always the editor's own camera, never the scene's. A person has to be
        // able to look at what they are editing, and at what a session is
        // doing, from somewhere other than where the game is looking.
        const engine::Mat4 clip_from_world =
            engine::editor::fly_clip_from_world(editor.view_camera, aspect,
                                                engine::editor::kFallbackFov,
                                                engine::kDefaultNearPlane);

        // The exposure is the scene's, because it is a property of the level
        // that somebody is judging by eye. A scene with no camera tonemaps at
        // one, which is neutral.
        float exposure = 1.0F;
        if (editor.camera != entt::null) {
            exposure =
                editor.world.registry().get<const engine::scene::Camera>(editor.camera).exposure;
        }

        const engine::render::SceneView view{
            .clip_from_world = clip_from_world,
            .camera_position = editor.view_camera.position,
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
            editor.scene.draw_tonemap(info.commands, exposure);

            // Inside the tonemap scope, because the line pipeline is built for
            // a target with no depth attachment. The colors are display colors
            // for the same reason the physics wireframe draws here: after the
            // curve, so the color chosen is the color on screen.
            if (panels.camera_lines && editor.camera != entt::null) {
                engine::editor::camera_lines(editor.world, editor.camera, aspect,
                                             editor.camera_line_buffer);
                editor.debug_lines.draw(info.commands, clip_from_world,
                                        editor.camera_line_buffer);
            }

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

        // After the frame that holds the main window has been presented. Each
        // panel that lives in its own OS window has a swapchain of its own, and
        // this is what draws and presents those.
        engine::gfx::imgui_render_platform_windows();

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

    /**
     * Samples the devices for the session, and folds the frame into its step.
     *
     * A session reads the keyboard only while the Viewport panel holds the
     * focus, so a value typed into the inspector cannot drive the game. Every
     * other frame feeds a default frame, which reads every action as false.
     *
     * The focus is what the panel reported on the frame before, because the
     * panels are drawn after this. One frame of lag on a click is invisible,
     * and the alternative is drawing the UI twice.
     *
     * ImGui is asked first, because it owns the keyboard while a person types.
     * platform/ sits below gfx/, so the module cannot ask on its own.
     *
     * @param editor Everything the program owns.
     */
    void update_input(Editor& editor) {
        engine::platform::InputConsumed consumed;
        engine::gfx::imgui_wants_input(&consumed.mouse, &consumed.keyboard);

        // The viewport is an ImGui window, so ImGui claims the mouse whenever
        // the pointer is over the picture. A camera drawn in that picture has
        // to ignore that claim or it can never be turned.
        const bool looking = editor.input.held(engine::editor::fly_action::kLook);
        consumed.mouse = engine::editor::mouse_consumed_by_ui(consumed.mouse,
                                                              editor.viewport_hovered, looking);

        engine::platform::InputFrame state;
        if (editor.viewport_hovered || editor.viewport_focused || looking) {
            state = engine::platform::sample(editor.window, consumed);
        }

        // The camera, on the frame clock. A frame the viewport did not hold the
        // focus for reads a default frame, so every action is false and the
        // camera stands still.
        editor.input.update(state);

        // And the game, on the fixed step. The same gate, because a key meant
        // for a panel is not a key meant for the game either. feed_input does
        // nothing while no session runs.
        editor.play.feed_input(state);
    }

    /**
     * Flies the editor view for one frame.
     *
     * The speed and the sensitivity come from the View panel, so a person tunes
     * them where they tune everything else. The pose belongs to the camera.
     *
     * @param editor Everything the program owns.
     * @param delta_seconds How much wall time this frame took.
     */
    void fly_view(Editor& editor, float delta_seconds) {
        editor.view_camera.move_speed = editor.view.move_speed;
        editor.view_camera.look_sensitivity = editor.view.look_sensitivity;
        // Hold the right mouse button to look, and the keys move while it is
        // held. That is what every editor does, and it is what leaves the
        // letter keys free for anything else.
        editor.view_camera.move_needs_look = true;
        (void)engine::editor::update_fly_camera(editor.view_camera, editor.window, editor.input,
                                                delta_seconds);
    }

    /**
     * Works out how much wall time this frame covers.
     *
     * A long stall, a debugger break, or a driver hitch would otherwise hand
     * the step accumulator the whole gap. The clock has a ceiling of its own,
     * and this is the same clamp the runtime applies for the same reason.
     *
     * @param last_frame When the last frame ran.
     * @param now The time this frame began.
     * @return Seconds since the last frame, clamped.
     */
    [[nodiscard]] float frame_delta(std::chrono::steady_clock::time_point last_frame,
                                    std::chrono::steady_clock::time_point now) {
        const float seconds = std::chrono::duration<float>(now - last_frame).count();
        return std::min(seconds, kMaxFrameDelta);
    }

    /// Runs frames until the user quits, the frame limit lands, or a frame fails.
    [[nodiscard]] bool run_frames(Editor& editor, const Options& options) {
        Panels panels;
        bool running = true;
        std::uint64_t frame = 0;
        engine::gfx::Extent2D last_extent = window_extent(editor.window);
        auto last_frame = std::chrono::steady_clock::now();

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

            const auto now = std::chrono::steady_clock::now();
            const float delta = frame_delta(last_frame, now);
            last_frame = now;

            update_input(editor);
            fly_view(editor, delta);

            // The rate and the ceiling are read each frame, because the View
            // panel can move either one while a session runs.
            //
            // A session that is paused advances nothing here, and one that is
            // not running does not exist. So an editor sitting in Edit state
            // costs a comparison.
            if (engine::play::Session* session = editor.play.session(); session != nullptr) {
                session->set_rate_hz(editor.view.physics_hz);
                session->set_max_steps(editor.view.max_physics_steps);
            }
            // The game plays through the scene camera, not through whatever the
            // editor is looking at. That is the point of the split M9.5a made:
            // a throw aimed along the editor view is a game the player cannot
            // reproduce.
            engine::play::View view{ .position = editor.view_camera.position,
                                     .forward =
                                         engine::editor::fly_forward(editor.view_camera) };
            if (editor.camera != entt::null) {
                engine::scene::camera_pose(editor.world, editor.camera, view.position,
                                           view.forward);
            }
            editor.play.advance(editor.world, view, delta);

            // After the step, because that is where a script can destroy an
            // entity, and before the frame below reads the camera.
            keep_camera_live(editor);

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
        bind_camera(editor);

        // What an AssetRef field shows instead of an identity nobody can read.
        // reflect/ sits below assets/, so the manifest arrives this way rather
        // than by an include. See reflect::set_asset_namer.
        engine::reflect::set_asset_namer(
            [&editor](std::string_view value) -> std::string {
                engine::Guid identity;
                if (!engine::from_text(value, identity)) {
                    return {};
                }
                return engine::assets::reference_for(editor.content.manifest(), identity);
            });

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

    // The solver runs on the job system, and a play session builds one. This is
    // before anything that could step, which is what the runtime does too.
    engine::jobs::init();

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
        engine::jobs::shutdown();
        engine::log::shutdown();
        return 1;
    }

    // After the components are registered, or the scene loses every component
    // nobody claimed. Nothing draws the world yet, so this needs no device.
    load_world(editor, options);

    // Where the person was standing when they last closed the editor. This is
    // the editor's own view and never the camera the game plays through, which
    // the scene carries.
    editor.camera_path = preferences_path(kCameraFile);
    if (std::filesystem::exists(editor.camera_path) &&
        engine::reflect::load_json(editor.camera_path, editor.view_camera)) {
        ENGINE_LOG_INFO("Read the saved view from {}.", editor.camera_path);
    }

    // A view file next to the executable wins over the defaults. It carries how
    // fast a person flies and how the simulation steps, and no camera: M9.5a
    // moved that into the scene.
    if (std::filesystem::exists(engine::editor::kViewSettingsFile) &&
        engine::reflect::load_json(engine::editor::kViewSettingsFile, editor.view)) {
        ENGINE_LOG_INFO("Read {}.", engine::editor::kViewSettingsFile);
    }

    // As if somebody had clicked the entity in the World panel. The gizmo
    // draws on the selection, and a run with no hands on the mouse selects
    // nothing without this.
    if (!options.select.empty()) {
        select_by_name(editor, options.select);
    }
    editor.gizmo = options.gizmo;

    // As if somebody had clicked Play on the first frame. A run with --frames
    // then exercises the session, which is what an editor with no offscreen
    // mode otherwise has no way to check.
    if (options.play) {
        apply_play_request(editor, engine::editor::PlayRequest::Play);
    }

    const bool ok = run_frames(editor, options);

    // Before the world goes, so a script that runs on_destroy still finds the
    // simulation it may reach. A session left running at exit is the normal
    // way to close the editor.
    editor.play.stop(editor.world);

    // Where the person was standing, so the next start opens there. ImGui saves
    // its layout beside this and for the same reason.
    if (!engine::reflect::save_json(editor.camera_path, editor.view_camera)) {
        ENGINE_LOG_ERROR("The editor view did not save to {}.", editor.camera_path);
    }

    stop(editor);
    engine::jobs::shutdown();
    engine::log::shutdown();
    return ok ? 0 : 1;
}
