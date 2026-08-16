#include "assets/hot_reload.h"
#include "assets/manifest.h"
#include "assets/script.h"
#include "core/arena.h"
#include "core/frame_stats.h"
#include "core/jobs.h"
#include "core/log.h"
#include "core/profile.h"
#include "core/timestep.h"
#include "core/version.h"
#include "editor/panels.h"
#include "editor/view_settings.h"
#include "gfx/device.h"
#include "gfx/imgui.h"
#include "math/conventions.h"
#include "platform/input.h"
#include "platform/paths.h"
#include "platform/window.h"
#include "reflect/json.h"
#include "reflect/registry.h"
#include "render/scene_renderer.h"
#include "screenshot.h"
#include "sandbox/game.h"
#include "physics/components.h"
#include "physics/simulation.h"
#include "render/debug_line_pass.h"
#include "scene/component_registry.h"
#if defined(ENGINE_WITH_LUA)
#include "script/components.h"
#include "script/host.h"
#endif
#include "scene/prefab.h"
#include "scene/step_motion.h"
#include "scene/world.h"

#if defined(ENGINE_WITH_UI)
#include "ui/image.h"
#include "ui/renderer.h"
#include "ui/ui.h"
#include "ui/font_factory.h"
#include "ui/ui_pass.h"

#include <moth_ui/context.h>
#include <moth_ui/layout/layout.h>
#include <moth_ui/nodes/node.h>
#endif

#include <imgui.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

    constexpr std::size_t kFrameArenaBytes = 4U * 1024U * 1024U;

    /// About one frame at 60 Hz. Long enough to idle, short enough to wake fast.
    constexpr int kMinimizedSleepMs = 16;


    /// Straight up and straight down have no usable basis, so stop short of both.
    constexpr float kLowestPitch = -89.0F;
    constexpr float kHighestPitch = 89.0F;
    constexpr float kFullTurnDegrees = 360.0F;
    /// Below this the movement keys cancel out and there is nothing to normalize.
    constexpr float kShortestMove = 1.0e-4F;
    /// How much faster shift makes the camera.
    constexpr float kSprintFactor = 4.0F;
    /// The largest step one frame may apply. A stall must not teleport the camera.
    constexpr float kLongestFrame = 0.1F;

    /// The step an offscreen run advances the world by, in seconds.
    ///
    /// An offscreen run exists to be compared against another one, and the
    /// sandbox turns things by elapsed time. Driving that from the wall clock
    /// makes the captured frame depend on how fast the run went, so two runs of
    /// the same length land on different rotations. Counting frames instead
    /// makes the same command produce the same image.
    constexpr float kOffscreenStep = 1.0F / 60.0F;

    /// How many frames the frame time report ignores at the start of a run.
    /// The first frames build pipelines and fill the caches, so they measure
    /// the startup rather than the renderer.
    constexpr std::size_t kFrameStatsWarmup = 60;

    /// Where each window opens. This overlay does not dock and writes no
    /// imgui.ini, so a run always starts from this layout and a move lasts
    /// until the program ends. Without these all three open at the same place,
    /// and the last one drawn buries the rest. The editor docks instead, and it
    /// places nothing.
    constexpr float kPanelMargin = 16.0F;
    constexpr float kPanelWidth = 340.0F;
    constexpr float kViewHeight = 320.0F;
    constexpr float kWorldHeight = 380.0F;
    constexpr float kInspectorHeight = 460.0F;

    /// The panels and the view state are shared with the editor now. See
    /// src/editor/ and DESIGN.md section 6.
    using engine::editor::ViewSettings;

    /// Where the view settings go. The working directory, so a run is easy to redo.
    /// The scene itself lives in the sandbox content directory, not here.
    constexpr const char* kViewPath = engine::editor::kViewSettingsFile;

    struct Options {
        std::uint64_t max_frames = 0; ///< 0 means run until the user quits.
        /// Where to write a PNG of the last frame. Empty writes nothing.
        std::string screenshot;
        bool validation = true;
        /// Whether to also check the barriers. Slow, so a person asks for it.
        bool sync_validation = false;
        /// Where the game reads its content. Empty means the compiled-in default.
        std::string content;
        /// The source content tree to watch. Empty means the compiled-in default.
        std::string watch;
        bool hot_reload = true; ///< False turns the watcher and the cooker off.
        /// Whether to wait for the refresh. On by default, because a person
        /// flying around the sandbox wants it. Turn it off to measure a change.
        bool vsync = true;
        /// Whether to draw without opening a window. See issue #139.
        bool offscreen = false;
        /**
         * The size to render at. Zero takes the default.
         *
         * Offscreen this is exact, because nothing else has a say. Windowed it
         * is only a request: a window manager is free to give another size, and
         * a tiling one always does. So a run that has to be the same size every
         * time needs --offscreen as well.
         */
        engine::gfx::Extent2D resolution{ 0, 0 };
        /**
         * The exposure to apply, or zero to keep whatever view.json holds.
         *
         * Zero rather than one as the "not given" value, because one is a
         * setting somebody may want and this has to tell the two apart. An
         * exposure of zero would black the frame, so it is not a value anybody
         * loses by spending it here.
         */
        float exposure = 0.0F;
        /**
         * The step rate to use, or zero to keep whatever view.json holds.
         *
         * Zero rather than 60 as the "not given" value, for the reason the
         * exposure above gives. A rate somebody asks for has to be tellable
         * from one nobody did.
         */
        float physics_hz = 0.0F;
        /// Whether to turn the physics wireframe on. Off unless asked for.
        bool physics_debug = false;
        /**
         * Throw a crate on this frame, or 0 to throw none.
         *
         * An offscreen capture has no keyboard, so the milestone test cannot be
         * captured without this. The camera is where the settings put it and
         * the frame number is fixed, so the throw is the same every run and the
         * picture is reproducible.
         */
        std::uint64_t throw_at_frame = 0;
        /**
         * The most lights one cluster cell may hold. Zero takes the default.
         *
         * There to force a cell to overflow. The grid grows to hold every
         * visible light, so no sandbox scene reaches the drop path on its own.
         * Lowering the ceiling makes a small scene reach it, which is how the
         * light it loses is measured. See issue #175.
         */
        std::uint32_t cluster_cell_lights = 0;
    };

    /**
     * Reads a `<width>x<height>` pair.
     *
     * `std::from_chars` rather than `strtoul`, because strtoul accepts a leading
     * sign and negates it. `-1` then arrives as the largest unsigned value, and
     * a check for zero does not catch it. This form parses straight into the
     * unsigned type, so a sign and an out of range value are both refused.
     *
     * A value it refuses is reported here rather than by the caller, so the rule
     * and the message about the rule sit together.
     *
     * A partly parsed pair is refused rather than half applied, because a run
     * that silently used one axis would produce an image nobody asked for.
     *
     * @param text The value given on the command line.
     * @param out Receives the size. Untouched unless the whole pair parsed.
     */
    void parse_resolution(std::string_view text, engine::gfx::Extent2D& out) {
        const auto read = [](std::string_view part, std::uint32_t& value) {
            if (part.empty()) {
                return false;
            }
            const char* first = part.data();
            const char* last = part.data() + part.size();
            const std::from_chars_result parsed = std::from_chars(first, last, value);
            // ptr must reach the end, or there was trailing text such as "720p".
            return parsed.ec == std::errc{} && parsed.ptr == last && value != 0;
        };

        const std::size_t cross = text.find('x');
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        const bool parsed = cross != std::string_view::npos &&
                            read(text.substr(0, cross), width) &&
                            read(text.substr(cross + 1), height);
        if (!parsed) {
            ENGINE_LOG_WARN("--resolution wants <width>x<height> above zero, so {} was ignored.",
                            text);
            return;
        }

        out = engine::gfx::Extent2D{ width, height };
    }

    /**
     * Reads an option that has to be a finite number above zero.
     *
     * `std::from_chars` for a float needs a whole value and no trailing text,
     * which is what refuses "1.0x" and "abc".
     *
     * It does parse "inf" and "nan", so those have to be refused by value
     * rather than by a parse failure. An exposure of either reaches the shader
     * as a scale that makes the curve produce something no display can show:
     * infinity divided by infinity is not a number, and a frame of those is
     * undefined rather than black. A step rate of either is worse, because a
     * NaN stops the simulation with no message at all. `std::isfinite` is what
     * rejects the pair, and the comparison against zero alone would let
     * infinity through.
     *
     * @param option The option name, for the message when the value is refused.
     * @param text The value given on the command line.
     * @param out Receives the value. Untouched unless the whole value parsed
     * and came out finite and above zero.
     */
    /**
     * Reads an option that has to be a whole count.
     *
     * `std::strtoull` was here first, and it takes what it can and stops. So
     * `3x` parsed as 3 and `abc` parsed as 0, and neither said anything. A
     * frame number that quietly became zero turned the option off.
     *
     * `std::from_chars` reports where it stopped, and this refuses anything it
     * did not read to the end.
     *
     * @param option The option name, for the message when the value is refused.
     * @param text The value given on the command line.
     * @param out Receives the count. Untouched unless the whole value parsed.
     */
    void parse_count(std::string_view option, std::string_view text, std::uint64_t& out) {
        const char* first = text.data();
        const char* last = text.data() + text.size();
        std::uint64_t value = 0;
        const std::from_chars_result parsed = std::from_chars(first, last, value);
        if (parsed.ec != std::errc{} || parsed.ptr != last) {
            ENGINE_LOG_WARN("{} wants a whole number, so {} was ignored.", option, text);
            return;
        }
        out = value;
    }

    void parse_positive_float(std::string_view option, std::string_view text, float& out) {
        const char* first = text.data();
        const char* last = text.data() + text.size();
        float value = 0.0F;
        const std::from_chars_result parsed = std::from_chars(first, last, value);
        const bool usable = parsed.ec == std::errc{} && parsed.ptr == last &&
                            std::isfinite(value) && value > 0.0F;
        if (!usable) {
            ENGINE_LOG_WARN("{} wants a finite number above zero, so {} was ignored.", option,
                            text);
            return;
        }
        out = value;
    }

    /**
     * Reads the per-cell light ceiling.
     *
     * `std::from_chars` on the unsigned type, for the reason parse_resolution()
     * gives: strtoul accepts a leading sign and negates it, so `-1` would arrive
     * as the largest unsigned value and pass a test for zero.
     *
     * @param text The value given on the command line.
     * @param out Receives the ceiling. Untouched unless the whole value parsed
     * and came out above zero.
     */
    void parse_cell_lights(std::string_view text, std::uint32_t& out) {
        const char* last = text.data() + text.size();
        std::uint32_t value = 0;
        const std::from_chars_result parsed = std::from_chars(text.data(), last, value);
        if (parsed.ec != std::errc{} || parsed.ptr != last || value == 0) {
            ENGINE_LOG_WARN("--cluster-cell-lights wants a whole number above zero, so {} was "
                            "ignored.",
                            text);
            return;
        }
        // The flag is there to lower the ceiling. Raising it past the memory
        // budget is clamped rather than obeyed, and saying so beats a run that
        // reports a capacity nobody asked for.
        if (value > engine::render::kMaxLightsPerCell) {
            ENGINE_LOG_WARN("--cluster-cell-lights {} is above the {} the cluster grid budget "
                            "allows, so it holds at {}.",
                            value, engine::render::kMaxLightsPerCell,
                            engine::render::kMaxLightsPerCell);
        }
        out = value;
    }

    /**
     * Reads an option that carries no value.
     *
     * These are split out because the chain in parse_options that reads them
     * all had grown past the branch count clang-tidy accepts in one function.
     * The ones that take a value have to stay together, because each of them
     * moves the loop index and the loop moves it again.
     *
     * @param arg One argument from the command line.
     * @param options The options to fill.
     * @return True when this took the argument, so the caller looks no further.
     */
    bool parse_flag(std::string_view arg, Options& options) {
        if (arg == "--physics-debug") {
            options.physics_debug = true;
        } else if (arg == "--no-watch") {
            options.hot_reload = false;
        } else if (arg == "--no-validation") {
            options.validation = false;
        } else if (arg == "--sync-validation") {
            options.sync_validation = true;
        } else if (arg == "--no-vsync") {
            options.vsync = false;
        } else if (arg == "--offscreen") {
            options.offscreen = true;
        } else {
            return false;
        }
        return true;
    }

    /**
     * Reads the command line.
     *
     * Every option that takes a value moves the index once, and the loop moves
     * it again. An option that moves it twice swallows whatever follows the
     * value, which is a hard failure to see because the swallowed option simply
     * does nothing.
     */
    Options parse_options(int argc, char** argv) {
        Options options;
        for (int i = 1; i < argc; ++i) {
            const std::string_view arg{ argv[i] };
            const bool has_value = i + 1 < argc;

            // The value-less ones first, so the chain below holds only the
            // options that have an index to move.
            if (parse_flag(arg, options)) {
                continue;
            }

            if (arg == "--frames" && has_value) {
                parse_count("--frames", argv[i + 1], options.max_frames);
                ++i;
            } else if (arg == "--content" && has_value) {
                options.content = argv[i + 1];
                ++i;
            } else if (arg == "--screenshot" && has_value) {
                options.screenshot = argv[i + 1];
                ++i;
            } else if (arg == "--watch" && has_value) {
                options.watch = argv[i + 1];
                ++i;
            } else if (arg == "--throw-at-frame" && has_value) {
                parse_count("--throw-at-frame", argv[i + 1], options.throw_at_frame);
                ++i;
            } else if (arg == "--resolution" && has_value) {
                parse_resolution(argv[i + 1], options.resolution);
                ++i;
            } else if (arg == "--exposure" && has_value) {
                parse_positive_float("--exposure", argv[i + 1], options.exposure);
                ++i;
            } else if (arg == "--physics-hz" && has_value) {
                parse_positive_float("--physics-hz", argv[i + 1], options.physics_hz);
                ++i;
            } else if (arg == "--cluster-cell-lights" && has_value) {
                parse_cell_lights(argv[i + 1], options.cluster_cell_lights);
                ++i;
            }
        }
        return options;
    }

    /**
     * Where a person edits the game content, or empty when it is not there.
     *
     * A build away from its source tree has none, and neither has a shipped
     * one. Hot reload turns itself off in that case, and the save reports
     * rather than writing somewhere the next cook overwrites.
     */
    std::filesystem::path game_source_directory(const Options& options) {
        const std::filesystem::path source =
            options.watch.empty() ? std::filesystem::path{ ENGINE_GAME_CONTENT_SOURCE }
                                  : std::filesystem::path{ options.watch };
        std::error_code error;
        return std::filesystem::is_directory(source, error) ? source : std::filesystem::path{};
    }

    /// The action names the runtime binds. One name for one thing, per §3.
    namespace action {
        constexpr const char* kForward = "move_forward";
        constexpr const char* kBack = "move_back";
        constexpr const char* kLeft = "move_left";
        constexpr const char* kRight = "move_right";
        constexpr const char* kUp = "move_up";
        constexpr const char* kDown = "move_down";
        constexpr const char* kSprint = "sprint";
        constexpr const char* kLook = "look";
        constexpr const char* kThrow = "throw";
        constexpr const char* kReset = "reset";
    } // namespace action

    /// The key the throw is bound to. Named once, because --throw-at-frame has
    /// to hold down the same one the binding reads.
    constexpr engine::platform::Key kThrowKey = engine::platform::Key::F;

    /**
     * Binds the runtime's own actions.
     *
     * The camera actions belong to the application. The throw and the reset
     * belong to the game, and puzzle.lua reads both by name. The binding stays
     * here because a key is what the application owns: a script names the
     * action and never the key, which is what issue #207 asked for.
     */
    void bind_actions(engine::platform::Input& input) {
        using engine::platform::Key;
        using engine::platform::MouseButton;

        input.bind(action::kForward, Key::W);
        input.bind(action::kBack, Key::S);
        input.bind(action::kLeft, Key::A);
        input.bind(action::kRight, Key::D);
        input.bind(action::kUp, Key::E);
        input.bind(action::kDown, Key::Q);
        input.bind(action::kSprint, Key::LeftShift);
        input.bind(action::kSprint, Key::RightShift);
        input.bind(action::kLook, MouseButton::Right);
        input.bind(action::kThrow, kThrowKey);
        input.bind(action::kReset, Key::R);
    }

    /**
     * Moves and turns the camera from the keyboard and the mouse.
     *
     * The right mouse button holds the look. While it is down the pointer is
     * captured, so a drag never runs out of screen.
     *
     * ImGui gets first refusal on both devices, so typing in a field does not
     * fly the camera away. That gate is applied in platform::sample() now, so
     * this function sees a frame with the taken parts already cleared.
     */
    void update_camera(const engine::platform::Window& window,
                       const engine::platform::Input& input, ViewSettings& settings,
                       float delta_seconds) {
        const bool looking = input.held(action::kLook);

        // The pointer stays put while the look is held, so the drag has no edge.
        engine::platform::set_relative_mouse(window, looking);

        if (looking) {
            const engine::Vec2 delta = input.mouse_delta();
            settings.camera_yaw -= delta.x * settings.look_sensitivity;
            settings.camera_pitch -= delta.y * settings.look_sensitivity;
            // Straight up would make the forward vector and world up parallel,
            // and lookAt has no basis to build from a pair like that.
            settings.camera_pitch = std::clamp(settings.camera_pitch, kLowestPitch, kHighestPitch);
            settings.camera_yaw = std::remainder(settings.camera_yaw, kFullTurnDegrees);
        }

        const engine::Vec3 forward = camera_forward(settings);
        const engine::Vec3 right = glm::normalize(glm::cross(forward, engine::world_up));

        engine::Vec3 wanted{ 0.0F, 0.0F, 0.0F };
        if (input.held(action::kForward)) {
            wanted += forward;
        }
        if (input.held(action::kBack)) {
            wanted -= forward;
        }
        if (input.held(action::kRight)) {
            wanted += right;
        }
        if (input.held(action::kLeft)) {
            wanted -= right;
        }
        if (input.held(action::kUp)) {
            wanted += engine::world_up;
        }
        if (input.held(action::kDown)) {
            wanted -= engine::world_up;
        }

        if (glm::length(wanted) < kShortestMove) {
            return;
        }

        const float speed =
            settings.move_speed * (input.held(action::kSprint) ? kSprintFactor : 1.0F);
        settings.camera_position += glm::normalize(wanted) * speed * delta_seconds;
    }

    /// The aspect ratio of an image, for engine::editor::view_projection().
    /// A zero height reads as square rather than dividing by zero.
    [[nodiscard]] float aspect_ratio(engine::gfx::Extent2D extent) {
        return extent.height == 0 ? 1.0F
                                  : static_cast<float>(extent.width) /
                                        static_cast<float>(extent.height);
    }

    engine::gfx::Extent2D window_extent(const engine::platform::Window& window) {
        return engine::gfx::Extent2D{ static_cast<std::uint32_t>(window.size().x),
                                      static_cast<std::uint32_t>(window.size().y) };
    }

    /// What the device renders at, which is the only size an offscreen run has.
    engine::gfx::Extent2D device_extent(engine::gfx::Device* device) {
        engine::gfx::Extent2D extent{};
        (void)engine::gfx::capture_frame(device, nullptr, 0, &extent);
        return extent;
    }

    /**
     * Rebuilds the swapchain and everything sized with it.
     *
     * The scene color target is the size of the window, so it goes with the
     * swapchain rather than surviving it. Its state is carried between frames,
     * and a new image has no history, so that carried state resets here as well.
     *
     * @param device The device to resize.
     * @param extent The size to ask for.
     * @param scene The renderer that owns the scene color target.
     * @return True when the swapchain and the target now match @p extent.
     */
    bool rebuild_swapchain(engine::gfx::Device* device, engine::gfx::Extent2D extent,
                           engine::render::SceneRenderer& scene) {
        const engine::gfx::Result result = engine::gfx::device_resize(device, extent);
        if (!engine::gfx::succeeded(result)) {
            ENGINE_LOG_CRITICAL("The swapchain did not rebuild: {}",
                                engine::gfx::result_name(result));
            return false;
        }

        // What the device settled on, which is not always what was asked for.
        // A surface can refuse a size, and the target has to match the frame
        // rather than the request.
        return scene.resize(device_extent(device));
    }

    /// What one pass through the render loop achieved.
    enum class FrameOutcome {
        Drawn,   ///< The frame reached the presentation engine.
        Skipped, ///< The swapchain was stale and has been rebuilt. Try again.
        Failed,  ///< The device reported an error the loop cannot handle.
    };

#if defined(ENGINE_WITH_UI)
    /**
     * Records a placeholder layout, so M6.2 has something to look at.
     *
     * moth_ui normally drives the recorder through its node tree. Nothing loads
     * a layout yet, so this calls the same interface by hand and covers each
     * recorder path: a solid rect, a gradient, a clip, a transform, and from
     * M6.3 an image drawn both ways the backend serves.
     *
     * Issue #200 replaces this with a real layout, and this goes away with it.
     */
    /// The size the probe rasterizes at. Large enough to read in a 1280 by 720
    /// screenshot, which is the only way this gets checked.
    constexpr int kUiProbeFontSize = 24;

    /**
     * Draws one string in a box, with the box outlined around it.
     *
     * The outline is what makes the alignment readable. Text alone shows where
     * it landed and not what it was aligned against, so a wrong alignment and a
     * wrong destination rectangle look the same.
     */
    void record_text_probe(engine::ui::Renderer& renderer, moth_ui::IFont& font,
                           const moth_ui::IntRect& box, std::string_view text,
                           moth_ui::TextHorizAlignment horizontal,
                           moth_ui::TextVertAlignment vertical) {
        renderer.PushColor(moth_ui::Color{ 0.35F, 0.35F, 0.40F, 1.0F });
        renderer.RenderRect(box);
        renderer.PopColor();

        renderer.PushColor(moth_ui::Color{ 0.95F, 0.95F, 0.90F, 1.0F });
        renderer.RenderText(text, font, horizontal, vertical, box);
        renderer.PopColor();
    }

    void record_ui_probe(engine::ui::Renderer& renderer, engine::gfx::Extent2D extent,
                         const moth_ui::IImage* image, moth_ui::IFont* font,
                         moth_ui::Node* layout) {
        renderer.begin(extent.width, extent.height);

        // M6.5. The layout first, so the probe below draws over it and a
        // regression in either one stays readable against the other.
        //
        // The screen rectangle is set on every frame rather than once, because
        // the device can settle on a size the window never asked for and a
        // resize has to reach the tree. moth_ui lays the children out from it.
        if (layout != nullptr) {
            layout->SetScreenRect(moth_ui::IntRect{
                { 0, 0 },
                { static_cast<int>(extent.width), static_cast<int>(extent.height) } });
            layout->Draw();
        }

        // A solid bar across the top.
        renderer.PushColor(moth_ui::Color{ 0.10F, 0.55F, 0.85F, 1.0F });
        renderer.RenderFilledRect(moth_ui::IntRect{ { 16, 16 }, { 336, 56 } });
        renderer.PopColor();

        // A gradient below it, turned so the angle path is exercised.
        moth_ui::LinearGradient gradient;
        gradient.startColor = moth_ui::Color{ 0.9F, 0.2F, 0.1F, 1.0F };
        gradient.endColor = moth_ui::Color{ 0.1F, 0.9F, 0.4F, 1.0F };
        gradient.angle = 0.4F;
        renderer.RenderGradientRect(moth_ui::IntRect{ { 16, 72 }, { 336, 152 } }, gradient);

        // A clip that cuts the next rect in half, which is the scissor path.
        renderer.PushClip(moth_ui::IntRect{ { 16, 168 }, { 176, 248 } });
        renderer.PushColor(moth_ui::Color{ 0.95F, 0.85F, 0.2F, 1.0F });
        renderer.RenderFilledRect(moth_ui::IntRect{ { 16, 168 }, { 336, 248 } });
        renderer.PopColor();
        renderer.PopClip();

        // A transform on the last one, to show the matrix reaching the vertex.
        renderer.PushTransform(moth_ui::FloatMat4x4::Translation({ 200.0F, 100.0F }));
        renderer.PushColor(moth_ui::Color{ 0.8F, 0.8F, 0.9F, 1.0F });
        renderer.RenderRect(moth_ui::IntRect{ { 16, 168 }, { 136, 248 } });
        renderer.PopColor();
        renderer.PopTransform();

        if (image != nullptr) {
            const moth_ui::IntRect source{ { 0, 0 }, image->GetDimensions() };

            // Stretched to twice its size, so a wrong texture coordinate shows
            // as a shifted corner mark rather than as one texel of colour.
            renderer.PushTextureFilter(moth_ui::TextureFilter::Linear);
            renderer.RenderImage(*image, source, moth_ui::IntRect{ { 368, 16 }, { 496, 144 } },
                                 moth_ui::ImageScaleType::Stretch, 1.0F);
            renderer.PopTextureFilter();

            // Tiled at half size, which puts four tiles across and three down.
            renderer.RenderImage(*image, source, moth_ui::IntRect{ { 368, 160 }, { 496, 256 } },
                                 moth_ui::ImageScaleType::Tile, 0.5F);

            // The same image under a tint and a transform. The tint reaches the
            // image through the vertex colour, and the transform through the
            // corner, so this covers both against one draw that shows neither.
            renderer.PushTransform(moth_ui::FloatMat4x4::Translation({ 144.0F, 0.0F }));
            renderer.PushColor(moth_ui::Color{ 0.4F, 0.7F, 1.0F, 1.0F });
            renderer.RenderImage(*image, source, moth_ui::IntRect{ { 368, 16 }, { 432, 80 } },
                                 moth_ui::ImageScaleType::Stretch, 1.0F);
            renderer.PopColor();
            renderer.PopTransform();
        }

        if (font != nullptr) {
            // One box for each corner of the alignment pair, so a swapped
            // horizontal and vertical shows as text in the wrong corner rather
            // than as text that is merely a few pixels out.
            record_text_probe(renderer, *font, moth_ui::IntRect{ { 16, 380 }, { 300, 460 } },
                              "Left Top", moth_ui::TextHorizAlignment::Left,
                              moth_ui::TextVertAlignment::Top);
            record_text_probe(renderer, *font, moth_ui::IntRect{ { 316, 380 }, { 600, 460 } },
                              "Center Middle", moth_ui::TextHorizAlignment::Center,
                              moth_ui::TextVertAlignment::Middle);
            record_text_probe(renderer, *font, moth_ui::IntRect{ { 16, 476 }, { 300, 556 } },
                              "Right Bottom", moth_ui::TextHorizAlignment::Right,
                              moth_ui::TextVertAlignment::Bottom);

            // A paragraph that has to wrap, with a blank line in it. The blank
            // line is the part that used to collapse, so it stays in the probe.
            record_text_probe(renderer, *font, moth_ui::IntRect{ { 316, 476 }, { 600, 660 } },
                              "The quick brown fox jumps over the lazy dog.\n\nAV kerns.",
                              moth_ui::TextHorizAlignment::Left,
                              moth_ui::TextVertAlignment::Top);
        }

        renderer.end();
    }
#endif

    /// What draw_frame() needs that does not change from one frame to the next.
    struct FrameContext {
        engine::gfx::Device* device = nullptr;
        /// The shadow, cull, mesh, and tonemap passes, and the barriers between them.
        engine::render::SceneRenderer* scene = nullptr;
#if defined(ENGINE_WITH_UI)
        /// M6.2. Draws a moth_ui recording over the tonemapped frame.
        engine::ui::UiPass* ui_pass = nullptr;
        /// The recording ui_pass draws. Recorded inside draw_frame, because
        /// only there is the settled swapchain size known.
        engine::ui::Renderer* ui_renderer = nullptr;
        /// M6.3. The image the probe draws, or null when it did not resolve.
        const moth_ui::IImage* ui_image = nullptr;
        /// The font the probe draws text with, or null when none loaded.
        moth_ui::IFont* ui_font = nullptr;
        /// The instantiated layout, or null when none loaded.
        moth_ui::Node* ui_layout = nullptr;
#endif
        /// False when there is no window, so no ImGui and no input.
        bool overlay = false;
        /// The game content tree, which holds the cooked meshes.
        const engine::assets::Content* game_content = nullptr;
        /// The engine content tree, which holds the cooked shaders.
        const engine::assets::Content* engine_content = nullptr;
        ViewSettings* settings = nullptr;
        engine::scene::World* world = nullptr;
        /// The bodies of the scene. A scene reload builds them again.
        engine::physics::Simulation* simulation = nullptr;
        /// M7.5. Draws the wireframe of those bodies when the toggle is on.
        engine::render::DebugLinePass* debug_lines = nullptr;
        /// The entity the inspector edits, or entt::null for none.
        entt::entity* selected = nullptr;
        /// The cooked game content directory, which holds the scene and the prefabs.
        std::filesystem::path content;
        /// The scene a person edits, or empty when no source tree is there.
        std::filesystem::path source_scene;
    };

    /// The physics wireframe of the current frame. Kept here rather than in the
    /// frame arena because it holds its memory between frames, so a frame with
    /// the toggle on allocates nothing after the first one. A frame with it off
    /// never touches this at all.
    std::vector<engine::physics::DebugLine> g_debug_lines;

    FrameOutcome draw_frame(const FrameContext& context, engine::gfx::Extent2D extent,
                            engine::gfx::Extent2D& out_extent) {
        engine::gfx::Device* device = context.device;
        ViewSettings& settings = *context.settings;
        engine::scene::World& world = *context.world;

        engine::gfx::FrameInfo info;
        engine::gfx::Result result = engine::gfx::begin_frame(device, &info);

        if (result == engine::gfx::Result::OutOfDate) {
            return rebuild_swapchain(device, extent, *context.scene) ? FrameOutcome::Skipped
                                                                     : FrameOutcome::Failed;
        }
        if (!engine::gfx::succeeded(result)) {
            ENGINE_LOG_CRITICAL("begin_frame failed: {}", engine::gfx::result_name(result));
            return FrameOutcome::Failed;
        }

        // Resets the timestamp pool and reads what the frame before it wrote.
        context.scene->begin_frame(info.commands);

        // The overlay opens after the frame does, so a skipped frame never
        // leaves an ImGui frame half open.
        if (context.overlay) {
            engine::gfx::imgui_new_frame();
            // The panels are shared with the editor and place themselves
            // nowhere, so this overlay says where each one opens. The editor
            // docks them instead and calls none of this.
            engine::editor::place_next_panel(kPanelMargin, kPanelMargin, kPanelWidth,
                                             kViewHeight);
            if (engine::editor::draw_view_panel(settings, kViewPath)) {
                // A real editor marks the document dirty here. The overlay only
                // needs to show that the inspector reports a change.
                ENGINE_LOG_TRACE("The user changed the view.");
            }
            engine::editor::place_next_panel(kPanelMargin, (2 * kPanelMargin) + kViewHeight,
                                             kPanelWidth, kWorldHeight);
            engine::editor::draw_world_panel(world, *context.selected, context.source_scene,
                                             *context.game_content);
            engine::editor::place_next_panel((2 * kPanelMargin) + kPanelWidth, kPanelMargin,
                                             kPanelWidth, kInspectorHeight);
            engine::editor::draw_inspector_panel(world, *context.selected);
        }

        // An edit in the inspector went around set_local(), so the matrices are
        // stale until this runs. Doing it here rather than before the windows is
        // what keeps the frame the user sees current with the frame they edited.
        world.update();

        // The scene, into the half float image the renderer owns. Every pass and
        // every barrier between them is in there, because the editor draws the
        // same four passes into a panel and a second copy of that order would
        // have to be kept in step by hand.
        //
        // info.extent, not the requested one. The device can settle on a
        // different size, and the cluster grid has to agree with the fragment
        // shader about which cell a pixel is in.
        const engine::Mat4 clip_from_world =
            engine::editor::view_projection(settings, aspect_ratio(info.extent));
        const engine::render::SceneView view{
            .clip_from_world = clip_from_world,
            .camera_position = settings.camera_position,
            .clear_color = { settings.clear_color.r, settings.clear_color.g,
                             settings.clear_color.b, 1.0F },
            .extent = info.extent,
        };
        if (!context.scene->draw_scene(info.commands, world, *context.game_content, view)) {
            return FrameOutcome::Failed;
        }

        // Then the frame itself. Black, because the full-screen triangle covers
        // every pixel. The clear color a person picked belongs to the scene
        // image above. The scope attaches no depth, because the triangle
        // neither reads nor writes it.
        constexpr engine::gfx::ColorRGBA kFrameClear{ 0.0F, 0.0F, 0.0F, 1.0F };
        engine::gfx::cmd_begin_rendering(info.commands, kFrameClear, false);
        context.scene->draw_tonemap(info.commands, settings.exposure);

        // M7.5. The physics wireframe, after the curve so the color Box3D chose
        // is the color on screen, and under the UI so a panel is never hidden
        // by it. It tests no depth, so a collider inside geometry still shows,
        // which is the case somebody is usually hunting.
        if (settings.physics_debug && context.simulation != nullptr &&
            context.debug_lines != nullptr) {
            context.simulation->world().debug_lines(g_debug_lines);
            context.debug_lines->draw(info.commands, clip_from_world, g_debug_lines);
        }

#if defined(ENGINE_WITH_UI)
        // M6.2. Game UI, in the same scope as the tonemap. It is authored in
        // display colors like the overlay below, so it draws after the curve
        // rather than through it.
        if (context.ui_pass != nullptr && context.ui_renderer != nullptr) {
            // info.extent, not the requested extent. The device can settle on a
            // different size, and the scissor and the vertex normalization both
            // have to agree with the image actually being drawn into.
            record_ui_probe(*context.ui_renderer, info.extent, context.ui_image,
                            context.ui_font, context.ui_layout);
            context.ui_pass->draw(info.commands, *context.ui_renderer, info.extent);
        }
#endif

        // The overlay goes over the tonemapped image rather than through it. It
        // is authored in display colors, so mapping it down with the scene would
        // be wrong twice over.
        if (context.overlay) {
            engine::gfx::imgui_render(info.commands);
        }
        engine::gfx::cmd_end_rendering(info.commands);

        result = engine::gfx::end_frame(device);
        if (result == engine::gfx::Result::OutOfDate) {
            // The frame did present. The swapchain is now stale or suboptimal, and
            // the window size alone does not report that, so rebuild here.
            if (!rebuild_swapchain(device, extent, *context.scene)) {
                return FrameOutcome::Failed;
            }
        } else if (!engine::gfx::succeeded(result)) {
            ENGINE_LOG_CRITICAL("end_frame failed: {}", engine::gfx::result_name(result));
            return FrameOutcome::Failed;
        }

        out_extent = info.extent;
        return FrameOutcome::Drawn;
    }

    /**
     * Everything the runtime owns for the whole run, in the order it is built.
     *
     * One struct rather than a ladder of locals, so shutdown is one call and
     * every early exit takes the same path.
     */
    struct Runtime {
        engine::platform::Window window;
        engine::gfx::Device* device = nullptr;
        /// The engine's own cooked assets: the two shaders and the split sum table.
        engine::assets::Content engine_content;
        /// The shadow, cull, mesh, and tonemap passes, and the barriers between
        /// them. It owns the half float image the scene renders into.
        engine::render::SceneRenderer scene;
#if defined(ENGINE_WITH_UI)
        /// M6.2. The moth_ui drawing surface and the pass that draws it.
        engine::ui::Renderer ui_renderer;
        engine::ui::UiPass ui_pass;
        /// M6.3. Turns a path in a layout into a cooked engine texture.
        engine::ui::ImageFactory ui_images;
        /// The one image the probe draws. Issue #200 replaces it with a layout.
        std::unique_ptr<moth_ui::IImage> ui_image;
        /// M6.4. Turns a font name in a layout into a rasterized atlas.
        engine::ui::FontFactory ui_fonts;
        /// The one font the probe draws, and the one a layout names.
        std::shared_ptr<moth_ui::IFont> ui_font;
        /// M6.5. What moth_ui needs to build a node tree: the two factories and
        /// the renderer. Held by pointer because Context takes them by pointer
        /// and has no default constructor.
        std::unique_ptr<moth_ui::Context> ui_context;
        /// The instantiated layout. Null when none loaded, and the probe then
        /// draws on its own.
        std::shared_ptr<moth_ui::Node> ui_layout;
#endif
        /// M7.5. Draws the physics wireframe. Costs nothing while it is off.
        engine::render::DebugLinePass debug_lines;
        /// The game's cooked assets, which today means the meshes a scene names.
        engine::assets::Content game_content;
        /// M4.5. Watches the game source tree and cooks what a person edits.
        engine::assets::HotReload reload;
        /// M4.5. The same for the engine tree, which holds the shaders and the table.
        engine::assets::HotReload engine_reload;
        /// M8.0. Keyboard and mouse, sampled once for each frame. Every reader
        /// asks it for an action by name rather than reading a device.
        engine::platform::Input input;

        /**
         * The same actions, on the fixed step instead of on the frame.
         *
         * A press edge is the difference between two update() calls, so which
         * clock update() runs on decides who can see one. The camera runs on
         * the frame and reads `input`. The game runs on the fixed step and
         * reads this.
         *
         * One Input for both cannot work. A frame often runs no step at all,
         * and offscreen it almost never does, so an edge raised and cleared
         * between two steps is one the game never sees. That is not a rare
         * race: it is what --throw-at-frame does every time.
         */
        engine::platform::Input step_input;

        /// Every device state since the last step, folded together. A key that
        /// was down on any frame in between is down for the step that follows.
        engine::platform::InputFrame pending;

        /// What the device actually read on the last frame, which is what
        /// `pending` goes back to once a step has taken it.
        engine::platform::InputFrame latest;
#if defined(ENGINE_WITH_LUA)
        /// M8.1. The interpreter, and one instance for each scripted entity.
        engine::script::Host script;

        /// The view pose a script reads. Rewritten at the top of every step,
        /// because a script holds the services only for the call they arrive on.
        engine::script::CameraView camera;
#endif
        bool overlay = false; ///< True once ImGui owns resources on the device.
    };

    /// The cooker that ships beside this executable.
    std::filesystem::path cooker_path() {
#if defined(_WIN32)
        constexpr const char* kCookerName = "cooker.exe";
#else
        constexpr const char* kCookerName = "cooker";
#endif
        return engine::platform::executable_directory() / kCookerName;
    }

    /**
     * Starts hot reload, or says why it is off.
     *
     * A machine with no source tree beside the executable cannot cook, and
     * that is not an error. The program runs on with the assets it already
     * has, which is what a shipped build does.
     */
#if defined(ENGINE_WITH_UI)
    /**
     * Builds the moth_ui node tree for the sandbox layout.
     *
     * The layout is a cooked asset, so it is read out of the cooked tree rather
     * than out of `sandbox/content`. That matters for more than tidiness:
     * `moth_ui::Layout::Load` resolves an image path against the directory the
     * layout was read from, so a layout read from the source tree would name
     * source images and none of them would be in the manifest.
     *
     * A failure here is not fatal. The layout is one part of the frame, and a
     * scene that draws without it is more useful than a runtime that refuses to
     * start.
     */
    void load_ui_layout(Runtime& runtime) {
        const std::filesystem::path path = runtime.game_content.root() / "ui/main.mothui";

        auto [layout, result] = moth_ui::Layout::Load(path);
        if (result != moth_ui::Layout::LoadResult::Success || !layout) {
            ENGINE_LOG_ERROR("The UI layout {} did not load, and nothing will draw from it.",
                             path.generic_string());
            return;
        }

        runtime.ui_context = std::make_unique<moth_ui::Context>(
            &runtime.ui_images, &runtime.ui_fonts, &runtime.ui_renderer);

        runtime.ui_layout = layout->Instantiate(*runtime.ui_context);
        if (!runtime.ui_layout) {
            ENGINE_LOG_ERROR("The UI layout {} loaded and would not instantiate.",
                             path.generic_string());
            runtime.ui_context.reset();
            return;
        }
        ENGINE_LOG_INFO("The UI layout {} loaded.", path.filename().generic_string());
    }
#endif

    void start_hot_reload(Runtime& runtime, const Options& options) {
        if (!options.hot_reload) {
            ENGINE_LOG_INFO("Hot reload is off, because --no-watch was given.");
            return;
        }

        const engine::assets::HotReloadDesc desc{
            .source = options.watch.empty() ? std::filesystem::path{ ENGINE_GAME_CONTENT_SOURCE }
                                            : std::filesystem::path{ options.watch },
            .cooker = cooker_path(),
        };
        // HotReload::start reports a source tree that is not there, so this
        // does not check for one first.
        (void)runtime.reload.start(desc);

        // The engine tree is watched as well, because it holds the shaders and
        // a shader is the asset most worth editing live. --watch names the game
        // tree only, so this one has no override: a person who moved the game
        // content still has the engine content where the build put it.
        const engine::assets::HotReloadDesc engine_desc{
            .source = std::filesystem::path{ ENGINE_ENGINE_CONTENT_SOURCE },
            .cooker = cooker_path(),
        };
        (void)runtime.engine_reload.start(engine_desc);
    }

    /**
     * Whether a reloaded identity is one the world was built out of.
     *
     * A mesh or a texture swaps in behind the entities that already name it,
     * and the world does not change. A scene or a prefab describes the
     * entities themselves, so the world has to be built again.
     *
     * The reload says what each identity was, a removal included, so deleting a
     * prefab a scene instances is seen here rather than needing a restart.
     *
     * The rebuild is the whole world and not the part that changed. Doing less
     * means knowing which entities came from which prefab and building only
     * those, and holding a selection across it. That is the editor's job and it
     * arrives with the editor. Rebuilding everything costs a scene load, which
     * is what a person just asked for by saving.
     */
    bool world_was_built_from(const std::vector<engine::assets::AssetChange>& changed) {
        for (const engine::assets::AssetChange& change : changed) {
            const std::filesystem::path cooked{ change.cooked };
            if (cooked.extension() == ".scene" || cooked.extension() == ".prefab") {
                return true;
            }
        }
        return false;
    }

    /// The identities out of a change list, which is what the caches take.
    std::vector<engine::Guid> identities_of(
        const std::vector<engine::assets::AssetChange>& changed) {
        std::vector<engine::Guid> out;
        out.reserve(changed.size());
        for (const engine::assets::AssetChange& change : changed) {
            out.push_back(change.guid);
        }
        return out;
    }

    /**
     * Loads every changed script again, and restarts what was running it.
     *
     * A script that will not compile is reported by reload() and changes
     * nothing, so the entity carries on with the text it already had. That is
     * what makes saving in the middle of an edit safe.
     *
     * **A script the manifest no longer holds is left alone.** Its bytes are
     * already in this process and the entities running it keep running it.
     * Deleting a source file during a session should not stop a game, and the
     * next start reports the entity as naming a script nobody loaded.
     *
     * @param runtime Holds the open content and the host.
     * @param changed What the cook reported.
     */
    void reload_scripts(Runtime& runtime,
                        const std::vector<engine::assets::AssetChange>& changed) {
#if defined(ENGINE_WITH_LUA)
        for (const engine::assets::AssetChange& change : changed) {
            if (change.gone || !std::string_view{ change.cooked }.ends_with(
                                   engine::assets::kScriptExtension)) {
                continue;
            }

            std::vector<std::byte> bytes;
            if (!runtime.game_content.read_bytes(change.guid, bytes)) {
                ENGINE_LOG_ERROR("{} cooked and will not read, so it was not loaded again.",
                                 change.cooked);
                continue;
            }
            if (runtime.script.reload(change.guid, change.cooked, bytes)) {
                ENGINE_LOG_INFO("{} was read again.", change.cooked);
            }
        }
#else
        (void)runtime;
        (void)changed;
#endif
    }

    /**
     * Cooks whatever changed and swaps it in.
     *
     * Call this between frames. MeshPass::reload() waits for the frames in
     * flight before it frees anything, which cannot happen inside one.
     */
    void apply_hot_reload(Runtime& runtime, const FrameContext& context,
                          engine::scene::World& world, engine::scene::StepMotion& motion) {
        std::vector<engine::assets::AssetChange> changed;

        // The engine tree holds the two shaders and the split sum lookup table.
        // They share a tree but not a reload path. Editing a shader rebuilds
        // only the pipelines, and editing ibl.brdf.meta reloads only the table.
        //
        // The GUID is captured before poll(), because poll() updates the
        // manifest. A BRDF that was removed would then look the same as a file
        // that was never there, and its gone-guid would drive a pipeline rebuild
        // rather than a table reload.
        const engine::assets::ManifestEntry* previous_brdf =
            runtime.engine_content.find("ibl.brdf");
        const engine::Guid previous_brdf_guid =
            previous_brdf != nullptr ? previous_brdf->guid : engine::Guid{};

        if (runtime.engine_reload.poll(runtime.engine_content, changed)) {
            bool had_shader = false;
            bool had_brdf = false;
            const engine::assets::ManifestEntry* current_brdf =
                runtime.engine_content.find("ibl.brdf");
            const engine::Guid current_brdf_guid =
                current_brdf != nullptr ? current_brdf->guid : engine::Guid{};

            for (const engine::assets::AssetChange& change : changed) {
                if ((previous_brdf_guid.valid() && change.guid == previous_brdf_guid) ||
                    (current_brdf_guid.valid() && change.guid == current_brdf_guid)) {
                    had_brdf = true;
                } else {
                    had_shader = true;
                }
            }

            if (had_shader) {
                (void)runtime.scene.reload_shaders(runtime.engine_content);
            }
            if (had_brdf) {
                (void)runtime.scene.mesh().reload_brdf_lut(runtime.engine_content);
            }
        }

        if (!runtime.reload.poll(runtime.game_content, changed)) {
            return;
        }

        runtime.scene.mesh().reload(identities_of(changed));
        reload_scripts(runtime, changed);
        // A mesh or a texture swapped in behind the entities that name it, so
        // the world stands and whatever was selected is still that entity.
        if (!world_was_built_from(changed)) {
            return;
        }

        // Every entity goes, so anything holding one lets go first. StepMotion
        // holds the pose of everything the game moved, keyed by entity, and
        // EnTT hands the same numbers out again after a clear.
        *context.selected = entt::null;
        motion.clear();
        world.clear();
        engine::scene::prefabs().clear();

        if (!sandbox::load(context.content, &runtime.game_content, world)) {
            // An empty world is what a broken scene looks like, and the log
            // above says which file. Saving a working one loads it again, so
            // this never ends the process.
            ENGINE_LOG_ERROR("The scene did not load, so the world is empty. Fix the file "
                             "and save it again.");
            // The bodies of the scene that just went away, so nothing steps a
            // body whose entity no longer exists.
            context.simulation->build(world);
            return;
        }
        // The entities are new, so every body is stale. build() throws the old
        // ones away and reads the scene again.
        context.simulation->build(world);
        ENGINE_LOG_INFO("The scene was read again. The world holds {} entities.", world.size());
    }

    /// @return True when everything started. The caller calls stop() either way.
    bool start(Runtime& runtime, const Options& options) {
        // The engine content tree holds the shaders, so it opens before the
        // device builds a pipeline out of them.
        if (!runtime.engine_content.open(engine::platform::cooked_content_root() / "engine")) {
            ENGINE_LOG_CRITICAL("The engine content is missing. Build the cooker target.");
            return false;
        }

        // No window at all when drawing offscreen. Opening one and hiding it
        // would still need a desktop, which is half of what this avoids.
        if (!options.offscreen) {
            engine::platform::WindowDesc window_desc{ .title = "Camina Engine (M4 sandbox)" };
            if (options.resolution.width != 0) {
                window_desc.width = static_cast<int>(options.resolution.width);
                window_desc.height = static_cast<int>(options.resolution.height);
            }
            if (!runtime.window.create(window_desc)) {
                return false;
            }
        }

        // Bound whether or not a window opened. An offscreen run reads every
        // action as false, and binding costs nothing, so the two paths stay the
        // same shape.
        bind_actions(runtime.input);
        bind_actions(runtime.step_input);

        const engine::gfx::DeviceDesc device_desc{
            .window = options.offscreen ? nullptr : runtime.window.native(),
            .app_name = "camina",
            .enable_validation = options.validation,
            .enable_sync_validation = options.sync_validation,
            .vsync = options.vsync,
            .offscreen_extent = options.resolution.width != 0
                                    ? options.resolution
                                    : engine::gfx::Extent2D{ engine::gfx::kDefaultOffscreenWidth,
                                                             engine::gfx::kDefaultOffscreenHeight },
        };
        engine::gfx::Result result = engine::gfx::create_device(device_desc, &runtime.device);
        if (!engine::gfx::succeeded(result)) {
            ENGINE_LOG_CRITICAL("The renderer did not start: {}",
                                engine::gfx::result_name(result));
            return false;
        }

        // Every scene pass, and the scene image they render into. After the
        // device, because that image is the size the swapchain settled on
        // rather than the size that was asked for.
        if (!runtime.scene.create(runtime.device, runtime.engine_content,
                                  device_extent(runtime.device))) {
            return false;
        }

        // Before the first cull, so the grid is allocated once at the size this
        // asks for rather than at the default and then again.
        runtime.scene.mesh().set_cluster_cell_ceiling(options.cluster_cell_lights);

#if defined(ENGINE_WITH_UI)
        // M6.2. Built after the engine content tree is read, because the
        // pipelines come from the cooked ui shaders in it. A failure here is
        // not fatal: UiPass::create clears its device on the way out, so the
        // pass reports itself not ready and draws nothing.
        if (!runtime.ui_pass.create(runtime.device, runtime.engine_content)) {
            ENGINE_LOG_ERROR("The UI pass did not build. Game UI will not draw.");
        }
#endif

        // M7.5. It draws inside the tonemap scope, so it is built beside it.
        if (!runtime.debug_lines.create(runtime.device, runtime.engine_content)) {
            return false;
        }

        if (options.offscreen) {
            // The ImGui SDL backend needs the window. A capture with no panels
            // over it is also the more useful one to compare.
            ENGINE_LOG_INFO("Drawing offscreen at {}x{}. There is no overlay and no window.",
                            device_desc.offscreen_extent.width,
                            device_desc.offscreen_extent.height);
            return true;
        }

        // No docking and no layout file. The panels below open at the constants
        // near the top of this file, so a run always starts the same way, and
        // an overlay that dropped an imgui.ini beside the executable would
        // surprise people. The editor asks for both. See DESIGN.md section 10.
        const engine::gfx::ImGuiDesc imgui_desc{ .sdl_window = runtime.window.native() };
        result = engine::gfx::imgui_init(runtime.device, imgui_desc);
        if (!engine::gfx::succeeded(result)) {
            ENGINE_LOG_CRITICAL("The overlay did not start: {}",
                                engine::gfx::result_name(result));
            return false;
        }
        runtime.overlay = true;

        // ImGui reads every event, and the window still acts on the ones it owns.
        runtime.window.set_event_hook(
            [](const void* event, void* /*user*/) { engine::gfx::imgui_process_event(event); },
            nullptr);
        return true;
    }

    /// Releases what start() built, in the opposite order. Safe after a partial start.
    void stop(Runtime& runtime) {
        if (runtime.device != nullptr) {
            // The resources must go after the device stops using them.
            engine::gfx::device_wait_idle(runtime.device);
        }
        runtime.window.set_event_hook(nullptr, nullptr);
        if (runtime.overlay) {
            engine::gfx::imgui_shutdown(runtime.device);
        }
#if defined(ENGINE_WITH_UI)
        // Outermost first. The node tree holds an IImage and an IFont, those
        // hold texture handles, and the two factories own the textures. So the
        // order is tree, then the things it points at, then the factories that
        // own those.
        runtime.ui_layout.reset();
        runtime.ui_context.reset();

        runtime.ui_image.reset();
        runtime.ui_images.destroy();
        runtime.ui_font.reset();
        runtime.ui_fonts.destroy();
        runtime.ui_pass.destroy();
#endif
        runtime.debug_lines.destroy();
        // Before the device goes. The passes free buffers, textures, and
        // pipelines through the device, and their destructors run when Runtime
        // goes out of scope, which is after this function returns.
        runtime.scene.destroy();
        if (runtime.device != nullptr) {
            engine::gfx::destroy_device(runtime.device);
        }
        runtime.window.destroy();
    }

    /**
     * Reports what the last frame drew and what it culled.
     *
     * Separate from the frame time, and not behind the warmup guard that report
     * needs. A cull that works changes no pixel, so these counts are the only
     * way to see that it ran, and a short run is exactly when somebody is
     * checking that. A run of 60 frames or fewer has no frame time to report and
     * still drew something.
     */
    void report_scene_counts(const engine::render::MeshPass& mesh,
                             const engine::render::ShadowPass& shadow) {
        ENGINE_LOG_INFO("lights | {} lit the last frame | {} culled by the frustum | buffer holds {}",
                        mesh.visible_light_count(), mesh.culled_light_count(),
                        mesh.light_capacity());

        // The per-cell capacity is what says whether a crowded cell can drop a
        // light. It holds every visible light unless the scene passed the
        // ceiling, and then the drop is possible and the message says so.
        ENGINE_LOG_INFO("clusters | {} cells | {} lights for each cell | {}",
                        engine::render::kClusterCellCount, mesh.cluster_cell_capacity(),
                        mesh.cluster_may_drop() ? "a crowded cell drops the rest"
                                                : "no cell can drop a light");

        ENGINE_LOG_INFO("meshes | {} draws the last frame | {} entities culled by the frustum",
                        mesh.draw_count(), mesh.culled_mesh_count());

        // The shadow counts run over every cascade, so one entity is tested four
        // times. It can be culled from some cascades and kept in others.
        ENGINE_LOG_INFO("shadows | {} draws over {} cascades | {} culled by the cascade volumes",
                        shadow.draw_count(), engine::render::kCascadeCount,
                        shadow.culled_count());
    }

    /**
     * Reports what the run cost, or says why the numbers mean nothing.
     *
     * The median is the number to compare two runs with. The mean moves with a
     * single hitch and the low is the best case, so neither says much about a
     * change. The high and p99 are there to show a hitch rather than hide it
     * inside an average.
     *
     * Vsync makes every one of them the refresh rate, so the report says so
     * rather than printing 16.67 and letting a reader draw a conclusion from it.
     */
    /**
     * Reports what the simulation did, and what it gave up.
     *
     * The dropped time is the part worth reading. It is simulated time the run
     * will never make up, so anything above zero says the machine could not
     * keep up with the step rate that was asked for.
     */
    void report_physics(const engine::FixedTimestep& clock, const engine::FrameStats& stats) {
        const engine::FrameSummary run = stats.summarize();
        ENGINE_LOG_INFO("physics on the cpu | {:.0f} Hz | {} steps | median {:.3f} ms each frame",
                        clock.rate_hz(), clock.steps_taken(), run.median_ms);

        if (clock.drop_events() == 0) {
            return;
        }
        ENGINE_LOG_WARN("physics dropped {:.3f} s of simulated time over {} frames that hit the "
                        "{} step ceiling. The simulation is behind by that much and does not "
                        "make it up. Lower --physics-hz, or find what made those frames slow.",
                        clock.dropped_seconds(), clock.drop_events(), clock.max_steps());
    }

    void report_frame_time(const engine::FrameStats& stats, const Options& options,
                           const engine::render::SceneRenderer& scene) {
        if (stats.counted() == 0) {
            ENGINE_LOG_INFO("No frame time to report. A run needs more than {} frames.",
                            kFrameStatsWarmup);
            return;
        }

        const engine::FrameSummary run = stats.summarize();
        ENGINE_LOG_INFO(
            "frame time over {} frames | median {:.3f} ms | mean {:.3f} ms | "
            "p95 {:.3f} ms | p99 {:.3f} ms | low {:.3f} ms | high {:.3f} ms",
            run.count, run.median_ms, run.mean_ms, run.p95_ms, run.p99_ms, run.low_ms,
            run.high_ms);

        // Nanoseconds to milliseconds.
        constexpr double kToMilliseconds = 1e-6;
        const double shadow_ns = scene.gpu_pass_ns(engine::render::ScenePass::Shadow);
        const double cull_ns = scene.gpu_pass_ns(engine::render::ScenePass::Cull);
        const double mesh_ns = scene.gpu_pass_ns(engine::render::ScenePass::Mesh);
        const double tonemap_ns = scene.gpu_pass_ns(engine::render::ScenePass::Tonemap);
        if (shadow_ns > 0.0 || cull_ns > 0.0 || mesh_ns > 0.0 || tonemap_ns > 0.0) {
            // In the order they run. The cull is what says whether the cluster
            // grid pays for itself.
            ENGINE_LOG_INFO("gpu passes | shadow {:.3f} ms | cull {:.3f} ms | mesh {:.3f} ms | "
                            "tonemap {:.3f} ms",
                            shadow_ns * kToMilliseconds, cull_ns * kToMilliseconds,
                            mesh_ns * kToMilliseconds, tonemap_ns * kToMilliseconds);
        }

        if (options.vsync) {
            ENGINE_LOG_INFO("Vsync is on, so that is the refresh rate. Use --no-vsync to measure "
                            "a change.");
        }
    }


    /**
     * Everything the fixed step owns, which outlives any one frame.
     *
     * The clock, the simulated clock reading the game runs on, and the poses a
     * frame blends for whatever the game moved. Bundled because all three
     * advance together and none of them means anything alone.
     */
    struct StepState {
        engine::FixedTimestep clock;      ///< Turns a frame delta into whole steps.
        engine::scene::StepMotion motion; ///< What the game moved, and where it was before.
        double seconds = 0.0;             ///< Simulated seconds since the run began.
    };

#if defined(ENGINE_WITH_LUA)
    /**
     * Reads every cooked script in the game tree into the host.
     *
     * This walks the manifest rather than a list the game holds, the same way
     * sandbox::add_prefabs() does. A game that named its scripts in C++ could
     * load only its own content tree, and a tree the runtime never saw at build
     * time is exactly what `--content` points at.
     *
     * A script that will not compile is reported by load() and skipped. The
     * rest still load, because one broken script should not take the others
     * down with it.
     *
     * @param runtime Holds the open content and the host to fill.
     */
    void load_scripts(Runtime& runtime) {
        std::size_t loaded = 0;
        std::size_t failed = 0;

        for (const engine::assets::ManifestEntry& entry :
             runtime.game_content.manifest().entries) {
            for (const engine::assets::ManifestOutput& output : entry.outputs) {
                if (!std::string_view{ output.cooked }.ends_with(
                        engine::assets::kScriptExtension)) {
                    continue;
                }

                std::vector<std::byte> bytes;
                if (!runtime.game_content.read_bytes(output, bytes)) {
                    ENGINE_LOG_ERROR("{} is in the manifest and will not read.", output.cooked);
                    ++failed;
                    continue;
                }
                if (runtime.script.load(output.guid, output.cooked, bytes)) {
                    ++loaded;
                } else {
                    ++failed;
                }
            }
        }

        ENGINE_LOG_INFO("Loaded {} script(s), {} failed.", loaded, failed);
    }
#endif

#if defined(ENGINE_WITH_LUA)
    /**
     * What a script may reach on this step.
     *
     * Built for each call rather than held, so a reload that builds a new
     * simulation cannot leave a script driving the old one. See issue #273.
     *
     * The camera pose goes in because the throw is a script now and it throws
     * along the line of sight. The view belongs to the application rather than
     * to the scene, so it is handed over for the step. A `scene::Camera`
     * component is where it belongs once a scene carries one.
     *
     * @param runtime Holds the input and the host.
     * @param simulation The simulation this step ran on.
     * @param settings Where the camera stands and which way it looks.
     * @param motion Where a transform a script writes is recorded for blending.
     * @return The services to pass to the host.
     */
    [[nodiscard]] engine::script::Services
    step_services(Runtime& runtime, engine::physics::Simulation& simulation,
                  const ViewSettings& settings, engine::scene::StepMotion& motion) {
        runtime.camera = engine::script::CameraView{ .position = settings.camera_position,
                                                     .forward = camera_forward(settings) };
        return engine::script::Services{
            .physics = &simulation,
            .input = &runtime.step_input,
            .prefabs = &engine::scene::prefabs(),
            .camera = &runtime.camera,
            .motion = &motion,
        };
    }
#endif

    /**
     * Runs the game logic for one fixed step.
     *
     * **Every line of it is Lua**, which M8.6 closed. The seconds are simulated
     * seconds and never the wall clock, which is what keeps a run reproducible.
     * See DESIGN.md section 9 and issue #245.
     *
     * @param runtime Holds the script host.
     * @param world The scene to run.
     * @param state The simulated seconds and the poses a frame blends.
     * @param settings Where the camera stands, which the throw reads.
     * @param simulation The bodies a script may push.
     */
    void run_game_step(Runtime& runtime, engine::scene::World& world, StepState& state,
                       const ViewSettings& settings,
                       engine::physics::Simulation& simulation) {
#if defined(ENGINE_WITH_LUA)
        // Passed on each step rather than held, so a reload that builds a new
        // simulation cannot leave a script driving the old one. See issue #273.
        runtime.script.update(world, state.seconds,
                              step_services(runtime, simulation, settings, state.motion));
#else
        (void)runtime;
        (void)world;
        (void)state;
        (void)settings;
        (void)simulation;
#endif
    }

    /**
     * Hands the scripts what the step that just ran reported.
     *
     * This runs inside the step loop and not once for each frame. The simulation
     * keeps the events of one step only, so a frame that ran three steps and
     * read them once would report the third and throw the first two away. An
     * overlap lost because two things happened in one frame is the kind nobody
     * reproduces. See issue #263.
     *
     * @param runtime Holds the script host.
     * @param world The world the events name.
     * @param simulation The simulation that has just stepped.
     * @param settings Where the camera stands, which a callback may read.
     * @param motion Where a transform a callback writes is recorded.
     */
    void report_physics_events(Runtime& runtime, engine::scene::World& world,
                               engine::physics::Simulation& simulation,
                               const ViewSettings& settings,
                               engine::scene::StepMotion& motion) {
#if defined(ENGINE_WITH_LUA)
        runtime.script.deliver_physics_events(
            world, simulation, step_services(runtime, simulation, settings, motion));
#else
        (void)runtime;
        (void)world;
        (void)simulation;
#endif
    }

    /**
     * Runs the whole steps this frame owes, then blends the pose it draws.
     *
     * Physics runs after the game, because the game moves the kinematic bodies
     * and the solver has to see this step's positions.
     *
     * @param runtime Passed through to the game step, which needs the scripts.
     * @param state The clock, the simulated seconds, and the game poses.
     * @param settings Where the step rate and the ceiling are kept.
     * @param simulation The bodies to step.
     * @param world The scene to read and to write the drawn pose into.
     * @param delta_seconds How much wall time this frame took.
     * @param stats Receives what the whole of it cost on the CPU.
     */
    void advance_simulation(Runtime& runtime, StepState& state, const ViewSettings& settings,
                            engine::physics::Simulation& simulation, engine::scene::World& world,
                            float delta_seconds, engine::FrameStats& stats) {
        // The inspector can move both of these while the program runs, so they
        // are read each frame rather than at startup. Neither call throws away
        // the time already accumulated.
        state.clock.set_rate_hz(settings.physics_hz);
        state.clock.set_max_steps(settings.max_physics_steps);

        const auto started = std::chrono::steady_clock::now();
        for (std::uint32_t left = state.clock.advance(delta_seconds); left > 0; --left) {
            ENGINE_PROFILE_ZONE_N("fixed step");

            // The actions the game reads move to this step, and the fold starts
            // again from whatever the device last read. So a key held across
            // several steps stays held, and a key tapped and let go between two
            // of them still raises exactly one edge.
            runtime.step_input.update(runtime.pending);
            runtime.pending = runtime.latest;

            // The game reads the pose the last step left rather than the blend
            // the last frame drew. Without this the motion of a frame that fell
            // between two steps feeds back in and compounds.
            state.motion.begin_step(world);

            // Simulated seconds, which is what makes a run reproducible. A
            // double all the way to the game. A float resolves steps of 1/60
            // until about three days of running, and then two steps in a row
            // land on the same number and the game stops advancing. See #245.
            state.seconds += static_cast<double>(state.clock.step_seconds());

            // The game moves things, then the solver runs. A kinematic body the
            // game drives has to carry its new transform into the step, so this
            // order is the one that works.
            run_game_step(runtime, world, state, settings, simulation);
            simulation.step(world, state.clock.step_seconds());

            // Inside the loop, because the simulation keeps one step of events.
            // Reading them after the loop would report the last step alone.
            report_physics_events(runtime, world, simulation, settings, state.motion);
        }

        // One alpha for both, because they blend the same pair of steps. Two
        // would let the game and the physics draw different instants.
        const float alpha = state.clock.alpha();
        simulation.interpolate(world, alpha);
        state.motion.interpolate(world, alpha);

        stats.add(
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started)
                .count());
    }

    /// What the start of a frame decided. A frame that cannot draw yet says so,
    /// rather than falling through to a draw that has nothing to draw into.
    enum class FrameStart {
        Draw,   ///< The window is up and the swapchain matches it.
        Skip,   ///< Nothing to draw this time around.
        Failed, ///< The swapchain would not rebuild.
    };

    /// Runs everything a frame needs before it draws: the minimized wait, any
    /// pending hot reload, and the swapchain rebuild after a resize.
    ///
    /// @param runtime The window, the device, and the passes.
    /// @param context What a frame draws with.
    /// @param options The command line.
    /// @param world The scene a reload rebuilds.
    /// @param last_extent The size the swapchain was built for. A rebuild
    ///                    replaces it.
    /// @param last_frame The time the last frame ran. A wait moves it, so the
    ///                   next delta does not count the idle time.
    /// @return What the caller should do with this frame.
    FrameStart begin_frame(Runtime& runtime, const FrameContext& context, const Options& options,
                           engine::scene::World& world, engine::scene::StepMotion& motion,
                           engine::gfx::Extent2D& last_extent,
                           std::chrono::steady_clock::time_point& last_frame) {
        if (!options.offscreen && runtime.window.minimized()) {
            // poll() does not block, so without this the loop pins one core
            // while the window is minimized and there is nothing to draw.
            std::this_thread::sleep_for(std::chrono::milliseconds(kMinimizedSleepMs));
            last_frame = std::chrono::steady_clock::now();
            return FrameStart::Skip;
        }

        // Between frames, because freeing a resource waits for the frames
        // in flight and a frame cannot wait for itself.
        apply_hot_reload(runtime, context, world, motion);

        // Offscreen the size is fixed for the whole run, which is the point of
        // it. Nothing resizes, so nothing rebuilds. The window reports its new
        // size before the swapchain knows about it.
        const engine::gfx::Extent2D extent =
            options.offscreen ? last_extent : window_extent(runtime.window);
        if (extent.width == last_extent.width && extent.height == last_extent.height) {
            return FrameStart::Draw;
        }

        if (!rebuild_swapchain(runtime.device, extent, runtime.scene)) {
            return FrameStart::Failed;
        }
        last_extent = extent;
        // The swapchain was just rebuilt, so this frame has no image to draw
        // into. draw_frame would see OutOfDate from the old extent and rebuild
        // again, which is the double rebuild issue #145.
        return FrameStart::Skip;
    }

    /// Works out how much wall time this frame covers.
    ///
    /// The game no longer reads a frame clock at all. It runs on the fixed
    /// step and counts simulated seconds, so this feeds the camera and the
    /// step accumulator and nothing else. See issue #245.
    ///
    /// @param options The command line.
    /// @param last_frame When the last frame ran.
    /// @param now The time this frame began.
    /// @return Seconds since the last frame.
    [[nodiscard]] float frame_delta(const Options& options,
                                    std::chrono::steady_clock::time_point last_frame,
                                    std::chrono::steady_clock::time_point now) {
        if (options.offscreen) {
            // Offscreen counts frames rather than reading the clock, so the
            // same command produces the same image. See kOffscreenStep.
            return kOffscreenStep;
        }
        // A long stall, a debugger break, or a driver hitch would otherwise
        // multiply move_speed by the whole gap and throw the camera across
        // the scene in one step.
        return std::min(std::chrono::duration<float>(now - last_frame).count(), kLongestFrame);
    }

    /// Says whether the frame limit has landed, and writes the screenshot when
    /// it has. The frame just presented is the one to capture, and the loop has
    /// not started another, so this is the only place a capture is certain of
    /// what it will read.
    ///
    /// @param runtime The device the capture reads.
    /// @param options The command line.
    /// @param frame The number of frames drawn so far.
    /// @return True when the loop should stop.
    [[nodiscard]] bool frame_limit_reached(Runtime& runtime, const Options& options,
                                           std::uint64_t frame) {
        if (options.max_frames == 0 || frame < options.max_frames) {
            return false;
        }
        ENGINE_LOG_INFO("Frame limit of {} reached. Exiting.", options.max_frames);
        if (!options.screenshot.empty()) {
            (void)runtime::write_screenshot(runtime.device, options.screenshot);
        }
        return true;
    }

    /**
     * Samples the devices once for this frame.
     *
     * This is the only place the runtime reads input. Every reader below it asks
     * the module for an action by name.
     *
     * An offscreen run has no window and no devices, so it feeds a default
     * frame. Every action then reads false, which is what the old
     * throw_pressed() spelled out with its own check on options.offscreen.
     *
     * ImGui is asked first, because it owns the keyboard while a person types in
     * a panel. platform/ sits below gfx/, so the module cannot ask on its own.
     */
    void update_input(Runtime& runtime, const FrameContext& context, const Options& options,
                      std::uint64_t frame) {
        engine::platform::InputFrame state;
        if (options.offscreen) {
            // No devices offscreen, so every key starts up.
            state = engine::platform::InputFrame{};
        } else {
            engine::platform::InputConsumed consumed;
            if (context.overlay) {
                engine::gfx::imgui_wants_input(&consumed.mouse, &consumed.keyboard);
            }
            state = engine::platform::sample(runtime.window, consumed);
        }

        // --throw-at-frame holds the throw key down for one frame rather than
        // calling the throw itself. The throw is a script now and it reads the
        // action, so a hook that went around the input module would drive a path
        // the game never takes. input.h calls this the replay shape: write a
        // frame out and feed it back. See DESIGN.md section 9.
        if (options.throw_at_frame != 0 && frame + 1 == options.throw_at_frame) {
            state.keys.at(static_cast<std::size_t>(kThrowKey)) = true;
        }

        runtime.input.update(state);

        // And fold it into what the next step will read. A key down on any
        // frame since the last step is down for that step, so an edge cannot
        // fall between two of them and go unseen.
        runtime.latest = state;
        for (std::size_t i = 0; i < state.keys.size(); ++i) {
            runtime.pending.keys.at(i) = runtime.pending.keys.at(i) || state.keys.at(i);
        }
        for (std::size_t i = 0; i < state.mouse_buttons.size(); ++i) {
            runtime.pending.mouse_buttons.at(i) =
                runtime.pending.mouse_buttons.at(i) || state.mouse_buttons.at(i);
        }
        runtime.pending.focused = runtime.pending.focused || state.focused;
    }

    /// Runs frames until the user quits, the frame limit lands, or a frame fails.
    bool run_frames(Runtime& runtime, const FrameContext& context, const Options& options,
                    engine::Arena& frame_arena, ViewSettings& settings,
                    engine::scene::World& world, engine::physics::Simulation& simulation) {
        // The settings own the rate. This reads them here and again each frame,
        // because the inspector can move either one while the program runs.
        StepState step;
        step.clock = engine::FixedTimestep(settings.physics_hz, settings.max_physics_steps);

        std::uint64_t frame = 0;
        auto started = std::chrono::steady_clock::now();
        auto last_report = started;
        auto last_frame = started;
        // The size the swapchain is built for. begin_frame replaces it when the
        // window moves, and offscreen it never changes.
        engine::gfx::Extent2D last_extent =
            options.offscreen ? device_extent(runtime.device) : window_extent(runtime.window);

        engine::FrameStats stats(kFrameStatsWarmup);
        // The same warm-up, for the same reason. The first frames build the
        // bodies and touch every page of the solver's memory for the first time.
        engine::FrameStats physics_stats(kFrameStatsWarmup);
        // A period runs from the start of one drawn frame to the start of the
        // next. A frame the loop did not draw leaves no period to close, so the
        // paths that skip one clear this and the next frame starts a new
        // interval. Measuring across the gap would report the skip as a hitch.
        auto drawn_at = started;
        bool have_drawn = false;
        double period_ms = 0.0;

        // With no window there is nothing to poll and no way to quit by hand, so
        // an offscreen run ends on the frame limit alone.
        while (options.offscreen ? true : runtime.window.poll()) {
            ENGINE_PROFILE_ZONE_N("frame");
            frame_arena.reset();

            const FrameStart start =
                begin_frame(runtime, context, options, world, step.motion, last_extent,
                            last_frame);
            if (start == FrameStart::Failed) {
                return false;
            }
            if (start == FrameStart::Skip) {
                // No frame ran, so the next period must not start from one the
                // loop did not draw.
                have_drawn = false;
                continue;
            }

            const auto now = std::chrono::steady_clock::now();
            const float delta = frame_delta(options, last_frame, now);
            last_frame = now;

            update_input(runtime, context, options, frame);
            update_camera(runtime.window, runtime.input, settings, delta);

            // The game and the solver both run on the fixed step now, so this
            // is one call rather than two. The frame composes the matrices and
            // draws after it. Reversing those two would draw a frame behind.
            advance_simulation(runtime, step, settings, simulation, world, delta, physics_stats);

            // Arm the capture before the frame that will be the last one, because
            // the copy happens inside end_frame() while this side still owns the
            // image. frame counts what has been drawn, so the frame about to run
            // is frame + 1. See issue #124.
            //
            // A skipped frame leaves the request armed, because end_frame() is
            // what consumes it, and a frame that never ended never copied.
            if (!options.screenshot.empty() && options.max_frames > 0 &&
                frame + 1 >= options.max_frames) {
                engine::gfx::request_capture(runtime.device);
            }

            engine::gfx::Extent2D drawn_extent{};
            const FrameOutcome outcome = draw_frame(context, last_extent, drawn_extent);
            if (outcome == FrameOutcome::Failed) {
                return false;
            }
            if (outcome == FrameOutcome::Skipped) {
                have_drawn = false;
                continue;
            }

            ++frame;
            settings.frames_drawn = frame;

            if (have_drawn) {
                period_ms = std::chrono::duration<double, std::milli>(now - drawn_at).count();
                stats.add(period_ms);
            }
            drawn_at = now;
            have_drawn = true;

            if (now - last_report >= std::chrono::seconds(1)) {
                ENGINE_LOG_INFO("frame {} | {}x{} | {:.2f} ms | arena high water {} bytes | workers {}",
                                frame, drawn_extent.width, drawn_extent.height, period_ms,
                                frame_arena.high_water(), engine::jobs::worker_count());
                last_report = now;
            }

            if (frame_limit_reached(runtime, options, frame)) {
                break;
            }

            ENGINE_PROFILE_FRAME();
        }

        ENGINE_LOG_INFO("Camina Engine stopped after {} frames.", frame);
        report_scene_counts(context.scene->mesh(), context.scene->shadow());
        // After the frame time, so the physics cost reads beside the per-pass
        // GPU split that report_frame_time prints. The two are different
        // domains and the labels say so: the solver runs on the CPU.
        report_frame_time(stats, options, *context.scene);
        report_physics(step.clock, physics_stats);
        return true;
    }

} // namespace

int main(int argc, char** argv) {
    const Options options = parse_options(argc, argv);

    engine::log::init();
    ENGINE_LOG_INFO("Camina Engine {} starting.", engine::Version);

#if defined(ENGINE_WITH_UI)
    // M6. A Conan editable can shadow the cache, and a version range can pick a
    // different moth_ui than the one somebody is editing. So report the version
    // this binary actually linked rather than the one anybody expected.
    ENGINE_LOG_INFO("Game UI is on, linked against moth_ui {}. Self test {}.",
                    engine::ui::moth_ui_version(),
                    engine::ui::self_test() ? "passed" : "FAILED");
#endif

    engine::jobs::init();
    engine::Arena frame_arena(kFrameArenaBytes);

    Runtime runtime;
    if (!start(runtime, options)) {
        stop(runtime);
        engine::jobs::shutdown();
        engine::log::shutdown();
        return 1;
    }

    ViewSettings settings;
    engine::reflect::registry().add<ViewSettings>();
    // A view file next to the executable wins over the defaults, so a run
    // continues where the last one stopped.
    if (std::filesystem::exists(kViewPath) && engine::reflect::load_json(kViewPath, settings)) {
        ENGINE_LOG_INFO("Read {}.", kViewPath);
    }

    // After the file, because a flag on the command line is the more specific
    // of the two. A run that has to produce one exposure every time cannot rely
    // on whatever the last session happened to save.
    if (options.physics_debug) {
        settings.physics_debug = true;
        ENGINE_LOG_INFO("The physics wireframe is on, from --physics-debug.");
    }
    if (options.physics_hz > 0.0F) {
        settings.physics_hz = options.physics_hz;
        ENGINE_LOG_INFO("Physics steps at {} Hz, from --physics-hz.", settings.physics_hz);
    }
    if (options.exposure > 0.0F) {
        settings.exposure = options.exposure;
    }

    // The engine registers what it defines, then the game registers what it
    // defines. A scene loaded before this loses every component nobody claimed.
    // Physics registers its own, so scene/ needs no physics header.
    engine::scene::register_builtin_components();
    engine::physics::register_components();
#if defined(ENGINE_WITH_LUA)
    engine::script::register_components();
#endif
    sandbox::register_components();

    const std::filesystem::path content = options.content.empty()
                                              ? sandbox::default_content_directory()
                                              : std::filesystem::path{ options.content };

    // The cooked meshes a scene names live here. Opening it after start()
    // is fine, because nothing draws until the frame loop begins.
    if (!runtime.game_content.open(content)) {
        ENGINE_LOG_CRITICAL("The game content is missing. Build the cooker target.");
        stop(runtime);
        engine::jobs::shutdown();
        engine::log::shutdown();
        return 1;
    }

#if defined(ENGINE_WITH_UI)
    // M6.3. After the game content opens, because the factory resolves a path
    // against that manifest. A failure here is not fatal: the probe draws its
    // shapes and no image, and the log says which path did not resolve.
    //
    // This resolves once and a reload never revisits it, so editing a UI image
    // needs a restart. The handle cannot go stale, because the factory owns the
    // only cache that holds it and nothing drops from that cache. Issue #210
    // holds the reload path.
    if (!runtime.ui_images.create(runtime.device, &runtime.game_content)) {
        ENGINE_LOG_ERROR("The UI image factory did not start. No layout image will draw.");
    } else {
        runtime.ui_image = runtime.ui_images.GetImage("ui/panel.png");
    }

    // The same resolution story as the image above, and the same restart rule.
    //
    // A layout names a font by a registered name rather than by a path, so the
    // runtime registers the one the sandbox ships. That is the game telling the
    // engine what its fonts are called, and it has no equivalent for an image:
    // moth_ui names an image by a path and a font by a name. See DESIGN.md
    // section 8.4.
    if (!runtime.ui_fonts.create(runtime.device, &runtime.game_content)) {
        ENGINE_LOG_ERROR("The UI font factory did not start. No layout text will draw.");
    } else {
        runtime.ui_fonts.AddFont("body", "ui/fonts/LiberationSans-Regular.ttf");
        runtime.ui_font = runtime.ui_fonts.GetDefaultFont(kUiProbeFontSize);
    }

    // M6.5. One layout, which is the done-when test for M6.
    //
    // The tree is built once and never animated. Update() would advance the
    // keyframe tracks, and every track here holds one frame, so a static layout
    // needs no tick. Animation, input and widgets are all M10.
    load_ui_layout(runtime);
#endif

    engine::scene::World world;
    if (!sandbox::load(content, &runtime.game_content, world)) {
        ENGINE_LOG_CRITICAL("The game did not load. There is nothing to draw.");
        stop(runtime);
        engine::jobs::shutdown();
        engine::log::shutdown();
        return 1;
    }

    // After the first load, so the watcher takes its snapshot of a tree that
    // is already cooked and nothing arrives as a change on the first frame.
    start_hot_reload(runtime, options);
    const std::filesystem::path source = game_source_directory(options);

#if defined(ENGINE_WITH_LUA)
    // Before the first step, so an entity that names a script finds it loaded
    // rather than reporting it missing on the first frame.
    load_scripts(runtime);
#endif

    // After the world loads, because a body starts where its entity sits. It is
    // also after jobs::init(), because the solver runs on the job system and
    // the world asks it how many workers there are.
    engine::physics::Simulation simulation;
    simulation.build(world);

    entt::entity selected = entt::null;
    const FrameContext context{
        .device = runtime.device,
        .scene = &runtime.scene,
#if defined(ENGINE_WITH_UI)
        .ui_pass = &runtime.ui_pass,
        .ui_renderer = &runtime.ui_renderer,
        .ui_image = runtime.ui_image.get(),
        .ui_font = runtime.ui_font.get(),
        .ui_layout = runtime.ui_layout.get(),
#endif
        .overlay = runtime.overlay,
        .game_content = &runtime.game_content,
        .engine_content = &runtime.engine_content,
        .settings = &settings,
        .world = &world,
        .simulation = &simulation,
        .debug_lines = &runtime.debug_lines,
        .selected = &selected,
        .content = content,
        .source_scene = source.empty() ? std::filesystem::path{} : source / sandbox::kSceneFile,
    };

    const bool ok = run_frames(runtime, context, options, frame_arena, settings, world, simulation);

    stop(runtime);
    engine::jobs::shutdown();
    engine::log::shutdown();
    return ok ? 0 : 1;
}
