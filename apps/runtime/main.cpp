#include "assets/hot_reload.h"
#include "assets/manifest.h"
#include "assets/reference.h"
#include "assets/script.h"
#include "core/arena.h"
#include "core/frame_stats.h"
#include "core/jobs.h"
#include "core/log.h"
#include "core/profile.h"
#include "core/timestep.h"
#include "core/version.h"
#include "gfx/device.h"
#include "gfx/imgui.h"
#include "math/conventions.h"
#include "platform/input.h"
#include "platform/paths.h"
#include "platform/window.h"
#include "reflect/inspector.h"
#include "reflect/json.h"
#include "reflect/registry.h"
#include "render/material_cache.h"
#include "render/mesh_pass.h"
#include "render/shadow_pass.h"
#include "render/tonemap_pass.h"
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
#include "scene/scene_file.h"
#include "scene/components.h"
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
#include <fstream>
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

    /// Meters each second. Fast enough to knock a crate off a stack rather than
    /// nudge it, and slow enough that a person can watch it travel.
    constexpr float kThrowSpeed = 16.0F;

    /// How far in front of the camera a thrown crate starts, in meters. A body
    /// created at the camera fills the screen for one frame before it leaves.
    constexpr float kThrowOffset = 1.2F;

    /// Half the crate, in meters. The model is a one meter cube.
    constexpr float kCrateHalfExtent = 0.5F;

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

    /// Where each window opens. The overlay writes no imgui.ini, so a run
    /// always starts from this layout and a move lasts until the program ends.
    /// Without these all three open at the same place, and the last one drawn
    /// buries the rest. M9 gives the editor a real settings path.
    constexpr float kPanelMargin = 16.0F;
    constexpr float kPanelWidth = 340.0F;
    constexpr float kViewHeight = 320.0F;
    constexpr float kWorldHeight = 380.0F;
    constexpr float kInspectorHeight = 460.0F;

    /// Where the view settings go. The working directory, so a run is easy to redo.
    /// The scene itself lives in the sandbox content directory, not here.
    constexpr const char* kViewPath = "view.json";

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

    /// Which pass in the schedule is which. The order here is the order they run.
    constexpr std::size_t kShadowPassIndex = 0;
    /// The cluster cull. It sits between the shadow pass and the mesh pass
    /// because it is a compute dispatch, and one cannot happen inside a
    /// rendering scope.
    constexpr std::size_t kCullPassIndex = 1;
    constexpr std::size_t kMeshPassIndex = 2;
    constexpr std::size_t kTonemapPassIndex = 3;

    /**
     * Works out every barrier this frame needs.
     *
     * The pass list is built fresh each frame rather than kept, because a
     * declaration is a handful of spans over static storage and building it
     * costs nothing. Keeping it would mean invalidating it whenever a pass
     * changed what it touches.
     *
     * @param states What state each resource is in. Read and then updated, so
     * the shadow map carries its state into the next frame.
     * @param out The schedule. issue_pass_barriers() reads it one pass at a time.
     * @return False when a declaration was refused, which is a programming
     * error rather than a run-time condition.
     */
    [[nodiscard]] bool derive_frame_barriers(
        std::array<engine::gfx::ResourceState, engine::render::kFrameResourceCount>& states,
        engine::render::GraphSchedule& out) {
        const std::array passes{ engine::render::ShadowPass::declare(),
                                 engine::render::MeshPass::declare_cull(),
                                 engine::render::MeshPass::declare(),
                                 engine::render::TonemapPass::declare() };

        // The two frame targets start over every frame. The swapchain image is a
        // different image on almost every acquire, and the depth image is
        // scratch that nothing reads across a frame boundary.
        states[engine::render::kFrameColor.index] = engine::gfx::ResourceState::Undefined;
        states[engine::render::kFrameDepth.index] = engine::gfx::ResourceState::Undefined;
        // The shadow map, the scene color, and the cluster grid do not. Each is
        // shared across the frames in flight, so the state the last frame left
        // it in is what the barrier has to order against. Calling one Undefined
        // here would derive a barrier that waits on the stage the new state
        // uses, and what it has to wait for is the previous frame's fragment
        // shader reading it. That is a write after read, and it is the same
        // hazard #125 found on the shared depth image.

        if (!engine::render::derive_barriers(passes, states, out)) {
            ENGINE_LOG_CRITICAL("The frame declarations were refused, so no barrier is safe.");
            return false;
        }

        // Carry the shadow map's state into the next frame. The graph works this
        // out already, which is what final_states is for.
        for (std::size_t i = 0; i < states.size(); ++i) {
            states[i] = out.final_states[i];
        }
        return true;
    }

    /// Which image each graph resource is, for the ones the frame does not own.
    /// The two frame targets are null here, because an enum names those.
    using GraphTextures =
        std::array<engine::gfx::TextureHandle, engine::render::kFrameResourceCount>;

    /// Puts the barriers one pass needs into the command list.
    void issue_pass_barriers(engine::gfx::CommandList* commands,
                             const engine::render::GraphSchedule& schedule, std::size_t pass,
                             const GraphTextures& textures) {
        if (pass >= schedule.passes.size()) {
            return;
        }
        for (const engine::render::GraphBarrier& barrier : schedule.passes[pass].before) {
            if (barrier.resource == engine::render::kClusterGrid) {
                // A buffer, so there is no layout to change and nothing to
                // name. gfx::cmd_buffer_barrier is a global memory barrier, and
                // the two states carry the whole dependency.
                engine::gfx::cmd_buffer_barrier(commands, barrier.before, barrier.after);
                continue;
            }
            const engine::gfx::TextureHandle texture = textures[barrier.resource.index];
            if (texture.valid()) {
                // Not a frame target, so it is named by a handle rather than by
                // an enum. See gfx::cmd_texture_barrier.
                engine::gfx::cmd_texture_barrier(commands, texture, barrier.before, barrier.after);
                continue;
            }
            const engine::gfx::FrameTarget target =
                barrier.resource == engine::render::kFrameDepth
                    ? engine::gfx::FrameTarget::Depth
                    : engine::gfx::FrameTarget::Color;
            engine::gfx::cmd_frame_barrier(commands, target, barrier.before, barrier.after);
        }
    }

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
        std::string name = "sandbox view";
        engine::Vec3 clear_color{ 0.25F, 0.25F, 0.3F };

        /// Where the camera stands. Saved, so a run opens where the last one stopped.
        engine::Vec3 camera_position{ 0.0F, 2.8F, 6.0F };
        /// Degrees around +Y. Zero looks down -Z, which is forward per DESIGN.md section 3.
        float camera_yaw = 0.0F;
        /// Degrees up from the horizon. Clamped, so the camera never rolls over the top.
        float camera_pitch = -8.0F;

        float fov_degrees = 60.0F;
        float move_speed = 6.0F;
        float look_sensitivity = 0.12F;

        /**
         * A linear scale on the scene before the ACES curve. One is neutral.
         *
         * It lives here rather than on the camera because it is a property of
         * the view and this struct is what the view already is. A
         * `scene::Camera` field is where it belongs once a scene carries more
         * than one camera, which nothing does yet.
         */
        float exposure = 1.0F;

        /**
         * How many physics steps make one second.
         *
         * The simulation advances at this rate whatever the frame rate is, so
         * the same scene behaves the same way on two machines. See
         * engine::FixedTimestep and DESIGN.md section 9.
         */
        float physics_hz = engine::kDefaultStepHz;

        /**
         * How many steps one frame runs before it drops the time it owes.
         *
         * A frame slower than one step leaves time owed, and paying all of it
         * back makes the next frame slower still. This is the ceiling that
         * stops that. The frame report says how much time it discarded.
         */
        std::uint32_t max_physics_steps = engine::kDefaultMaxStepsPerFrame;

        /**
         * Whether to draw the physics wireframe over the frame.
         *
         * Off by default. A collider is invisible, and this is what makes one
         * that does not match its mesh a five second answer.
         */
        bool physics_debug = false;

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
            ENGINE_FIELD(ViewSettings, camera_position, Range{ -100.0, 100.0, 0.05 },
                         Category{ "Camera" }, Tooltip{ "Meters. Hold the right mouse button and use WASD" }),
            ENGINE_FIELD(ViewSettings, camera_yaw, Range{ -180.0, 180.0, 0.5 },
                         Category{ "Camera" }, Tooltip{ "Degrees around up. Zero looks down -Z" }),
            ENGINE_FIELD(ViewSettings, camera_pitch, Range{ -89.0, 89.0, 0.5 },
                         Category{ "Camera" }, Tooltip{ "Degrees above the horizon" }),
            ENGINE_FIELD(ViewSettings, fov_degrees, Range{ 20.0, 120.0, 0.5 }, Category{ "Camera" }),
            ENGINE_FIELD(ViewSettings, move_speed, Range{ 0.5, 40.0, 0.1 }, Category{ "Camera" },
                         Tooltip{ "Meters each second. Hold shift to go faster" }),
            ENGINE_FIELD(ViewSettings, look_sensitivity, Range{ 0.01, 1.0, 0.01 },
                         Category{ "Camera" }, Tooltip{ "Degrees for each mouse count" }),
            ENGINE_FIELD(ViewSettings, exposure, Range{ 0.05, 8.0, 0.01 },
                         Tooltip{ "Scales the scene before the ACES curve. One is neutral" }),
            // Live, so a person can drag the rate down and watch the blend hold
            // the motion together. That is the fastest way to see what the
            // interpolation is for.
            ENGINE_FIELD(ViewSettings, physics_hz, Range{ 1.0, 240.0, 1.0 },
                         Category{ "Physics" },
                         Tooltip{ "Simulation steps each second, whatever the frame rate" }),
            ENGINE_FIELD(ViewSettings, max_physics_steps, Range{ 1.0, 32.0, 1.0 },
                         Category{ "Physics" },
                         Tooltip{ "Steps one frame runs before it drops the time it owes" }),
            ENGINE_FIELD(ViewSettings, physics_debug, Category{ "Physics" },
                         Tooltip{ "Draw every collider as a wireframe, from what Box3D reports" }),
            // ReadOnly keeps the editor from changing it. Transient keeps it out
            // of the file. The two attributes are read by different consumers,
            // and neither consumer knows about the other.
            ENGINE_FIELD(ViewSettings, frames_drawn, ReadOnly{}, Transient{}, Category{ "Debug" }));
    }
};
// NOLINTEND(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)

namespace {

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

    /**
     * Writes the scene where a person edits it, references and all.
     *
     * The world holds identities, because that is what the engine reads. The
     * source document holds references, because an identity is derived and
     * nobody chose it. Writing the world straight out would replace every
     * reference with the GUID it resolved to, which undoes the reason a scene
     * may name an asset by path at all.
     */
    bool write_scene_source(const std::filesystem::path& path, const engine::scene::World& world,
                            const engine::assets::Content& content) {
        nlohmann::json document = engine::scene::save_scene(world);
        const std::size_t restored =
            engine::assets::restore_references(document, content.manifest());

        // Through a temporary in the same directory, then a rename. Writing
        // over the scene directly means a disk that fills up, or a close that
        // fails, leaves a person with half a scene and no copy of the whole
        // one. The rename is what makes the swap all or nothing, and it is
        // only reached once the bytes are down.
        std::filesystem::path staged = path;
        staged += ".writing";

        {
            std::ofstream file(staged, std::ios::binary | std::ios::trunc);
            if (!file) {
                ENGINE_LOG_ERROR("Could not open {} for writing.", staged.string());
                return false;
            }
            constexpr int kIndent = 2;
            file << document.dump(kIndent) << '\n';
            file.close();
            if (!file) {
                ENGINE_LOG_ERROR("Could not write {}, so {} is untouched.", staged.string(),
                                 path.string());
                std::error_code ignored;
                std::filesystem::remove(staged, ignored);
                return false;
            }
        }

        std::error_code error;
        std::filesystem::rename(staged, path, error);
        if (error) {
            ENGINE_LOG_ERROR("Could not put {} in place of {}. {}", staged.string(),
                             path.string(), error.message());
            std::error_code ignored;
            std::filesystem::remove(staged, ignored);
            return false;
        }

        ENGINE_LOG_INFO("Wrote {}, with {} asset references put back.", path.string(), restored);
        return true;
    }

    /**
     * Which way the camera looks, from its yaw and pitch.
     *
     * Yaw of zero looks down -Z, which DESIGN.md section 3 calls forward. Pitch
     * lifts from the horizon. The result is a unit vector.
     */
    engine::Vec3 camera_forward(const ViewSettings& settings) {
        const float yaw = glm::radians(settings.camera_yaw);
        const float pitch = glm::radians(settings.camera_pitch);
        const float flat = std::cos(pitch);
        return glm::normalize(engine::Vec3{ -flat * std::sin(yaw), std::sin(pitch),
                                            -flat * std::cos(yaw) });
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
    } // namespace action

    /**
     * Binds the runtime's own actions.
     *
     * These belong to the debug camera and to the throw, which are application
     * concerns rather than game logic. M8.6 moves the throw into a script, and
     * its binding moves with it.
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
        input.bind(action::kThrow, Key::F);
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

    /**
     * Builds the matrix that turns a world position into clip space.
     *
     * A model matrix is not part of this: each entity supplies its own, and
     * scene::World has already composed it. Reverse-Z means the near plane maps
     * to depth 1, and perspective_reverse_z already negates the Y row for
     * Vulkan clip space.
     */
    engine::Mat4 view_projection(const ViewSettings& settings, engine::gfx::Extent2D extent) {
        const float aspect = extent.height == 0
                                 ? 1.0F
                                 : static_cast<float>(extent.width) /
                                       static_cast<float>(extent.height);
        const engine::Mat4 projection = engine::perspective_reverse_z(
            glm::radians(settings.fov_degrees), aspect, engine::kDefaultNearPlane);

        const engine::Mat4 view = glm::lookAt(settings.camera_position,
                                              settings.camera_position + camera_forward(settings),
                                              engine::world_up);

        return projection * view;
    }

    /// Opens a window where it belongs, and lets the user move it for this run.
    bool begin_panel(const char* name, ImVec2 position, ImVec2 size) {
        ImGui::SetNextWindowPos(position, ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(size, ImGuiCond_FirstUseEver);
        return ImGui::Begin(name);
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

        if (begin_panel("View", { kPanelMargin, kPanelMargin },
                        { kPanelWidth, kViewHeight })) {
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

    /// What the label for one entity says in the tree.
    std::string entity_label(const entt::registry& entities, entt::entity entity) {
        const auto* named = entities.try_get<engine::scene::Name>(entity);
        std::string label = named != nullptr ? named->value : std::string{ "unnamed" };
        if (entities.all_of<engine::scene::PrefabInstance>(entity)) {
            label += "  [" + entities.get<engine::scene::PrefabInstance>(entity).prefab + "]";
        }
        return label;
    }

    /// Draws one entity and everything under it, and reports a click.
    void draw_entity_node(const engine::scene::World& world, entt::entity entity,
                          entt::entity& selected) {
        const entt::registry& entities = world.registry();
        const engine::scene::Hierarchy& node = entities.get<engine::scene::Hierarchy>(entity);

        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow |
                                   ImGuiTreeNodeFlags_SpanAvailWidth |
                                   ImGuiTreeNodeFlags_DefaultOpen;
        if (node.child_count == 0) {
            flags |= ImGuiTreeNodeFlags_Leaf;
        }
        if (entity == selected) {
            flags |= ImGuiTreeNodeFlags_Selected;
        }

        // The entity value is the identity here. Two entities can share a name,
        // and ImGui needs the labels to differ.
        ImGui::PushID(static_cast<int>(entt::to_integral(entity)));
        const bool open = ImGui::TreeNodeEx("node", flags, "%s",
                                            entity_label(entities, entity).c_str());
        // A press and a release inside one frame make ImGui hold the click for
        // two frames, so guard on a real change rather than logging twice.
        if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen() && selected != entity) {
            selected = entity;
            ENGINE_LOG_TRACE("Selected entity {}.", entt::to_integral(entity));
        }
        if (open) {
            for (entt::entity child = node.first_child; child != entt::null;
                 child = entities.get<engine::scene::Hierarchy>(child).next_sibling) {
                draw_entity_node(world, child, selected);
            }
            ImGui::TreePop();
        }
        ImGui::PopID();
    }

    /**
     * Draws the hierarchy, and lets the user pick an entity out of it.
     *
     * This window reads the world the sandbox loaded. It names no game type, so
     * a different game in the same runtime shows the same tree.
     */
    void draw_world_window(const engine::scene::World& world, entt::entity& selected,
                           const std::filesystem::path& scene_path,
                           const engine::assets::Content& content) {
        ENGINE_PROFILE_ZONE_N("draw_world_window");

        if (begin_panel("World", { kPanelMargin, (2 * kPanelMargin) + kViewHeight },
                        { kPanelWidth, kWorldHeight })) {
            ImGui::Text("Entities: %zu", world.size());
            ImGui::Text("Matrices rebuilt last frame: %zu", world.rebuilt_last_update());

            // Without a source tree there is nowhere to save that a person
            // would find again. Writing into the cooked tree looks like it
            // worked and the next cook throws it away.
            const bool can_save = !scene_path.empty();
            ImGui::BeginDisabled(!can_save);
            if (ImGui::Button("Save scene") && can_save) {
                // Every prefab instance collapses again here, so what the user
                // changed comes back as an override rather than as entities.
                if (!write_scene_source(scene_path, world, content)) {
                    ENGINE_LOG_ERROR("The scene did not write to {}.", scene_path.string());
                }
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            // The whole path, because which of the two trees this writes to is
            // the thing worth knowing.
            ImGui::TextDisabled("%s", can_save ? scene_path.string().c_str()
                                               : "no source tree, so there is nowhere to save");
            if (can_save && ImGui::IsItemHovered()) {
                ImGui::SetTooltip("The source scene. The cooker turns it into what runs.");
            }

            ImGui::Separator();

            const entt::registry& entities = world.registry();
            for (const auto [entity, node] :
                 entities.view<const engine::scene::Hierarchy>().each()) {
                if (node.parent == entt::null) {
                    draw_entity_node(world, entity, selected);
                }
            }
        }
        ImGui::End();
    }

    /**
     * Draws every component the selected entity carries.
     *
     * Nothing here names a component type. The registry holds a function that
     * already knows the type, and it calls reflect::inspect() through it. Add a
     * described component to the registry and it appears in this window, game
     * types included.
     */
    /**
     * Draws the material block layout that mesh.frag declares.
     *
     * The renderer validates these against the shader at startup, and a
     * person editing a material needs to see the names and the types the
     * shader expects. The values come from the cooked material asset, and
     * editing them needs a material source that does not exist yet. Issue
     * #102 records that decision.
     */
    void draw_material_block_info() {
        if (!ImGui::CollapsingHeader("Material block", ImGuiTreeNodeFlags_DefaultOpen)) {
            return;
        }

        ImGui::TextDisabled("What mesh.frag expects. The values come from the cooked glTF.");
        ImGui::Separator();

        if (!ImGui::BeginTable("material_params", 3,
                               ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
            return;
        }
        ImGui::TableSetupColumn("Name");
        ImGui::TableSetupColumn("Type");
        ImGui::TableSetupColumn("Offset");
        ImGui::TableHeadersRow();

        for (const engine::render::MaterialUniformMember& member :
             engine::render::material_uniform_layout()) {
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(member.name);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(engine::render::param_type_name(member.type));
            ImGui::TableNextColumn();
            ImGui::Text("%u", member.offset);
        }
        ImGui::EndTable();
    }

    void draw_inspector_window(engine::scene::World& world, entt::entity selected) {
        ENGINE_PROFILE_ZONE_N("draw_inspector_window");

        if (begin_panel("Inspector", { (2 * kPanelMargin) + kPanelWidth, kPanelMargin },
                        { kPanelWidth, kInspectorHeight })) {
            if (selected == entt::null || !world.registry().valid(selected)) {
                ImGui::TextDisabled("Pick an entity in the World window.");
                ImGui::End();
                return;
            }

            ImGui::Text("%s", entity_label(world.registry(), selected).c_str());
            ImGui::Separator();

            bool moved = false;
            for (const engine::scene::ComponentOps& ops : engine::scene::components().all()) {
                if (!ops.has(world.registry(), selected)) {
                    continue;
                }
                ImGui::PushID(ops.name);
                if (ImGui::CollapsingHeader(ops.name, ImGuiTreeNodeFlags_DefaultOpen)) {
                    moved = ops.inspect(world.registry(), selected) || moved;
                }
                ImGui::PopID();
            }

            const auto* renderer = world.registry().try_get<engine::scene::MeshRenderer>(selected);
            if (renderer != nullptr && renderer->mesh.valid()) {
                draw_material_block_info();
            }

            if (moved) {
                // The edit went through the registry, so it went around
                // set_local(). World says to mark the subtree, or the matrices
                // stay stale and only a later move would put them right.
                world.mark_dirty(selected);
            }
        }
        ImGui::End();
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
     * @param tonemap The pass that owns the scene color target.
     * @param states The carried resource states, which this resets for the
     * image it replaced.
     * @return True when the swapchain and the target now match @p extent.
     */
    bool rebuild_swapchain(
        engine::gfx::Device* device, engine::gfx::Extent2D extent,
        engine::render::TonemapPass& tonemap,
        std::array<engine::gfx::ResourceState, engine::render::kFrameResourceCount>& states) {
        const engine::gfx::Result result = engine::gfx::device_resize(device, extent);
        if (!engine::gfx::succeeded(result)) {
            ENGINE_LOG_CRITICAL("The swapchain did not rebuild: {}",
                                engine::gfx::result_name(result));
            return false;
        }

        // What the device settled on, which is not always what was asked for.
        // A surface can refuse a size, and the target has to match the frame
        // rather than the request.
        if (!tonemap.resize(device_extent(device))) {
            ENGINE_LOG_CRITICAL("The scene color target did not rebuild, so nothing can draw.");
            return false;
        }
        states[engine::render::kSceneColor.index] = engine::gfx::ResourceState::Undefined;
        return true;
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
        engine::render::MeshPass* mesh_pass = nullptr;
        engine::render::ShadowPass* shadow_pass = nullptr;
        /// Owns the scene color target and writes the frame out.
        engine::render::TonemapPass* tonemap_pass = nullptr;
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
        /// What state each graph resource is in. The shadow map carries its
        /// state across frames, so this outlives one frame.
        std::array<engine::gfx::ResourceState, engine::render::kFrameResourceCount>*
            resource_states = nullptr;
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

    /// Where each pass writes its pair of timestamps. A pass writes the first
    /// slot before it records and the second after, so the difference is what
    /// it cost. The cull pair is last because it was added last, and these
    /// number the slots rather than the order the passes run.
    constexpr std::uint32_t kShadowTimestamp = 0;
    constexpr std::uint32_t kMeshTimestamp = 2;
    constexpr std::uint32_t kTonemapTimestamp = 4;
    constexpr std::uint32_t kCullTimestamp = 6;
    /// How many slots the four pairs take. The pool holds far more.
    constexpr std::uint32_t kTimestampCount = 8;

    /// GPU time for the last frame, in nanoseconds, one entry for each pass in
    /// the order the constants above declare them.
    std::array<double, kTimestampCount / 2> g_gpu_pass_ns{};

    /// The physics wireframe of the current frame. Kept here rather than in the
    /// frame arena because it holds its memory between frames, so a frame with
    /// the toggle on allocates nothing after the first one. A frame with it off
    /// never touches this at all.
    std::vector<engine::physics::DebugLine> g_debug_lines;
    /// The GPU timestamp period, in nanoseconds per tick.
    float g_timestamp_period = 0.0F;
    /// True after the first frame's pool reset, so reads are valid.
    bool g_timestamps_ready = false;

    /// Resets the timestamp pool at the start of the frame and reads results
    /// from the previous one, once they are ready.
    void read_gpu_timestamps(engine::gfx::Device* device, engine::gfx::FrameInfo& info) {
        engine::gfx::cmd_reset_timestamps(info.commands);

        if (!g_timestamps_ready) {
            g_timestamps_ready = true;
            return;
        }

        if (g_timestamp_period == 0.0F) {
            g_timestamp_period = engine::gfx::timestamp_period(device);
        }
        if (g_timestamp_period <= 0.0F) {
            return;
        }

        std::array<std::uint64_t, kTimestampCount> ticks{};
        if (!engine::gfx::read_timestamps(device, 0, kTimestampCount, ticks.data())) {
            return;
        }

        // Each pass wrote a pair, and the difference is what it cost. The slot
        // constant gives the index as well as the pair, so a pass added later
        // cannot put the value in one place and read it from another.
        const double period = static_cast<double>(g_timestamp_period);
        const auto elapsed = [&ticks, period](std::uint32_t first) {
            return static_cast<double>(ticks[first + 1] - ticks[first]) * period;
        };
        g_gpu_pass_ns[kShadowTimestamp / 2] = elapsed(kShadowTimestamp);
        g_gpu_pass_ns[kMeshTimestamp / 2] = elapsed(kMeshTimestamp);
        g_gpu_pass_ns[kTonemapTimestamp / 2] = elapsed(kTonemapTimestamp);
        g_gpu_pass_ns[kCullTimestamp / 2] = elapsed(kCullTimestamp);
    }

    FrameOutcome draw_frame(const FrameContext& context, engine::gfx::Extent2D extent,
                            engine::gfx::Extent2D& out_extent) {
        engine::gfx::Device* device = context.device;
        ViewSettings& settings = *context.settings;
        engine::scene::World& world = *context.world;

        engine::gfx::FrameInfo info;
        engine::gfx::Result result = engine::gfx::begin_frame(device, &info);

        if (result == engine::gfx::Result::OutOfDate) {
            return rebuild_swapchain(device, extent, *context.tonemap_pass,
                                     *context.resource_states)
                       ? FrameOutcome::Skipped
                       : FrameOutcome::Failed;
        }
        if (!engine::gfx::succeeded(result)) {
            ENGINE_LOG_CRITICAL("begin_frame failed: {}", engine::gfx::result_name(result));
            return FrameOutcome::Failed;
        }

        read_gpu_timestamps(device, info);

        // The overlay opens after the frame does, so a skipped frame never
        // leaves an ImGui frame half open.
        if (context.overlay) {
            engine::gfx::imgui_new_frame();
            draw_view_window(settings);
            draw_world_window(world, *context.selected, context.source_scene,
                              *context.game_content);
            draw_inspector_window(world, *context.selected);
        }

        // An edit in the inspector went around set_local(), so the matrices are
        // stale until this runs. Doing it here rather than before the windows is
        // what keeps the frame the user sees current with the frame they edited.
        world.update();

        // The render graph, before anything opens a rendering scope. begin_frame
        // leaves the frame images in Undefined and this is what moves them,
        // because the graph is what knows which pass needs them and in what
        // state.
        //
        // Three passes now. Each declares what it touches, and the barriers that
        // move the shadow map and the scene color from a target to a shader read
        // fall out of the declarations rather than being written by hand here.
        engine::render::GraphSchedule schedule;
        if (!derive_frame_barriers(*context.resource_states, schedule)) {
            return FrameOutcome::Failed;
        }

        // Which image each resource is. The two frame targets stay null, because
        // an enum names those and the handle table is how the rest are found.
        GraphTextures textures{};
        textures[engine::render::kShadowMap.index] = context.shadow_pass->map();
        textures[engine::render::kSceneColor.index] = context.tonemap_pass->target();

        // The shadow pass first, because the mesh pass reads what it wrote. Its
        // barriers go in before it records, and its own rendering scope opens
        // and closes inside draw().
        const engine::Mat4 clip_from_world = view_projection(settings, info.extent);

        issue_pass_barriers(info.commands, schedule, kShadowPassIndex, textures);
        engine::gfx::cmd_write_timestamp(info.commands, kShadowTimestamp);
        context.shadow_pass->draw(info.commands, world, *context.game_content,
                                  context.mesh_pass->meshes(), clip_from_world);
        engine::gfx::cmd_write_timestamp(info.commands, kShadowTimestamp + 1);
        context.mesh_pass->set_shadow_view(context.shadow_pass->light_view_projections(),
                                           context.shadow_pass->cascade_splits(),
                                           context.shadow_pass->cascade_biases(),
                                           context.shadow_pass->has_light());

        // The cluster cull, which is a pass of its own. It runs here rather than
        // inside the mesh pass because a compute dispatch cannot happen inside a
        // rendering scope. Its barrier moves the cluster grid from the read the
        // last frame left it in to a compute write.
        issue_pass_barriers(info.commands, schedule, kCullPassIndex, textures);
        engine::gfx::cmd_write_timestamp(info.commands, kCullTimestamp);
        const engine::render::ClusterView cluster_view{
            .z_near = engine::kDefaultNearPlane,
            .viewport_width = static_cast<float>(info.extent.width),
            .viewport_height = static_cast<float>(info.extent.height),
        };
        context.mesh_pass->cull(info.commands, world, *context.game_content, clip_from_world,
                                settings.camera_position, cluster_view);
        engine::gfx::cmd_write_timestamp(info.commands, kCullTimestamp + 1);

        // Then the mesh pass barriers, which include moving the shadow map from
        // a depth target to something a shader can read, and the cluster grid
        // from the compute write to a fragment read.
        issue_pass_barriers(info.commands, schedule, kMeshPassIndex, textures);

        const engine::gfx::ColorRGBA clear{ settings.clear_color.r, settings.clear_color.g,
                                            settings.clear_color.b, 1.0F };
        // Into the half float scene image, not the swapchain. An 8-bit sRGB
        // image would clip every value above 1 as the fragment shader wrote it.
        //
        // A draw or an end outside a rendering scope is invalid, so nothing is
        // recorded when the scope did not open. The frame then reaches the
        // tonemap pass with a scene image nobody drew into, which is a black
        // picture rather than undefined behavior.
        if (engine::gfx::cmd_begin_color_rendering(info.commands, context.tonemap_pass->target(),
                                                   clear)) {
            // Every entity that names a mesh draws it, and that is now every
            // entity that draws at all. This is the pipeline made visible: the
            // geometry comes from a cooked file that a glTF produced, and
            // nothing here knows which file that was.
            engine::gfx::cmd_write_timestamp(info.commands, kMeshTimestamp);
            context.mesh_pass->draw(info.commands, world, *context.game_content,
                                    settings.camera_position);
            engine::gfx::cmd_write_timestamp(info.commands, kMeshTimestamp + 1);

            engine::gfx::cmd_end_rendering(info.commands);
        }

        // Then the frame itself. The tonemap pass reads the scene image and
        // writes the swapchain, and the barrier that moves the scene image to a
        // shader read is the one the graph derived from the pair.
        issue_pass_barriers(info.commands, schedule, kTonemapPassIndex, textures);

        // Black, because the full-screen triangle covers every pixel. The clear
        // color a person picked belongs to the scene image above. The pass
        // attaches no depth, because the triangle neither reads nor writes it.
        constexpr engine::gfx::ColorRGBA kFrameClear{ 0.0F, 0.0F, 0.0F, 1.0F };
        engine::gfx::cmd_begin_rendering(info.commands, kFrameClear, false);
        engine::gfx::cmd_write_timestamp(info.commands, kTonemapTimestamp);
        context.tonemap_pass->draw(info.commands, settings.exposure);
        engine::gfx::cmd_write_timestamp(info.commands, kTonemapTimestamp + 1);

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
            if (!rebuild_swapchain(device, extent, *context.tonemap_pass,
                                   *context.resource_states)) {
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
        engine::render::MeshPass mesh;
        /// Renders the directional light's depth, which the mesh pass samples.
        engine::render::ShadowPass shadow;
        /// Owns the half float image the scene renders into, and writes it out.
        engine::render::TonemapPass tonemap;
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
        /// Carried across frames, because the shadow map and the scene color are
        /// each one image that every frame in flight shares.
        std::array<engine::gfx::ResourceState, engine::render::kFrameResourceCount> states{};
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
#if defined(ENGINE_WITH_LUA)
        /// M8.1. The interpreter, and one instance for each scripted entity.
        engine::script::Host script;
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
                (void)runtime.mesh.reload_shaders(runtime.engine_content);
                (void)runtime.shadow.reload_shaders(runtime.engine_content);
                (void)runtime.tonemap.reload_shaders(runtime.engine_content);
            }
            if (had_brdf) {
                (void)runtime.mesh.reload_brdf_lut(runtime.engine_content);
            }
        }

        if (!runtime.reload.poll(runtime.game_content, changed)) {
            return;
        }

        runtime.mesh.reload(identities_of(changed));
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

        // The shadow pass first. The mesh pass binds its map into every frame
        // descriptor set, so the map has to exist before those sets are built.
        if (!runtime.shadow.create(runtime.device, runtime.engine_content)) {
            return false;
        }

        if (!runtime.mesh.create(runtime.device, runtime.engine_content, runtime.shadow.map())) {
            return false;
        }

        // Before the first cull, so the grid is allocated once at the size this
        // asks for rather than at the default and then again.
        runtime.mesh.set_cluster_cell_ceiling(options.cluster_cell_lights);

#if defined(ENGINE_WITH_UI)
        // M6.2. Built after the engine content tree is read, because the
        // pipelines come from the cooked ui shaders in it. A failure here is
        // not fatal: UiPass::create clears its device on the way out, so the
        // pass reports itself not ready and draws nothing.
        if (!runtime.ui_pass.create(runtime.device, runtime.engine_content)) {
            ENGINE_LOG_ERROR("The UI pass did not build. Game UI will not draw.");
        }
#endif

        // After the device, because the target is the size the swapchain
        // settled on rather than the size that was asked for.
        if (!runtime.tonemap.create(runtime.device, runtime.engine_content,
                                    device_extent(runtime.device))) {
            return false;
        }

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

        result = engine::gfx::imgui_init(runtime.device, runtime.window.native());
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
        // Before the device goes. MeshPass frees buffers, textures, and a
        // pipeline through the device, and its destructor runs when Runtime
        // goes out of scope, which is after this function returns.
        runtime.mesh.destroy();
        runtime.shadow.destroy();
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
        runtime.tonemap.destroy();
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

    void report_frame_time(const engine::FrameStats& stats, const Options& options) {
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
        if (std::ranges::any_of(g_gpu_pass_ns, [](double ns) { return ns > 0.0; })) {
            // In the order they run, which is not the order the slots number.
            // The cull is what says whether the cluster grid pays for itself.
            ENGINE_LOG_INFO("gpu passes | shadow {:.3f} ms | cull {:.3f} ms | mesh {:.3f} ms | "
                            "tonemap {:.3f} ms",
                            g_gpu_pass_ns[kShadowTimestamp / 2] * kToMilliseconds,
                            g_gpu_pass_ns[kCullTimestamp / 2] * kToMilliseconds,
                            g_gpu_pass_ns[kMeshTimestamp / 2] * kToMilliseconds,
                            g_gpu_pass_ns[kTonemapTimestamp / 2] * kToMilliseconds);
        }

        if (options.vsync) {
            ENGINE_LOG_INFO("Vsync is on, so that is the refresh rate. Use --no-vsync to measure "
                            "a change.");
        }
    }

    /**
     * Throws a crate from the camera, along the way it is looking.
     *
     * The projectile is an instance of the same prefab the scene stacks, so
     * nothing new is authored and the thing thrown is the thing being hit. The
     * two physics components are added by the instance record, which
     * merge_patch allows because a prefab that does not carry a component gains
     * it rather than being refused.
     *
     * @param world The world to add the crate to.
     * @param simulation The bodies. The new one joins without rebuilding them.
     * @param settings Where the camera is and which way it faces.
     * @return The new entity, or entt::null when the prefab is not loaded.
     */
    entt::entity throw_crate(engine::scene::World& world, engine::physics::Simulation& simulation,
                             const ViewSettings& settings) {
        const engine::scene::Prefab* prefab = engine::scene::prefabs().find(sandbox::kCratePrefab);
        if (prefab == nullptr) {
            ENGINE_LOG_WARN("There is no {} to throw. The content tree did not load it.",
                            sandbox::kCratePrefab);
            return entt::null;
        }

        const engine::Vec3 forward = camera_forward(settings);
        // In front of the camera rather than at it. A body created inside the
        // near plane is a crate that fills the screen for one frame.
        const engine::Vec3 from = settings.camera_position + (forward * kThrowOffset);

        // An empty record, and then the components are placed as components.
        // Writing them as an override patch would spell out the field names and
        // the schema versions that reflect/ already owns, which is the second
        // descriptor system rule 4.5 forbids. It would also go stale in silence
        // the day a field is renamed.
        const entt::entity crate =
            engine::scene::instantiate(world, *prefab, nlohmann::json::object());
        if (crate == entt::null) {
            ENGINE_LOG_ERROR("The thrown crate did not instance.");
            return entt::null;
        }

        entt::registry& registry = world.registry();
        registry.emplace_or_replace<engine::scene::Name>(crate,
                                                         engine::scene::Name{ "thrown crate" });
        registry.emplace_or_replace<engine::physics::RigidBody>(
            crate, engine::physics::RigidBody{ .type = engine::physics::BodyType::Dynamic });
        registry.emplace_or_replace<engine::physics::BoxCollider>(
            crate, engine::physics::BoxCollider{ .half_extents = { kCrateHalfExtent,
                                                                   kCrateHalfExtent,
                                                                   kCrateHalfExtent } });

        // The prefab put the crate at the origin, so this is where it is thrown
        // from. set_local rather than the transform component, because the
        // hierarchy has to know the world matrix went stale.
        engine::Transform local = world.local(crate);
        local.position = from;
        world.set_local(crate, local);

        // add_body rather than build. Rebuilding would put the stack back where
        // the scene file put it, which is the opposite of hitting it.
        if (!simulation.add_body(world, crate)) {
            ENGINE_LOG_ERROR("The thrown crate got no body, so it will not fall.");
            return crate;
        }
        (void)simulation.set_linear_velocity(crate, forward * kThrowSpeed);
        return crate;
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

    /**
     * Runs the game logic for one fixed step.
     *
     * The C++ game and the scripts are one thing from here, and both take
     * simulated seconds. Neither one sees the wall clock, which is what keeps a
     * run reproducible. See DESIGN.md section 9 and issue #245.
     *
     * @param runtime Holds the script host.
     * @param world The scene to run.
     * @param state The simulated seconds and the poses a frame blends.
     */
    void run_game_step(Runtime& runtime, engine::scene::World& world, StepState& state,
                       engine::physics::Simulation& simulation) {
        sandbox::update(world, state.seconds, state.motion);
#if defined(ENGINE_WITH_LUA)
        // Passed on each step rather than held, so a reload that builds a new
        // simulation cannot leave a script driving the old one. See issue #273.
        runtime.script.update(world, state.seconds,
                              engine::script::Services{
                                  .physics = &simulation,
                                  .input = &runtime.input,
                                  .prefabs = &engine::scene::prefabs(),
                              });
#else
        (void)runtime;
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
            run_game_step(runtime, world, state, simulation);
            simulation.step(world, state.clock.step_seconds());
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

        if (!rebuild_swapchain(runtime.device, extent, runtime.tonemap, runtime.states)) {
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
    void update_input(Runtime& runtime, const FrameContext& context, const Options& options) {
        if (options.offscreen) {
            runtime.input.update(engine::platform::InputFrame{});
            return;
        }

        engine::platform::InputConsumed consumed;
        if (context.overlay) {
            engine::gfx::imgui_wants_input(&consumed.mouse, &consumed.keyboard);
        }
        runtime.input.update(engine::platform::sample(runtime.window, consumed));
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

            update_input(runtime, context, options);
            update_camera(runtime.window, runtime.input, settings, delta);

            // The key edge rather than the key being down, or holding it would
            // fill the room with crates in one second.
            const bool throw_now = runtime.input.pressed(action::kThrow);
            const bool throw_this_frame =
                options.throw_at_frame != 0 && frame + 1 == options.throw_at_frame;
            if (throw_now || throw_this_frame) {
                (void)throw_crate(world, simulation, settings);
            }

            // The game and the solver both run on the fixed step now, so this
            // is one call rather than two. The frame composes the matrices and
            // draws after it. Reversing those two would draw a frame behind.
            advance_simulation(runtime, step, settings, simulation, world, delta, physics_stats);

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
        report_scene_counts(*context.mesh_pass, *context.shadow_pass);
        // After the frame time, so the physics cost reads beside the per-pass
        // GPU split that report_frame_time prints. The two are different
        // domains and the labels say so: the solver runs on the CPU.
        report_frame_time(stats, options);
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
        .mesh_pass = &runtime.mesh,
        .shadow_pass = &runtime.shadow,
        .tonemap_pass = &runtime.tonemap,
#if defined(ENGINE_WITH_UI)
        .ui_pass = &runtime.ui_pass,
        .ui_renderer = &runtime.ui_renderer,
        .ui_image = runtime.ui_image.get(),
        .ui_font = runtime.ui_font.get(),
        .ui_layout = runtime.ui_layout.get(),
#endif
        .overlay = runtime.overlay,
        .resource_states = &runtime.states,
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
