#include "assets/hot_reload.h"
#include "assets/manifest.h"
#include "assets/reference.h"
#include "core/arena.h"
#include "core/frame_stats.h"
#include "core/jobs.h"
#include "core/log.h"
#include "core/profile.h"
#include "core/version.h"
#include "gfx/device.h"
#include "gfx/imgui.h"
#include "math/conventions.h"
#include "platform/paths.h"
#include "platform/window.h"
#include "reflect/inspector.h"
#include "reflect/json.h"
#include "reflect/registry.h"
#include "render/mesh_pass.h"
#include "render/shadow_pass.h"
#include "render/tonemap_pass.h"
#include "screenshot.h"
#include "sandbox/game.h"
#include "scene/component_registry.h"
#include "scene/prefab.h"
#include "scene/scene_file.h"
#include "scene/components.h"
#include "scene/world.h"

#include <SDL3/SDL.h>
#include <imgui.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

    constexpr std::size_t kFrameArenaBytes = 4U * 1024U * 1024U;
    constexpr int kDecimalBase = 10;
    /// About one frame at 60 Hz. Long enough to idle, short enough to wake fast.
    constexpr int kMinimizedSleepMs = 16;

    constexpr float kNearPlane = 0.1F;

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
        /// The shader compiler to cook with. Empty means the compiled-in default.
        std::string glslc;
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
     * Reads the exposure.
     *
     * `std::from_chars` for a float needs a whole value and no trailing text,
     * which is what refuses "1.0x" and "abc".
     *
     * It does parse "inf" and "nan", so those have to be refused by value
     * rather than by a parse failure. Both reach the shader as a scale that
     * makes the curve produce something no display can show: infinity divided
     * by infinity is not a number, and a frame of those is undefined rather
     * than black. `std::isfinite` is what rejects the pair, and the comparison
     * against zero alone would let infinity through.
     *
     * @param text The value given on the command line.
     * @param out Receives the exposure. Untouched unless the whole value parsed
     * and came out finite and above zero.
     */
    void parse_exposure(std::string_view text, float& out) {
        const char* first = text.data();
        const char* last = text.data() + text.size();
        float value = 0.0F;
        const std::from_chars_result parsed = std::from_chars(first, last, value);
        const bool usable = parsed.ec == std::errc{} && parsed.ptr == last &&
                            std::isfinite(value) && value > 0.0F;
        if (!usable) {
            ENGINE_LOG_WARN("--exposure wants a finite number above zero, so {} was ignored.",
                            text);
            return;
        }
        out = value;
    }

    /// Which pass in the schedule is which. The order here is the order they run.
    constexpr std::size_t kShadowPassIndex = 0;
    constexpr std::size_t kMeshPassIndex = 1;
    constexpr std::size_t kTonemapPassIndex = 2;

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
                                 engine::render::MeshPass::declare(),
                                 engine::render::TonemapPass::declare() };

        // The two frame targets start over every frame. The swapchain image is a
        // different image on almost every acquire, and the depth image is
        // scratch that nothing reads across a frame boundary.
        states[engine::render::kFrameColor.index] = engine::gfx::ResourceState::Undefined;
        states[engine::render::kFrameDepth.index] = engine::gfx::ResourceState::Undefined;
        // The shadow map and the scene color do not. Each is one image that
        // every frame in flight shares, so the state the last frame left it in
        // is what the barrier has to order against. Calling either Undefined
        // here would derive a barrier that waits on the stage the new state
        // uses, and what it has to wait for is the previous frame's fragment
        // shader reading the image. That is a write after read, and it is the
        // same hazard #125 found on the shared depth image.

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
            // ReadOnly keeps the editor from changing it. Transient keeps it out
            // of the file. The two attributes are read by different consumers,
            // and neither consumer knows about the other.
            ENGINE_FIELD(ViewSettings, frames_drawn, ReadOnly{}, Transient{}, Category{ "Debug" }));
    }
};
// NOLINTEND(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)

namespace {

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
            if (arg == "--frames" && has_value) {
                options.max_frames = std::strtoull(argv[i + 1], nullptr, kDecimalBase);
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
            } else if (arg == "--glslc" && has_value) {
                options.glslc = argv[i + 1];
                ++i;
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
            } else if (arg == "--resolution" && has_value) {
                parse_resolution(argv[i + 1], options.resolution);
                ++i;
            } else if (arg == "--exposure" && has_value) {
                parse_exposure(argv[i + 1], options.exposure);
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

    /**
     * Moves and turns the camera from the keyboard and the mouse.
     *
     * The right mouse button holds the look. While it is down the pointer is
     * captured, so a drag never runs out of screen. ImGui gets first refusal on
     * both devices, so typing in a field does not fly the camera away.
     */
    void update_camera(ViewSettings& settings, float delta_seconds) {
        bool imgui_mouse = false;
        bool imgui_keyboard = false;
        engine::gfx::imgui_wants_input(&imgui_mouse, &imgui_keyboard);

        float mouse_x = 0.0F;
        float mouse_y = 0.0F;
        const SDL_MouseButtonFlags buttons = SDL_GetRelativeMouseState(&mouse_x, &mouse_y);
        const bool looking =
            !imgui_mouse && (buttons & SDL_BUTTON_RMASK) != 0;

        // The pointer stays put while the look is held, so the drag has no edge.
        SDL_Window* focus = SDL_GetKeyboardFocus();
        if (focus != nullptr) {
            SDL_SetWindowRelativeMouseMode(focus, looking);
        }

        if (looking) {
            settings.camera_yaw -= mouse_x * settings.look_sensitivity;
            settings.camera_pitch -= mouse_y * settings.look_sensitivity;
            // Straight up would make the forward vector and world up parallel,
            // and lookAt has no basis to build from a pair like that.
            settings.camera_pitch = std::clamp(settings.camera_pitch, kLowestPitch, kHighestPitch);
            settings.camera_yaw = std::remainder(settings.camera_yaw, kFullTurnDegrees);
        }

        if (imgui_keyboard) {
            return;
        }

        const bool* keys = SDL_GetKeyboardState(nullptr);
        if (keys == nullptr) {
            return;
        }

        const engine::Vec3 forward = camera_forward(settings);
        const engine::Vec3 right = glm::normalize(glm::cross(forward, engine::world_up));

        engine::Vec3 wanted{ 0.0F, 0.0F, 0.0F };
        if (keys[SDL_SCANCODE_W]) {
            wanted += forward;
        }
        if (keys[SDL_SCANCODE_S]) {
            wanted -= forward;
        }
        if (keys[SDL_SCANCODE_D]) {
            wanted += right;
        }
        if (keys[SDL_SCANCODE_A]) {
            wanted -= right;
        }
        if (keys[SDL_SCANCODE_E]) {
            wanted += engine::world_up;
        }
        if (keys[SDL_SCANCODE_Q]) {
            wanted -= engine::world_up;
        }

        if (glm::length(wanted) < kShortestMove) {
            return;
        }

        const float speed =
            settings.move_speed * ((keys[SDL_SCANCODE_LSHIFT] || keys[SDL_SCANCODE_RSHIFT])
                                       ? kSprintFactor
                                       : 1.0F);
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
            glm::radians(settings.fov_degrees), aspect, kNearPlane);

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

    /// What draw_frame() needs that does not change from one frame to the next.
    struct FrameContext {
        engine::gfx::Device* device = nullptr;
        engine::render::MeshPass* mesh_pass = nullptr;
        engine::render::ShadowPass* shadow_pass = nullptr;
        /// Owns the scene color target and writes the frame out.
        engine::render::TonemapPass* tonemap_pass = nullptr;
        /// False when there is no window, so no ImGui and no input.
        bool overlay = false;
        /// What state each graph resource is in. The shadow map carries its
        /// state across frames, so this outlives one frame.
        std::array<engine::gfx::ResourceState, engine::render::kFrameResourceCount>*
            resource_states = nullptr;
        /// The game content tree, which holds the cooked meshes.
        const engine::assets::Content* game_content = nullptr;
        ViewSettings* settings = nullptr;
        engine::scene::World* world = nullptr;
        /// The entity the inspector edits, or entt::null for none.
        entt::entity* selected = nullptr;
        /// The cooked game content directory, which holds the scene and the prefabs.
        std::filesystem::path content;
        /// The scene a person edits, or empty when no source tree is there.
        std::filesystem::path source_scene;
    };

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
        context.shadow_pass->draw(info.commands, world, *context.game_content,
                                  context.mesh_pass->meshes(), clip_from_world);
        context.mesh_pass->set_shadow_view(context.shadow_pass->light_view_projections(),
                                           context.shadow_pass->cascade_splits(),
                                           context.shadow_pass->cascade_biases(),
                                           context.shadow_pass->has_light());

        // Then the mesh pass barriers, which include moving the shadow map from
        // a depth target to something a shader can read.
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
            context.mesh_pass->draw(info.commands, world, *context.game_content, clip_from_world,
                                    settings.camera_position);

            engine::gfx::cmd_end_rendering(info.commands);
        }

        // Then the frame itself. The tonemap pass reads the scene image and
        // writes the swapchain, and the barrier that moves the scene image to a
        // shader read is the one the graph derived from the pair.
        issue_pass_barriers(info.commands, schedule, kTonemapPassIndex, textures);

        // Black, because the full screen triangle covers every pixel. The clear
        // color a person picked belongs to the scene image above.
        constexpr engine::gfx::ColorRGBA kFrameClear{ 0.0F, 0.0F, 0.0F, 1.0F };
        engine::gfx::cmd_begin_rendering(info.commands, kFrameClear);
        context.tonemap_pass->draw(info.commands, settings.exposure);

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
        /// Carried across frames, because the shadow map and the scene color are
        /// each one image that every frame in flight shares.
        std::array<engine::gfx::ResourceState, engine::render::kFrameResourceCount> states{};
        /// The game's cooked assets, which today means the meshes a scene names.
        engine::assets::Content game_content;
        /// M4.5. Watches the game source tree and cooks what a person edits.
        engine::assets::HotReload reload;
        /// M4.5. The same for the engine tree, which holds the shaders and the table.
        engine::assets::HotReload engine_reload;
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
    void start_hot_reload(Runtime& runtime, const Options& options) {
        if (!options.hot_reload) {
            ENGINE_LOG_INFO("Hot reload is off, because --no-watch was given.");
            return;
        }

        const std::string glslc =
            options.glslc.empty() ? std::string{ ENGINE_GLSLC_PATH } : options.glslc;

        const engine::assets::HotReloadDesc desc{
            .source = options.watch.empty() ? std::filesystem::path{ ENGINE_GAME_CONTENT_SOURCE }
                                            : std::filesystem::path{ options.watch },
            .cooker = cooker_path(),
            .glslc = glslc,
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
            .glslc = glslc,
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
                          engine::scene::World& world) {
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

        // Every entity goes, so anything holding one lets go first.
        *context.selected = entt::null;
        world.clear();
        engine::scene::prefabs().clear();

        if (!sandbox::load(context.content, &runtime.game_content, world)) {
            // An empty world is what a broken scene looks like, and the log
            // above says which file. Saving a working one loads it again, so
            // this never ends the process.
            ENGINE_LOG_ERROR("The scene did not load, so the world is empty. Fix the file "
                             "and save it again.");
            return;
        }
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

        // After the device, because the target is the size the swapchain
        // settled on rather than the size that was asked for.
        if (!runtime.tonemap.create(runtime.device, runtime.engine_content,
                                    device_extent(runtime.device))) {
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
        runtime.tonemap.destroy();
        if (runtime.device != nullptr) {
            engine::gfx::destroy_device(runtime.device);
        }
        runtime.window.destroy();
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
    void report_frame_time(const engine::FrameStats& stats, const Options& options,
                           const engine::render::MeshPass& mesh) {
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

        // The light counts of the last frame. Reported because a cull that does
        // nothing changes no pixel, so the picture cannot say whether it ran.
        ENGINE_LOG_INFO("lights | {} lit the last frame | {} culled by the frustum | buffer holds {}",
                        mesh.visible_light_count(), mesh.culled_light_count(),
                        mesh.light_capacity());

        if (options.vsync) {
            ENGINE_LOG_INFO("Vsync is on, so that is the refresh rate. Use --no-vsync to measure "
                            "a change.");
        }
    }

    /// Runs frames until the user quits, the frame limit lands, or a frame fails.
    bool run_frames(Runtime& runtime, const FrameContext& context, const Options& options,
                    engine::Arena& frame_arena, ViewSettings& settings,
                    engine::scene::World& world) {
        std::uint64_t frame = 0;
        auto started = std::chrono::steady_clock::now();
        auto last_report = started;
        auto last_frame = started;
        // Offscreen the size is fixed for the whole run, which is the point of
        // it. Nothing resizes, so nothing rebuilds.
        engine::gfx::Extent2D last_extent =
            options.offscreen ? device_extent(runtime.device) : window_extent(runtime.window);

        engine::FrameStats stats(kFrameStatsWarmup);
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

            if (!options.offscreen && runtime.window.minimized()) {
                // poll() does not block, so without this the loop pins one core
                // while the window is minimized and there is nothing to draw.
                std::this_thread::sleep_for(std::chrono::milliseconds(kMinimizedSleepMs));
                // No frame ran, so the next delta must not count the idle time.
                last_frame = std::chrono::steady_clock::now();
                have_drawn = false;
                continue;
            }

            // Between frames, because freeing a resource waits for the frames
            // in flight and a frame cannot wait for itself.
            apply_hot_reload(runtime, context, world);

            // The window reports its new size before the swapchain knows about it.
            const engine::gfx::Extent2D extent =
                options.offscreen ? last_extent : window_extent(runtime.window);
            if (extent.width != last_extent.width || extent.height != last_extent.height) {
                if (!rebuild_swapchain(runtime.device, extent, runtime.tonemap,
                                       runtime.states)) {
                    return false;
                }
                last_extent = extent;
            }

            const auto now = std::chrono::steady_clock::now();
            // Offscreen counts frames rather than reading the clock, so the same
            // command produces the same image. See kOffscreenStep.
            const float seconds = options.offscreen
                                      ? static_cast<float>(frame) * kOffscreenStep
                                      : std::chrono::duration<float>(now - started).count();
            // A long stall, a debugger break, or a driver hitch would otherwise
            // multiply move_speed by the whole gap and throw the camera across
            // the scene in one step.
            const float delta =
                options.offscreen ? kOffscreenStep
                                  : std::min(std::chrono::duration<float>(now - last_frame).count(),
                                             kLongestFrame);
            last_frame = now;

            update_camera(settings, delta);

            // The game moves things, then the frame composes the matrices and
            // draws them. Reversing those two would draw a frame behind.
            sandbox::update(world, seconds);

            engine::gfx::Extent2D drawn_extent{};
            const FrameOutcome outcome = draw_frame(context, extent, drawn_extent);
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

            if (options.max_frames != 0 && frame >= options.max_frames) {
                ENGINE_LOG_INFO("Frame limit of {} reached. Exiting.", options.max_frames);
                // The frame just presented is the one to capture, and the loop
                // has not started another. So this is the only place a capture
                // is certain of what it will read.
                if (!options.screenshot.empty()) {
                    (void)runtime::write_screenshot(runtime.device, options.screenshot);
                }
                break;
            }

            ENGINE_PROFILE_FRAME();
        }

        ENGINE_LOG_INFO("Camina Engine stopped after {} frames.", frame);
        report_frame_time(stats, options, *context.mesh_pass);
        return true;
    }

} // namespace

int main(int argc, char** argv) {
    const Options options = parse_options(argc, argv);

    engine::log::init();
    ENGINE_LOG_INFO("Camina Engine {} starting.", engine::Version);

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
    if (options.exposure > 0.0F) {
        settings.exposure = options.exposure;
    }

    // The engine registers what it defines, then the game registers what it
    // defines. A scene loaded before this loses every component nobody claimed.
    engine::scene::register_builtin_components();
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

    entt::entity selected = entt::null;
    const FrameContext context{
        .device = runtime.device,
        .mesh_pass = &runtime.mesh,
        .shadow_pass = &runtime.shadow,
        .tonemap_pass = &runtime.tonemap,
        .overlay = runtime.overlay,
        .resource_states = &runtime.states,
        .game_content = &runtime.game_content,
        .settings = &settings,
        .world = &world,
        .selected = &selected,
        .content = content,
        .source_scene = source.empty() ? std::filesystem::path{} : source / sandbox::kSceneFile,
    };

    const bool ok = run_frames(runtime, context, options, frame_arena, settings, world);

    stop(runtime);
    engine::jobs::shutdown();
    engine::log::shutdown();
    return ok ? 0 : 1;
}
