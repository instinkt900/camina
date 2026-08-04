// M3.3 tests for the game module.
//
// These read the content the sandbox actually ships. A test that built its own
// scene would pass while the shipped files were broken, and the shipped files
// are what the runtime opens.

#include "assets/content.h"
#include "assets/reference.h"
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

        // Eight from the four crate instances, which carry a mesh on the box
        // and on the lid, six from the flight helmet, and the beacon. The
        // prefab root the cooker added draws nothing, so it names no mesh.
        check(named == 15, "every entity that draws names a mesh");
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
        check(load_shipped(world, registry, library),
              "the shipped content loads");
        // The hand-authored crate, and the flight helmet the cooker wrote
        // from the glTF node tree.
        check(library.size() == 2, "both prefabs went into the library");

        // Four crate instances of two entities each, one beacon, and seven for
        // the flight helmet: the root the cooker added, and one for each of the
        // six nodes the model holds.
        check(world.size() == 16, "the scene holds sixteen entities");

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
        // The four crates and the flight helmet. The helmet is one instance
        // now rather than six hand-written entities, which is what the node
        // tree becoming a prefab bought.
        check(instances == 5, "five entities are prefab instances");
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

        const std::size_t restored =
            engine::assets::restore_references(saved, content.manifest());
        check(restored > 0, "saving puts references back");

        const std::string text = saved.dump();
        check(text.find("asset:models/crate/crate.gltf#mesh:0") != std::string::npos,
              "and the crate mesh reads as the path the source names");

        // The names the scene carries are ordinary strings and must come
        // through untouched.
        check(text.find("\"beacon\"") != std::string::npos, "an ordinary name is untouched");

        // What went back has to be what the cooker reads forward again, or the
        // save writes a file the next cook cannot resolve.
        for (const auto& entity : saved["entities"]) {
            for (const auto& part : entity.value("components", nlohmann::json::object())) {
                for (const auto& value : part) {
                    if (!value.is_string()) {
                        continue;
                    }
                    const auto text_value = value.get<std::string>();
                    if (!text_value.starts_with(engine::assets::kAssetPrefix)) {
                        continue;
                    }
                    engine::assets::AssetReference parsed;
                    check(engine::assets::parse_reference(text_value, parsed),
                          "every reference written back reads forward again");
                }
            }
        }
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
        check(load_shipped(world, registry, library),
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
    test_saving_puts_the_references_back();
    test_overrides_reach_the_world();
    std::printf("the game loop\n");
    test_update_turns_what_it_should();
    test_update_refuses_a_bad_spin();
    return test::report();
}
