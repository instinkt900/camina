// M3.3 tests for the game module.
//
// These read the content the sandbox actually ships. A test that built its own
// scene would pass while the shipped files were broken, and the shipped files
// are what the runtime opens.

#include "assets/content.h"
#include "import/source_assets.h"
#include "assets/reference.h"
#include "assets/texture.h"
#include "check.h"
#include "core/jobs.h"
#include "math/transform.h"
#include "assets/script.h"
#include "physics/components.h"
#include "physics/simulation.h"
#include "sandbox/components.h"
#include "sandbox/game.h"
#include "scene/component_registry.h"
#include "scene/components.h"
#include "scene/prefab.h"
#include "scene/references.h"
#include "scene/scene_file.h"
#include "scene/world.h"
// Outside the guard below. The component is registered in every build since
// #288, because a scene that names a script has to open with no interpreter.
#include "script/components.h"

#if defined(ENGINE_WITH_LUA)
#include "platform/input.h"
#include "play/session.h"
#include "scene/step_motion.h"
#include "script/host.h"

#if defined(ENGINE_WITH_AUDIO)
#include "audio/bus.h"
#include "audio/mixer.h"
#include "audio/scene_audio.h"
#include "audio/script_audio.h"
#endif
#endif

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace {

    using test::check;
    using test::section;
    namespace sc = engine::scene;

    /**
     * The registry the runtime builds, in the order it builds it.
     *
     * Physics and script register their own, the same way the game does, so a
     * registry missing either one loads the shipped scene and drops every
     * RigidBody, collider and ScriptComponent it carries. It says so once for
     * each entity and carries on, which reads like noise rather than like the
     * scene arriving half built. See issue #280.
     */
    sc::ComponentRegistry make_registry() {
        sc::ComponentRegistry registry;
        sc::register_builtin_components(registry);
        engine::physics::register_components(registry);
        engine::script::register_components(registry);
        sandbox::register_components(registry);
        return registry;
    }

    /// Drops every generated entity identity, which no two loads agree on.
    void strip_identities(nlohmann::json& document) {
        if (document.is_object()) {
            document.erase("id");
        }
        // A range-for over a JSON object walks its values, which is what this
        // wants. items() would give a proxy that clang-tidy asks to be const,
        // and these values are edited.
        if (document.is_object() || document.is_array()) {
            for (nlohmann::json& value : document) {
                strip_identities(value);
            }
        }
    }

    /// Every name in the world, so a test can say what arrived.
    std::vector<std::string> names(const sc::World& world) {
        std::vector<std::string> found;
        for (const auto [entity, name] : world.registry().view<const sc::Name>().each()) {
            (void)entity;
            found.push_back(name.value);
        }
        return found;
    }

    bool holds(const std::vector<std::string>& all, const std::string& wanted) {
        return std::find(all.begin(), all.end(), wanted) != all.end();
    }

    void test_registration() {
        sc::ComponentRegistry registry;
        sandbox::register_components(registry);
        check(registry.size() == 2, "the game registers two components of its own");
        check(registry.find("Spin") != nullptr, "Spin is findable by the name a file stores");
        check(registry.find("Goal") != nullptr, "and so is the Goal the puzzle keeps its win on");

        // The engine never names a game type. The game joins the same registry,
        // and so do physics and script. Seven built in, three physics, the
        // game's own Spin and Goal, and the one ScriptComponent.
        const sc::ComponentRegistry full = make_registry();
        // The two audio components joined at M11.4, and ScriptComponent is the
        // same case since #288. Each is registered in every build, whatever the
        // option, because a scene that carries one has to open either way. An
        // option decides what runs, never what a document may hold. See
        // scene/components.h and script/components.h.
        constexpr std::size_t kExpected = 15;
        check(full.size() == kExpected, "every subsystem and the game share one registry");
    }

    /**
     * Loads the shipped content, with the cooked tree open beside it.
     *
     * The model prefab lives in the cooked tree rather than in the source
     * tree, because the cooker writes it from the glTF node tree. So loading
     * the game needs both: the source directory for the hand-authored prefab
     * and the scene, and the manifest to turn a model path into its identity.
     */
    [[nodiscard]] bool load_shipped(sc::World& world, const sc::ComponentRegistry& registry,
                                    sc::PrefabLibrary& library) {
        engine::assets::Content cooked;
        if (!cooked.open(sandbox::default_content_directory())) {
            return false;
        }
        return sandbox::load(sandbox::default_content_directory(), &cooked, world, registry,
                             library);
    }

    /**
     * Every mesh the scene names has to be a mesh the cooker wrote.
     *
     * The scene stores those GUIDs as text. They are derived rather than
     * generated, so they are stable, and nothing checks them at load: a mesh
     * that will not resolve draws nothing and the scene still opens. The
     * failure is therefore silent, and it looks exactly like a mesh that
     * failed to upload.
     *
     * This is what catches a stale GUID after somebody replaces the model or
     * its sidecar.
     */
    void test_every_named_mesh_is_cooked() {
        const sc::ComponentRegistry registry = make_registry();
        sc::PrefabLibrary library;
        sc::World world;
        check(load_shipped(world, registry, library),
              "the shipped content loads");

        engine::assets::Content content;
        check(content.open(sandbox::default_content_directory()), "the cooked content opens");

        std::size_t named = 0;
        std::size_t resolved = 0;
        for (const auto [entity, renderer] :
             world.registry().view<const sc::MeshRenderer>().each()) {
            ++named;
            if (renderer.mesh.valid() &&
                engine::assets::find_by_guid(content.manifest(), renderer.mesh) != nullptr) {
                ++resolved;
            }
        }

        // The room, sixteen from the eight crate instances, which carry a mesh
        // on the box and on the lid, six from the flight helmet, the beacon,
        // the two glass panes, and the seven roughness spheres. A prefab root
        // the cooker added draws nothing, so it names no mesh, and neither does
        // the floor body, which the room already draws.
        check(named == 33, "every entity that draws names a mesh");
        check(resolved == named, "and the cooker wrote every one of them");
    }

    /**
     * The shipped scene names an environment, and that GUID is a real cubemap.
     *
     * Counting entities does not say this. The count stays right when somebody
     * drops the component and leaves the entity, and it stays right when the
     * GUID goes stale. Both failures end the same way: the picture falls back
     * to six grey texels and nothing reports it.
     *
     * The face count is read from the cooked file rather than assumed, because
     * a flat texture named here binds nothing a `samplerCube` can read.
     */
    void test_shipped_environment_is_a_cubemap() {
        const sc::ComponentRegistry registry = make_registry();
        sc::PrefabLibrary library;
        sc::World world;
        check(load_shipped(world, registry, library), "the shipped content loads");

        engine::assets::Content content;
        check(content.open(sandbox::default_content_directory()), "the cooked content opens");

        std::size_t found = 0;
        std::size_t cubemaps = 0;
        for (const auto [entity, environment] :
             world.registry().view<const sc::Environment>().each()) {
            ++found;
            std::vector<std::byte> bytes;
            engine::assets::TextureView view;
            if (environment.cubemap.valid() && content.read_bytes(environment.cubemap, bytes) &&
                engine::assets::read_texture(bytes, view, "the shipped environment") &&
                view.face_count == engine::assets::kCubeFaceCount) {
                ++cubemaps;
            }
        }

        check(found == 1, "the scene names exactly one environment");
        check(cubemaps == 1, "and the cooker wrote it as a six-face cubemap");
    }

    /**
     * A prefab name is decided by the cooked path, not by manifest order.
     *
     * The identity of a cooked prefab is derived from its glTF scene index, and
     * the cooked path carries that same index. A name counted from the position
     * in the manifest would be a second source of truth. It agrees today only
     * because the cooker writes the scenes in ascending order, and a scene file
     * that named the wrong prefab would build the wrong entities in silence.
     */
    void test_prefab_names_come_from_the_cooked_path() {
        const std::string source = "models/room/room.gltf";

        check(engine::assets::prefab_name(source, source + ".0.prefab") == source,
              "the first scene keeps the source path");
        check(engine::assets::prefab_name(source, source + ".1.prefab") == source + "#1",
              "a later scene carries its index");
        check(engine::assets::prefab_name(source, source + ".2.prefab") == source + "#2",
              "and the index is the one in the path");
        check(engine::assets::prefab_name("crate.prefab", "crate.prefab") == "crate.prefab",
              "a copied prefab keeps the source path");

        // The reordering the finding asked for. Reading the outputs of one
        // source backwards has to give every prefab the same name it had
        // forwards, because nothing in the name is counted.
        const std::vector<std::string> forwards{ source + ".0.prefab", source + ".1.prefab",
                                                 source + ".2.prefab" };
        std::vector<std::string> backwards = forwards;
        std::ranges::reverse(backwards);

        for (const std::string& cooked : backwards) {
            const std::string expected =
                cooked == forwards[0] ? source
                                      : source + "#" + cooked.substr(source.size() + 1, 1);
            check(engine::assets::prefab_name(source, cooked) == expected,
                  "the name does not move when the outputs do");
        }

        // A shape the cooker does not write today. It must not collide with the
        // source path, because two prefabs under one name lose one of them.
        check(engine::assets::prefab_name(source, source + ".later.prefab") != source,
              "an unplanned shape does not take the source path");
    }

    void test_content_is_there() {
        const std::filesystem::path content = sandbox::default_content_directory();
        check(std::filesystem::is_directory(content), "the content directory exists");
        check(std::filesystem::exists(content / sandbox::kSceneFile), "the scene file ships");
        check(std::filesystem::exists(content / "crate.prefab"), "the crate prefab ships");
    }

    void test_shipped_scene_loads() {
        const sc::ComponentRegistry registry = make_registry();
        sc::PrefabLibrary library;

        sc::World world;
        check(load_shipped(world, registry, library),
              "the shipped content loads");
        // Every prefab in the cooked tree, which is the two hand-authored ones
        // and the five the cooker wrote from a glTF node tree. The crate glTF is
        // one of those five, and nothing instances it, because crate.prefab
        // wraps it. Registering it anyway is what "every prefab" means. The
        // second hand-authored one is thrown_crate.prefab, which carries the
        // body and the collider a thrown crate needs.
        check(library.size() == 7, "every prefab in the cooked tree went into the library");

        // The room, seven crate instances of two entities each, one beacon,
        // seven for the flight helmet (the root the cooker added, and one for
        // each of the six nodes the model holds), the three lights, three for
        // the glass (a cooker-added root and the two panes), eight for the
        // spheres (a cooker-added root and one for each roughness step), the
        // one that carries the environment, the floor body, and the goal volume.
        // Three of the crates are the M7.6 stack, one is the M7.4 crate that
        // drops onto the floor body beside it, and one is the scaled crate of
        // #237, which collides at the size it draws rather than at the size of
        // its collider.
        //
        // The goal volume is the M8.4 trigger. The M7.4 crate falls through it,
        // so the sandbox exercises an overlap with nobody touching a key.
        // Forty three since M9.5a, which added the camera the game plays through.
        check(world.size() == 43, "the scene holds forty three entities");

        const std::vector<std::string> found = names(world);
        check(holds(found, "crate"), "a crate that took the prefab name is there");
        check(holds(found, "small crate"), "an instance that overrode its name is there");
        check(holds(found, "stacked crate"), "the instance parented to another instance is there");
        check(holds(found, "beacon"), "the entity that is not an instance is there");
        check(holds(found, "lid"), "a child the prefab supplied is there");

        std::size_t instances = 0;
        for (const auto entity : world.registry().view<const sc::PrefabInstance>()) {
            (void)entity;
            ++instances;
        }
        // The room, the eight crates, the flight helmet, the glass, and the
        // spheres. The helmet is one instance rather than six hand-written
        // entities, which is what the node tree becoming a prefab bought.
        check(instances == 12, "twelve entities are prefab instances");
    }

    void test_scene_round_trips() {
        const sc::ComponentRegistry registry = make_registry();
        sc::PrefabLibrary library;

        sc::World world;
        check(load_shipped(world, registry, library),
              "the shipped content loads");

        // The shipped file has to survive a save and a load, or an editor would
        // damage it the first time a user pressed save.
        const nlohmann::json first = sc::save_scene(world, registry, library);
        sc::World again;
        check(sc::load_scene(first, again, registry, library), "the saved scene loads back");
        check(sc::save_scene(again, registry, library) == first,
              "the shipped scene round trips unchanged");
    }

    /**
     * Issue #73. What the Save button does, minus the button.
     *
     * A live world holds identities and the source scene holds references, so
     * saving has to put the references back. Without that a save replaces every
     * name a person wrote with the GUID it resolved to, and the next person to
     * read the file finds a value nobody chose.
     */
    void test_saving_puts_the_references_back() {
        const sc::ComponentRegistry registry = make_registry();
        sc::PrefabLibrary library;

        engine::assets::Content content;
        check(content.open(sandbox::default_content_directory()), "the cooked content opens");

        sc::World world;
        check(load_shipped(world, registry, library), "the shipped content loads");

        nlohmann::json saved = sc::save_scene(world, registry, library);
        const std::string identities = saved.dump();
        check(identities.find("asset:") == std::string::npos,
              "a world saves identities, because that is what it holds");

        const std::size_t restored = sc::restore_references(saved, content.manifest(), registry);
        check(restored > 0, "saving puts references back");

        const std::string text = saved.dump();
        check(text.find("asset:models/crate/crate.gltf#mesh:0") != std::string::npos,
              "and the crate mesh reads as the path the source names");

        // The names the scene carries are ordinary strings and must come
        // through untouched.
        check(text.find("\"beacon\"") != std::string::npos, "an ordinary name is untouched");

        // What went back has to be what the cooker reads forward again, or the
        // save writes a file the next cook cannot resolve. The walk is the one
        // the save used, so a scene of prefab instances is covered: nearly
        // every reference the sandbox holds sits in an override patch.
        std::size_t seen = 0;
        sc::for_each_reference_field(saved, registry, [&](nlohmann::json& value) {
            const auto text_value = value.get<std::string>();
            if (!text_value.starts_with(engine::assets::kAssetPrefix)) {
                return true;
            }
            ++seen;
            engine::assets::AssetReference parsed;
            check(engine::assets::parse_reference(text_value, parsed),
                  "every reference written back reads forward again");
            return true;
        });
        check(seen == restored, "and the walk finds every one of them again");
    }

    void test_overrides_reach_the_world() {
        const sc::ComponentRegistry registry = make_registry();
        sc::PrefabLibrary library;

        sc::World world;
        check(load_shipped(world, registry, library),
              "the shipped content loads");
        world.update();

        // The scene adds Spin to one instance through an override. The prefab
        // has no Spin at all, so this only works when a patch can add a
        // component rather than only change a field.
        std::size_t spinning_instances = 0;
        for (const auto [entity, spin] : world.registry().view<const sandbox::Spin>().each()) {
            (void)spin;
            if (world.registry().all_of<sc::PrefabInstance>(entity)) {
                ++spinning_instances;
            }
        }
        check(spinning_instances == 1, "an override added a component the prefab does not have");

        // The stacked crate is a child of another instance, so its world
        // position has to include the parent it hangs off.
        //
        // The parent is found rather than written down. The property under test
        // is that the child inherited an x it does not carry itself, and a
        // number copied from the scene file would only say where the crate
        // happened to stand on the day this was written.
        bool checked_stack = false;
        for (const auto [entity, name] : world.registry().view<const sc::Name>().each()) {
            if (name.value != "stacked crate") {
                continue;
            }
            const auto* node = world.registry().try_get<sc::Hierarchy>(entity);
            const bool hangs = node != nullptr && node->parent != entt::null;
            check(hangs, "the stacked crate hangs off another entity");
            if (!hangs) {
                continue;
            }

            const engine::Mat4& matrix = world.world_matrix(entity);
            const engine::Mat4& above = world.world_matrix(node->parent);
            check(matrix[3][0] == above[3][0],
                  "the stacked crate took its x from the crate it sits on");
            check(matrix[3][1] > above[3][1], "and it sits above that crate");
            checked_stack = true;
        }
        check(checked_stack, "and the stacked crate was there to check");
    }


#if defined(ENGINE_WITH_LUA)

    /// The first entity that carries this name, or null when none does.
    [[nodiscard]] entt::entity find_by_name(const sc::World& world, std::string_view wanted) {
        for (const auto [entity, name] : world.registry().view<const sc::Name>().each()) {
            if (name.value == wanted) {
                return entity;
            }
        }
        return entt::null;
    }

    // ---------------------------------------------------------------------
    // The game, which is entirely Lua from M8.6.
    //
    // Every test below drives a cooked .lua through the script callbacks. None
    // of them can be satisfied by a C++ entry point, because there is no longer
    // one to call: sandbox::update and throw_crate are both gone. A test that
    // kept one alive would pass while the binding did nothing, which is the one
    // result worse than no test at all.
    // ---------------------------------------------------------------------

    /// The step this test drives the session at, in seconds.
    ///
    /// The same number the runtime feeds an offscreen frame. One step for each
    /// call, so a run of N is N steps whatever the machine is doing.
    constexpr float kStepSeconds = 1.0F / 60.0F;

    /// Everything one step of the real game needs, built from the shipped tree.
    ///
    /// The step order is engine::play::Session::advance and not a copy of it.
    /// This used to hold its own, under a comment saying it was the order the
    /// runtime runs. The runtime stopped holding that order at #315, and a
    /// copy with no compiler tying it to the original drifts with nothing to
    /// report it.
    struct Game {
        /// The cooked tree, held rather than opened and dropped. The audio
        /// surfaces keep a reference to it, so it has to outlive them.
        engine::assets::Content cooked;
        sc::PrefabLibrary library;
        sc::World world;

        /// The clock, the solver, the scripts and the input the game reads.
        engine::play::Session session;

        /// Where the view stands, for a script that acts along the line of
        /// sight. The throw reads it.
        engine::play::View view;

#if defined(ENGINE_WITH_AUDIO)
        /// A real mixer with no device under it. Nothing is audible and every
        /// count is real, which is what a check about sound needs.
        engine::audio::Mixer mixer;
        engine::audio::ScriptAudio script_audio;
        engine::audio::SceneAudio scene_audio;
#endif

        /// Every trigger overlap since the game started. The simulation keeps
        /// the events of one step, so a check written after the run would read
        /// an empty list and pass whatever happened.
        std::vector<engine::physics::Simulation::Touch> triggers;

        /// One frame worth exactly one fixed step.
        void step() {
            // What the devices read this frame, which is nothing unless press()
            // fed a key for this one. The session folds every device frame
            // since the last step into what that step reads, so a key fed once
            // is down for one step and up for the next.
            session.feed_input(engine::platform::InputFrame{});
            session.advance(world, view, kStepSeconds);

            // The simulation keeps the events of one step, and one advance ran
            // one step, so this is that step's list.
            for (const engine::physics::Simulation::Touch& touch :
                 session.simulation().trigger_events()) {
                triggers.push_back(touch);
            }
            world.update();
        }

        void run(std::uint32_t steps) {
            for (std::uint32_t i = 0; i < steps; ++i) {
                step();
            }
        }

        /// Holds @p key down for the step that follows.
        void press(engine::platform::Key key) {
            engine::platform::InputFrame frame;
            frame.keys.at(static_cast<std::size_t>(key)) = true;
            session.feed_input(frame);
        }
    };

    /// Loads the shipped scene and every cooked script into a Game.
    [[nodiscard]] bool start_game(Game& game) {
        // The process-wide registry, filled the way apps/runtime fills it.
        // engine::play::Session builds its script host from that one, so a
        // registry local to this test would leave the host unable to name a
        // component the game defines.
        sc::register_builtin_components();
        engine::physics::register_components();
        engine::script::register_components();
        sandbox::register_components();

        // The game's own table, not a copy of it. A key changed in
        // sandbox::bind_actions used to leave this test pressing the old one,
        // and the failure read as a broken script.
        sandbox::bind_actions(game.session.input());

        if (!game.cooked.open(sandbox::default_content_directory())) {
            return false;
        }
        // The process-wide library too, for the same reason: a script instances
        // a prefab through engine::scene::prefabs() and a session hands it that
        // one.
        if (!sandbox::load(sandbox::default_content_directory(), &game.cooked, game.world)) {
            return false;
        }

#if defined(ENGINE_WITH_AUDIO)
        // A mixer with no device under it. Nothing pulls frames from it, so
        // nothing is heard and nothing is timed: what a voice count says here
        // is exactly what the game asked for.
        if (!game.mixer.create(2, 48000)) {
            return false;
        }
        game.script_audio.bind(game.mixer, game.cooked);
        game.scene_audio.bind(game.mixer, game.cooked);
        game.session.set_audio(&game.scene_audio);
        game.session.set_script_audio(&game.script_audio);
#endif

        // A tree with no script would make every test below vacuous, so this
        // asks the project what it holds before it loads anything.
        std::vector<engine::assets::AssetRecord> scripts;
        if (!game.cooked.assets_of_kind(engine::assets::kScriptExtension, scripts) ||
            scripts.empty()) {
            return false;
        }

        // The engine's loader, the same call the runtime makes. It reads the
        // project rather than a list somebody keeps up to date.
        game.session.load_scripts(game.cooked);
        game.session.build(game.world);
        return true;
    }

    void test_spin_turns_what_it_should() {
        section("spin.lua turns the entities the scene gives a Spin");

        Game game;
        check(start_game(game), "the shipped game starts");

        // Two entities carry a Spin, and each carries spin.lua beside it. The
        // count is the point: a script that reached one of them would pass a
        // check that only asked whether anything moved.
        std::vector<entt::entity> spinners;
        for (const auto [entity, spin] : game.world.registry().view<const sandbox::Spin>().each()) {
            (void)spin;
            spinners.push_back(entity);
        }
        check(spinners.size() == 2, "the scene carries two spinning entities");

        game.step();
        game.world.update();
        std::vector<engine::Mat4> first;
        for (const entt::entity entity : spinners) {
            first.push_back(game.world.world_matrix(entity));
        }

        game.run(60);
        game.world.update();
        std::size_t turned = 0;
        for (std::size_t i = 0; i < spinners.size(); ++i) {
            if (game.world.world_matrix(spinners.at(i)) != first.at(i)) {
                ++turned;
            }
        }
        check(turned == 2, "a second later both of them have turned");

        // The step owns the pose, so a frame between two steps blends it rather
        // than showing the newest. Without this a Spin steps at 60 Hz and holds
        // still in between, which is the judder a fixed step exists to remove.
        check(game.session.motion().tracked() == 2, "and both are recorded for blending");
    }

    void test_a_bad_spin_turns_nothing() {
        section("spin.lua leaves a stopped or degenerate Spin alone");

        Game game;
        check(start_game(game), "the shipped game starts");

        // Built here rather than shipped, because the scene has no reason to
        // carry a broken one. The script is the shipped script all the same.
        const engine::Guid spin_script =
            game.world.registry()
                .get<engine::script::ScriptComponent>(
                    game.world.registry().view<const sandbox::Spin>().front())
                .script;

        const auto add = [&](const sandbox::Spin& spin) {
            const entt::entity entity = game.world.create();
            game.world.set_local(entity, engine::Transform{});
            game.world.registry().emplace<sandbox::Spin>(entity, spin);
            game.world.registry().emplace<engine::script::ScriptComponent>(
                entity, engine::script::ScriptComponent{ spin_script });
            return entity;
        };

        const entt::entity stopped = add(sandbox::Spin{ .seconds_per_turn = 0.0F });
        const entt::entity degenerate =
            add(sandbox::Spin{ .axis = { 0.0F, 0.0F, 0.0F }, .seconds_per_turn = 1.0F });

        game.run(30);
        game.world.update();

        check(game.world.local(stopped).rotation == engine::Quat{ 1.0F, 0.0F, 0.0F, 0.0F },
              "a turn of zero seconds leaves the entity alone");

        // A zero axis normalized would fill the rotation with NaN, and NaN
        // spreads to every child through the matrix.
        for (const entt::entity entity : { stopped, degenerate }) {
            const engine::Mat4& matrix = game.world.world_matrix(entity);
            check(matrix[0][0] == matrix[0][0], "the matrix holds no NaN");
        }
    }

    /// Where an entity ended up after the hierarchy composed.
    [[nodiscard]] engine::Vec3 world_position(const sc::World& world, entt::entity entity) {
        const engine::Mat4& matrix = world.world_matrix(entity);
        return engine::Vec3{ matrix[3][0], matrix[3][1], matrix[3][2] };
    }

    /// How many entities carry this name.
    [[nodiscard]] std::size_t count_named(const sc::World& world, std::string_view wanted) {
        std::size_t found = 0;
        for (const auto [entity, name] : world.registry().view<const sc::Name>().each()) {
            (void)entity;
            if (name.value == wanted) {
                ++found;
            }
        }
        return found;
    }

    void test_the_throw_makes_a_crate_that_falls() {
        section("puzzle.lua throws a crate when the throw action fires");

        Game game;
        check(start_game(game), "the shipped game starts");
        game.view = engine::play::View{ .position = { 0.0F, 3.0F, 4.0F },
                                        .forward = { 0.0F, 0.0F, -1.0F } };

        game.run(2);
        const std::size_t bodies = game.session.simulation().body_count();
        check(count_named(game.world, "thrown crate") == 0, "nothing has been thrown yet");

        game.press(sandbox::kThrowKey);
        game.step();

        check(count_named(game.world, "thrown crate") == 1, "the throw made one crate");
        check(game.session.simulation().body_count() == bodies + 1,
              "and it has a body, so it is not hanging in the air");

        // The press edge and not the key being down. A throw on every step would
        // fill the room in one second.
        game.run(10);
        check(count_named(game.world, "thrown crate") == 1, "and one press throws one crate");
    }

    void test_a_crate_at_rest_in_the_goal_wins() {
        section("puzzle.lua wins when a stack crate settles in the goal");

        Game game;
        check(start_game(game), "the shipped game starts");

        const entt::entity goal = find_by_name(game.world, "goal volume");
        const entt::entity crate = find_by_name(game.world, "stack crate 0");
        check(goal != entt::null && crate != entt::null, "the goal and the stack are there");

        // Put a crate in the goal and let it settle. teleport rather than a
        // Transform write, because a dynamic body owns its pose.
        game.run(2);
        const engine::Vec3 target = world_position(game.world, goal);
        check(game.session.simulation().teleport(game.world, crate,
                                                 engine::Vec3{ target.x, 0.5F, target.z },
                                                 engine::Quat{ 1.0F, 0.0F, 0.0F, 0.0F }),
              "the crate moves into the goal");

        // Long enough for the body to come to rest, because the win waits for a
        // crate to settle rather than firing as it passes through.
        game.run(240);

        check(!game.session.simulation().is_awake(crate), "the crate went to sleep in the goal");
        check(game.session.scripts().stopped_count() == 0, "and no script raised an error");

        // The win itself, read off the component the script keeps it on. A log
        // line is what a player sees and nothing a test can check.
        const auto* won = game.world.registry().try_get<const sandbox::Goal>(goal);
        check(won != nullptr && won->won, "and the puzzle says it is won");
        check(game.session.scripts().call_count(engine::script::Callback::Trigger) > 0,
              "the goal volume reported the crate crossing into it");

        // The direction, which the M8.4 test used to carry. The volume has to be
        // the trigger side and the crate the visitor. A swap would send every
        // game event to the wrong object, and puzzle.lua would never see a win.
        bool named_both = false;
        for (const engine::physics::Simulation::Touch& touch : game.triggers) {
            named_both = named_both || (touch.a == goal && touch.b == crate);
        }
        check(named_both, "and it named the volume first and the crate second");
    }

#if defined(ENGINE_WITH_AUDIO)

    /**
     * The game makes a sound when something happens, and not before.
     *
     * This is the milestone test of M11 in the form a test can hold. The rest
     * of it is a person listening, which no check here stands in for.
     *
     * The counts come off a mixer with no device under it, so nothing is heard
     * and nothing is timed. A voice count is then exactly what the game asked
     * for.
     */
    void test_the_game_makes_a_sound() {
        section("the shipped game plays sounds");

        Game game;
        check(start_game(game), "the shipped game starts");

        // A game that has done nothing has played nothing. Without this a build
        // that played a sound on every step would pass every check below.
        game.run(30);
        check(game.mixer.started() == 0, "a game where nothing happened has played nothing");

        game.press(sandbox::kThrowKey);
        game.run(1);
        const std::uint64_t after_throw = game.mixer.started();
        check(after_throw >= 1, "the throw plays something");

        // The crate lands, hits the stack, and the stack moves. Every one of
        // those is a contact, and crate_sound.lua is what turns them into a
        // sound.
        game.run(120);
        const std::uint64_t after_landing = game.mixer.started();
        check(after_landing > after_throw, "and landing plays more");

        // Not one for every contact. A crate that lands touches many times over
        // and a stack touches every neighbour, so a thud for each one is a
        // burst rather than an impact.
        check(after_landing - after_throw < 30,
              "but not one for every contact, because the cooldown holds them apart");

        game.press(sandbox::kResetKey);
        game.run(1);
        check(game.mixer.started() > after_landing, "and the reset plays something");

        // Four sounds ship and the run above has reason to reach three of them:
        // the throw, the thud, and the reset. The click needs a button.
        check(game.mixer.sounds() >= 3, "at least three different sounds were loaded");
    }

    /**
     * The world's sounds and the menu's are on different buses.
     *
     * That split is what lets a pause quiet the room without silencing the menu
     * that is doing the pausing. **The pause itself is not checked here**, and
     * cannot be: this build binds no UI surface, so `pause_game` in puzzle.lua
     * returns before it pauses. A pause with no menu on the screen would be a
     * game nobody could resume, which M10 settled. The runtime's audio report
     * is where that half is read instead.
     *
     * What this does check is the half a test can hold: a thrown crate is heard
     * on the effects bus, so muting that bus reaches it.
     */
    void test_the_world_plays_on_its_own_bus() {
        section("the world's sounds are on the effects bus");

        Game game;
        check(start_game(game), "the shipped game starts");
        check(game.mixer.buses() == 0, "a game that has played nothing has built no bus");

        game.press(sandbox::kThrowKey);
        game.run(60);

        check(game.mixer.buses() == 1, "the throw and the landing built one bus");
        check(!game.mixer.bus_settings(engine::audio::Bus::Effects).mute,
              "which is not muted while the game runs");
        check(!game.mixer.bus_settings(engine::audio::Bus::Master).mute,
              "and neither is the master, because the menu lives on it");
    }

#endif

    void test_the_reset_puts_the_room_back() {
        section("puzzle.lua puts the crates back and clears what was thrown");

        Game game;
        check(start_game(game), "the shipped game starts");
        game.view = engine::play::View{ .position = { 0.0F, 3.0F, 4.0F },
                                        .forward = { 0.0F, 0.0F, -1.0F } };

        const entt::entity crate = find_by_name(game.world, "stack crate 2");
        check(crate != entt::null, "the top of the stack is there");

        game.run(2);
        const engine::Vec3 home = world_position(game.world, crate);

        // Knock it a long way from home, and throw something as well, so the
        // reset has both kinds of work to undo.
        check(game.session.simulation().teleport(game.world, crate, engine::Vec3{ 4.0F, 0.5F, 4.0F },
                                                 engine::Quat{ 1.0F, 0.0F, 0.0F, 0.0F }),
              "the crate is moved away");
        game.press(sandbox::kThrowKey);
        game.step();
        check(count_named(game.world, "thrown crate") == 1, "and a crate was thrown");

        game.run(30);
        check(glm::length(world_position(game.world, crate) - home) > 1.0F,
              "the crate is nowhere near where it started");

        game.press(sandbox::kResetKey);
        game.step();
        game.run(2);

        check(glm::length(world_position(game.world, crate) - home) < 0.1F,
              "the reset put the crate back where the scene had it");
        check(count_named(game.world, "thrown crate") == 0,
              "and took away what the throw had made");
    }

#endif // ENGINE_WITH_LUA

} // namespace

// M13.4a. The whole of M13 rests on this: a project read from source and
// the same project read from a cooked tree build the same world. If they
// ever differ, the editor and the runtime are two engines and comparing
// their pictures means nothing.
void test_a_source_project_gives_the_same_world() {
    // make_registry(), not a hand-built one. The document rule resolves a
    // reference only in a component it knows, so a registry missing
    // ScriptComponent cannot resolve the script a scene names.
    const sc::ComponentRegistry registry = make_registry();

    sc::World cooked_world;
    sc::PrefabLibrary cooked_library;
    check(load_shipped(cooked_world, registry, cooked_library),
          "the cooked project loads");

    engine::import::SourceAssets source;
    check(source.open(ENGINE_GAME_CONTENT_SOURCE), "the source project opens");
    source.set_components(&registry);

    sc::World source_world;
    sc::PrefabLibrary source_library;
    check(sandbox::load(ENGINE_GAME_CONTENT_SOURCE, &source, source_world, registry,
                        source_library),
          "and the same project loads from source");

    check(source_world.size() == cooked_world.size(),
          "both worlds hold the same number of entities");

    // The whole world as a document, not just a count. A count would pass
    // while every transform and every asset reference was wrong.
    //
    // Without the identities. An entity that comes back with no id in the
    // file is given a fresh one, so two loads of the same scene disagree
    // there and always will. Everything that says what the world is stays
    // in: the components, the parents, the order, and the asset each
    // renderer names.
    nlohmann::json from_cooked = sc::save_scene(cooked_world);
    nlohmann::json from_source = sc::save_scene(source_world);
    strip_identities(from_cooked);
    strip_identities(from_source);
    check(from_source == from_cooked, "and the two worlds write out the same document");
    if (from_source != from_cooked) {
        ENGINE_LOG_ERROR("cooked: {}", from_cooked.dump().substr(0, 400));
        ENGINE_LOG_ERROR("source: {}", from_source.dump().substr(0, 400));
    }
}


// M13.6. The milestone's own claim, over the tree the game actually ships
// rather than a tree a test built.
//
// The editor draws what it imports and the runtime draws what the cooker
// wrote. Both go through the same render code, so the picture can only
// differ if the bytes differ or the scene resolves differently. This checks
// both, asset by asset, over every asset in sandbox/content.
void test_the_editor_imports_what_the_cooker_writes() {
    const sc::ComponentRegistry registry = make_registry();

    engine::assets::Content cooked;
    check(cooked.open(sandbox::default_content_directory()),
          "the cooked tree the runtime reads opens");

    engine::import::SourceAssets source;
    check(source.open(ENGINE_GAME_CONTENT_SOURCE), "and the source tree the editor reads");
    source.set_components(&registry);

    std::size_t compared = 0;
    std::size_t missing = 0;
    std::size_t differed = 0;

    for (const engine::assets::ManifestEntry& entry : cooked.manifest().entries) {
        for (const engine::assets::ManifestOutput& output : entry.outputs) {
            std::vector<std::byte> from_cook;
            if (!cooked.read(output.guid, from_cook)) {
                continue;
            }

            std::vector<std::byte> from_import;
            if (!source.read(output.guid, from_import)) {
                ENGINE_LOG_ERROR("{} is in the cooked tree and the editor cannot import it.",
                                 output.cooked);
                ++missing;
                continue;
            }

            if (from_import != from_cook) {
                ENGINE_LOG_ERROR("{}: cooked {} bytes, imported {}.", output.cooked,
                                 from_cook.size(), from_import.size());
                ++differed;
            }
            ++compared;
        }
    }

    ENGINE_LOG_INFO("Compared {} shipped asset(s).", compared);
    check(compared >= 25, "every asset the game ships was compared");
    check(missing == 0, "the editor can import all of them");
    check(differed == 0, "and every one is byte for byte what the cooker wrote");
}


int main() {
    std::printf("registration\n");
    test_registration();
    std::printf("prefab names\n");
    test_prefab_names_come_from_the_cooked_path();
    std::printf("shipped content\n");
    test_content_is_there();
    test_shipped_scene_loads();
    std::printf("a source project\n");
    test_a_source_project_gives_the_same_world();
    test_the_editor_imports_what_the_cooker_writes();
    test_every_named_mesh_is_cooked();
    test_shipped_environment_is_a_cubemap();
    test_scene_round_trips();
    test_saving_puts_the_references_back();
    test_overrides_reach_the_world();

    // The physics world runs its solver on the scheduler, so everything below
    // that steps one needs the pool up.
    engine::jobs::init();

#if defined(ENGINE_WITH_LUA)
    // The game is Lua, so a build without it has no game logic to drive. The
    // content tests above still run, because a scene and a prefab are the same
    // either way.
    std::printf("the game, which is entirely Lua\n");
    test_spin_turns_what_it_should();
    test_a_bad_spin_turns_nothing();
    test_the_throw_makes_a_crate_that_falls();
    test_a_crate_at_rest_in_the_goal_wins();
    test_the_reset_puts_the_room_back();
#if defined(ENGINE_WITH_AUDIO)
    test_the_game_makes_a_sound();
    test_the_world_plays_on_its_own_bus();
#endif
#endif

    engine::jobs::shutdown();
    return test::report();
}
