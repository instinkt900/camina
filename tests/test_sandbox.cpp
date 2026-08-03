// M3.3 tests for the game module.
//
// These read the content the sandbox actually ships. A test that built its own
// scene would pass while the shipped files were broken, and the shipped files
// are what the runtime opens.

#include "assets/content.h"
#include "check.h"
#include "math/transform.h"
#include "sandbox/components.h"
#include "sandbox/game.h"
#include "scene/component_registry.h"
#include "scene/components.h"
#include "scene/prefab.h"
#include "scene/scene_file.h"
#include "scene/world.h"

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

namespace {

    using test::check;
    namespace sc = engine::scene;

    /// The registry the runtime builds: engine types first, then game types.
    sc::ComponentRegistry make_registry() {
        sc::ComponentRegistry registry;
        sc::register_builtin_components(registry);
        sandbox::register_components(registry);
        return registry;
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
        check(registry.size() == 1, "the game registers one component of its own");
        check(registry.find("Spin") != nullptr, "Spin is findable by the name a file stores");

        // The engine never names a game type. The game joins the same registry.
        const sc::ComponentRegistry full = make_registry();
        check(full.size() == 4, "the engine types and the game type share one registry");
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
        check(sandbox::load(sandbox::default_content_directory(), world, registry, library),
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

        check(named == 6, "the scene names the six meshes the flight helmet holds");
        check(resolved == named, "and the cooker wrote every one of them");
    }

    void test_content_is_there() {
        const std::filesystem::path content = sandbox::default_content_directory();
        check(std::filesystem::is_directory(content), "the content directory exists");
        check(std::filesystem::exists(content / sandbox::kSceneFile), "the scene file ships");
        check(std::filesystem::exists(content /
                                      (std::string(sandbox::kCratePrefab) + ".prefab")),
              "the crate prefab ships");
    }

    void test_shipped_scene_loads() {
        const sc::ComponentRegistry registry = make_registry();
        sc::PrefabLibrary library;

        sc::World world;
        check(sandbox::load(sandbox::default_content_directory(), world, registry, library),
              "the shipped content loads");
        check(library.size() == 1, "the crate prefab went into the library");

        // Four crate instances of two entities each, one beacon, and one
        // entity for each of the six meshes the flight helmet holds.
        check(world.size() == 15, "the scene holds fifteen entities");

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
        check(instances == 4, "four entities are prefab instances");
    }

    void test_scene_round_trips() {
        const sc::ComponentRegistry registry = make_registry();
        sc::PrefabLibrary library;

        sc::World world;
        check(sandbox::load(sandbox::default_content_directory(), world, registry, library),
              "the shipped content loads");

        // The shipped file has to survive a save and a load, or an editor would
        // damage it the first time a user pressed save.
        const nlohmann::json first = sc::save_scene(world, registry, library);
        sc::World again;
        check(sc::load_scene(first, again, registry, library), "the saved scene loads back");
        check(sc::save_scene(again, registry, library) == first,
              "the shipped scene round trips unchanged");
    }

    void test_overrides_reach_the_world() {
        const sc::ComponentRegistry registry = make_registry();
        sc::PrefabLibrary library;

        sc::World world;
        check(sandbox::load(sandbox::default_content_directory(), world, registry, library),
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
        for (const auto [entity, name] : world.registry().view<const sc::Name>().each()) {
            if (name.value != "stacked crate") {
                continue;
            }
            const engine::Mat4& matrix = world.world_matrix(entity);
            check(matrix[3][0] == -2.5F,
                  "the stacked crate took its x from the crate it sits on");
            check(matrix[3][1] > 1.0F, "and it sits above that crate");
        }
    }

    void test_update_turns_what_it_should() {
        const sc::ComponentRegistry registry = make_registry();
        sc::PrefabLibrary library;

        sc::World world;
        check(sandbox::load(sandbox::default_content_directory(), world, registry, library),
              "the shipped content loads");

        check(sandbox::update(world, 0.0F) == 2, "the update moves both spinning entities");

        // The angle follows the elapsed time, so the same time gives the same
        // rotation. A test can therefore state an exact value.
        world.update();
        std::vector<engine::Mat4> at_zero;
        for (const auto [entity, spin] : world.registry().view<const sandbox::Spin>().each()) {
            (void)spin;
            at_zero.push_back(world.world_matrix(entity));
        }

        sandbox::update(world, 1.0F);
        world.update();
        std::size_t moved = 0;
        std::size_t i = 0;
        for (const auto [entity, spin] : world.registry().view<const sandbox::Spin>().each()) {
            (void)spin;
            if (world.world_matrix(entity) != at_zero.at(i)) {
                ++moved;
            }
            ++i;
        }
        check(moved == 2, "a second later both of them have turned");

        // Going back to the same time gives the same matrices, which is what
        // makes the elapsed-time form reproducible.
        sandbox::update(world, 0.0F);
        world.update();
        std::size_t same = 0;
        i = 0;
        for (const auto [entity, spin] : world.registry().view<const sandbox::Spin>().each()) {
            (void)spin;
            if (world.world_matrix(entity) == at_zero.at(i)) {
                ++same;
            }
            ++i;
        }
        check(same == 2, "the same elapsed time gives the same rotation");
    }

    void test_update_refuses_a_bad_spin() {
        sc::World world;
        const entt::entity stopped = world.create();
        world.registry().emplace<sandbox::Spin>(stopped,
                                                sandbox::Spin{ .seconds_per_turn = 0.0F });

        const entt::entity degenerate = world.create();
        world.registry().emplace<sandbox::Spin>(
            degenerate, sandbox::Spin{ .axis = { 0.0F, 0.0F, 0.0F }, .seconds_per_turn = 1.0F });

        check(sandbox::update(world, 1.0F) == 0, "neither entity turns");

        world.update();
        // A zero axis normalizes to NaN, and NaN spreads to every child through
        // the matrix. Nothing here may produce one.
        for (const auto entity : { stopped, degenerate }) {
            const engine::Mat4& matrix = world.world_matrix(entity);
            check(matrix[0][0] == matrix[0][0], "the matrix holds no NaN");
        }
    }

} // namespace

int main() {
    std::printf("registration\n");
    test_registration();
    std::printf("shipped content\n");
    test_content_is_there();
    test_shipped_scene_loads();
    test_every_named_mesh_is_cooked();
    test_scene_round_trips();
    test_overrides_reach_the_world();
    std::printf("the game loop\n");
    test_update_turns_what_it_should();
    test_update_refuses_a_bad_spin();
    return test::report();
}
