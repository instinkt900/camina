#include "assets/hot_reload.h"
#include "assets/manifest.h"
#include "core/arena.h"
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
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
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

    /// Where each window opens. The overlay writes no imgui.ini, so a run
    /// always starts from this layout and a move lasts until the program ends.
    /// Without these all three open at the same place, and the last one drawn
    /// buries the rest. M8 gives the editor a real settings path.
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
        /// Where the game reads its content. Empty means the compiled-in default.
        std::string content;
        /// The source content tree to watch. Empty means the compiled-in default.
        std::string watch;
        /// The shader compiler to cook with. Empty means the compiled-in default.
        std::string glslc;
        bool hot_reload = true; ///< False turns the watcher and the cooker off.
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

        /// Where the camera stands. Saved, so a run opens where the last one stopped.
        engine::Vec3 camera_position{ 0.0F, 2.5F, 8.0F };
        /// Degrees around +Y. Zero looks down -Z, which is forward per DESIGN.md section 3.
        float camera_yaw = 0.0F;
        /// Degrees up from the horizon. Clamped, so the camera never rolls over the top.
        float camera_pitch = -8.0F;

        float fov_degrees = 60.0F;
        float move_speed = 6.0F;
        float look_sensitivity = 0.12F;

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
            }
        }
        return options;
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
                           const std::filesystem::path& scene_path) {
        ENGINE_PROFILE_ZONE_N("draw_world_window");

        if (begin_panel("World", { kPanelMargin, (2 * kPanelMargin) + kViewHeight },
                        { kPanelWidth, kWorldHeight })) {
            ImGui::Text("Entities: %zu", world.size());
            ImGui::Text("Matrices rebuilt last frame: %zu", world.rebuilt_last_update());

            if (ImGui::Button("Save scene")) {
                // Every prefab instance collapses again here, so what the user
                // changed comes back as an override rather than as entities.
                if (engine::scene::save_scene_file(scene_path, world)) {
                    ENGINE_LOG_INFO("Wrote {}.", scene_path.string());
                } else {
                    ENGINE_LOG_ERROR("The scene did not write to {}.", scene_path.string());
                }
            }
            ImGui::SameLine();
            ImGui::TextDisabled("%s", scene_path.filename().string().c_str());

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
        engine::render::MeshPass* mesh_pass = nullptr;
        /// The game content tree, which holds the cooked meshes.
        const engine::assets::Content* game_content = nullptr;
        ViewSettings* settings = nullptr;
        engine::scene::World* world = nullptr;
        /// The entity the inspector edits, or entt::null for none.
        entt::entity* selected = nullptr;
        /// The cooked game content directory, which holds the scene and the prefabs.
        std::filesystem::path content;
    };

    FrameOutcome draw_frame(const FrameContext& context, engine::gfx::Extent2D extent,
                            engine::gfx::Extent2D& out_extent) {
        engine::gfx::Device* device = context.device;
        ViewSettings& settings = *context.settings;
        engine::scene::World& world = *context.world;

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
        draw_world_window(world, *context.selected, context.content / sandbox::kSceneFile);
        draw_inspector_window(world, *context.selected);

        // An edit in the inspector went around set_local(), so the matrices are
        // stale until this runs. Doing it here rather than before the windows is
        // what keeps the frame the user sees current with the frame they edited.
        world.update();

        const engine::gfx::ColorRGBA clear{ settings.clear_color.r, settings.clear_color.g,
                                            settings.clear_color.b, 1.0F };
        engine::gfx::cmd_begin_rendering(info.commands, clear);

        const engine::Mat4 clip_from_world = view_projection(settings, info.extent);

        // Every entity that names a mesh draws it, and that is now every
        // entity that draws at all. This is the pipeline made visible: the
        // geometry comes from a cooked file that a glTF produced, and nothing
        // here knows which file that was.
        context.mesh_pass->draw(info.commands, world, *context.game_content, clip_from_world);

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

    /**
     * Everything the runtime owns for the whole run, in the order it is built.
     *
     * One struct rather than a ladder of locals, so shutdown is one call and
     * every early exit takes the same path.
     */
    struct Runtime {
        engine::platform::Window window;
        engine::gfx::Device* device = nullptr;
        /// The engine's own cooked assets, which today means the two shaders.
        engine::assets::Content engine_content;
        engine::render::MeshPass mesh;
        /// The game's cooked assets, which today means the meshes a scene names.
        engine::assets::Content game_content;
        /// M4.5. Watches the game source tree and cooks what a person edits.
        engine::assets::HotReload reload;
        /// M4.5. The same for the engine tree, which holds the two shaders.
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
     * An identity that went away is not in the manifest any more, and nothing
     * here can tell what it used to be. Deleting a prefab therefore needs a
     * restart, and issue #59 covers that.
     */
    bool world_was_built_from(const engine::assets::Content& content,
                              const std::vector<engine::Guid>& changed) {
        for (const engine::Guid guid : changed) {
            const engine::assets::ManifestOutput* output =
                engine::assets::find_by_guid(content.manifest(), guid);
            if (output == nullptr) {
                continue;
            }
            const std::filesystem::path cooked{ output->cooked };
            if (cooked.extension() == ".scene" || cooked.extension() == ".prefab") {
                return true;
            }
        }
        return false;
    }

    /**
     * Cooks whatever changed and swaps it in.
     *
     * Call this between frames. MeshPass::reload() waits for the frames in
     * flight before it frees anything, which cannot happen inside one.
     */
    void apply_hot_reload(Runtime& runtime, const FrameContext& context,
                          engine::scene::World& world) {
        std::vector<engine::Guid> changed;

        // The engine tree holds the two shaders and nothing else, so any change
        // to it means the pipeline. Adding a third asset to that tree would
        // make this rebuild on a change that does not need it, which costs a
        // stall and nothing else.
        if (runtime.engine_reload.poll(runtime.engine_content, changed)) {
            (void)runtime.mesh.reload_shaders(runtime.engine_content);
        }

        if (!runtime.reload.poll(runtime.game_content, changed)) {
            return;
        }

        runtime.mesh.reload(changed);
        if (!world_was_built_from(runtime.game_content, changed)) {
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

        if (!runtime.window.create({ .title = "Camina Engine (M4 sandbox)" })) {
            return false;
        }

        const engine::gfx::DeviceDesc device_desc{
            .window = runtime.window.native(),
            .app_name = "camina",
            .enable_validation = options.validation,
            .vsync = true,
        };
        engine::gfx::Result result = engine::gfx::create_device(device_desc, &runtime.device);
        if (!engine::gfx::succeeded(result)) {
            ENGINE_LOG_CRITICAL("The renderer did not start: {}",
                                engine::gfx::result_name(result));
            return false;
        }

        if (!runtime.mesh.create(runtime.device, runtime.engine_content)) {
            return false;
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
        if (runtime.device != nullptr) {
            engine::gfx::destroy_device(runtime.device);
        }
        runtime.window.destroy();
    }

    /// Runs frames until the user quits, the frame limit lands, or a frame fails.
    bool run_frames(Runtime& runtime, const FrameContext& context, const Options& options,
                    engine::Arena& frame_arena, ViewSettings& settings,
                    engine::scene::World& world) {
        std::uint64_t frame = 0;
        auto started = std::chrono::steady_clock::now();
        auto last_report = started;
        auto last_frame = started;
        engine::gfx::Extent2D last_extent = window_extent(runtime.window);

        while (runtime.window.poll()) {
            ENGINE_PROFILE_ZONE_N("frame");
            frame_arena.reset();

            if (runtime.window.minimized()) {
                // poll() does not block, so without this the loop pins one core
                // while the window is minimized and there is nothing to draw.
                std::this_thread::sleep_for(std::chrono::milliseconds(kMinimizedSleepMs));
                // No frame ran, so the next delta must not count the idle time.
                last_frame = std::chrono::steady_clock::now();
                continue;
            }

            // Between frames, because freeing a resource waits for the frames
            // in flight and a frame cannot wait for itself.
            apply_hot_reload(runtime, context, world);

            // The window reports its new size before the swapchain knows about it.
            const engine::gfx::Extent2D extent = window_extent(runtime.window);
            if (extent.width != last_extent.width || extent.height != last_extent.height) {
                if (!rebuild_swapchain(runtime.device, extent)) {
                    return false;
                }
                last_extent = extent;
            }

            const auto now = std::chrono::steady_clock::now();
            const float seconds = std::chrono::duration<float>(now - started).count();
            // A long stall, a debugger break, or a driver hitch would otherwise
            // multiply move_speed by the whole gap and throw the camera across
            // the scene in one step.
            const float delta = std::min(
                std::chrono::duration<float>(now - last_frame).count(), kLongestFrame);
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
                continue;
            }

            ++frame;
            settings.frames_drawn = frame;

            if (now - last_report >= std::chrono::seconds(1)) {
                ENGINE_LOG_INFO("frame {} | {}x{} | arena high water {} bytes | workers {}",
                                frame, drawn_extent.width, drawn_extent.height,
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

    entt::entity selected = entt::null;
    const FrameContext context{
        .device = runtime.device,
        .mesh_pass = &runtime.mesh,
        .game_content = &runtime.game_content,
        .settings = &settings,
        .world = &world,
        .selected = &selected,
        .content = content,
    };

    const bool ok = run_frames(runtime, context, options, frame_arena, settings, world);

    stop(runtime);
    engine::jobs::shutdown();
    engine::log::shutdown();
    return ok ? 0 : 1;
}
