// M3.2 tests. The scene file is the third consumer of the reflection
// descriptors, after the inspector and the field serializer.
//
// The check that matters is the round trip: save, load, save, and compare the
// two documents. It catches a field the writer drops, a link the reader
// rebuilds in the wrong order, and a sibling list that comes back reversed.

#include "check.h"
#include "math/transform.h"
#include "scene/component_registry.h"
#include "scene/components.h"
#include "scene/scene_file.h"
#include "scene/world.h"

#include <filesystem>
#include <string>
#include <vector>

namespace {

    using test::check;
    namespace sc = engine::scene;

    /// A component the engine does not define, so the registry holds three.
    struct Health {
        float current = 100.0F;
        float maximum = 100.0F;
    };

} // namespace

template <>
struct engine::reflect::Describe<Health> {
    static constexpr const char* name = "Health";
    static constexpr auto fields() {
        return std::make_tuple(ENGINE_FIELD(Health, current), ENGINE_FIELD(Health, maximum));
    }
};

namespace {

    sc::ComponentRegistry make_registry() {
        sc::ComponentRegistry registry;
        sc::register_builtin_components(registry);
        registry.add<Health>();
        return registry;
    }

    /// Builds a world worth saving: two roots, a chain, and a wide sibling list.
    entt::entity build_world(sc::World& world) {
        const entt::entity root = world.create();
        world.registry().emplace<sc::Name>(root, sc::Name{ "root" });
        world.set_local(root, { .position = { 1.0F, 2.0F, 3.0F } });

        const entt::entity middle = world.create();
        world.registry().emplace<sc::Name>(middle, sc::Name{ "middle" });
        check(world.set_parent(middle, root), "middle attaches");
        world.set_local(middle, { .scale = { 2.0F, 2.0F, 2.0F } });

        for (int i = 0; i < 3; ++i) {
            const entt::entity leaf = world.create();
            world.registry().emplace<sc::Name>(leaf, sc::Name{ "leaf" + std::to_string(i) });
            world.registry().emplace<Health>(leaf,
                                             Health{ .current = static_cast<float>(i), .maximum = 50.0F });
            check(world.set_parent(leaf, middle), "a leaf attaches");
        }

        // A second root, so the writer has to order more than one.
        const entt::entity other = world.create();
        world.registry().emplace<sc::Name>(other, sc::Name{ "second root" });
        return root;
    }

    std::vector<std::string> names_in_order(const sc::World& world) {
        // Hold the document in a local. Iterating a subobject of a temporary
        // leaves the range dangling, because the temporary dies at the end of
        // the full expression and only the range itself is extended.
        const nlohmann::json document = sc::save_scene(world, make_registry());

        std::vector<std::string> found;
        for (const auto& record : document.at("entities")) {
            found.push_back(record.at("components").at("Name").at("value").get<std::string>());
        }
        return found;
    }

    void test_registry() {
        sc::ComponentRegistry registry;
        check(registry.size() == 0, "a new component registry is empty");

        sc::register_builtin_components(registry);
        check(registry.size() == 2, "the engine registers Transform and Name");
        check(registry.find("Transform") != nullptr, "Transform is findable by name");
        check(registry.find("WorldTransform") == nullptr,
              "a derived component stays out of the file");
        check(registry.find("Hierarchy") == nullptr, "the parent link is not a component in the file");

        sc::register_builtin_components(registry);
        check(registry.size() == 2, "registering twice does nothing");

        registry.add<Health>();
        check(registry.size() == 3, "a game component joins the same registry");
        check(registry.find("Nothing") == nullptr, "an unknown name finds nothing");
    }

    void test_round_trip() {
        const sc::ComponentRegistry registry = make_registry();

        sc::World original;
        build_world(original);
        const nlohmann::json first = sc::save_scene(original, registry);

        sc::World loaded;
        check(sc::load_scene(first, loaded, registry), "the document loads");
        check(loaded.size() == original.size(), "every entity came back");

        const nlohmann::json second = sc::save_scene(loaded, registry);
        check(first == second, "saving what was loaded gives the same document");
    }

    void test_hierarchy_survives() {
        const sc::ComponentRegistry registry = make_registry();

        sc::World original;
        build_world(original);
        original.update();

        sc::World loaded;
        check(sc::load_scene(sc::save_scene(original, registry), loaded, registry),
              "the document loads");
        loaded.update();

        check(names_in_order(original) == names_in_order(loaded),
              "the entities come back in the same order");

        // The world matrices must agree, which they only do when the parent
        // links and the sibling order both survived.
        const std::vector<std::string> order = names_in_order(loaded);
        check(order.size() == 6, "the world holds six entities");
        check(order.at(0) == "root" && order.at(1) == "middle",
              "a parent still comes before its child");
        check(order.at(2) == "leaf0" && order.at(4) == "leaf2",
              "the sibling list did not reverse");
        check(order.at(5) == "second root", "the second root is still last");
    }

    void test_transforms_match() {
        const sc::ComponentRegistry registry = make_registry();

        sc::World original;
        const entt::entity root = build_world(original);
        original.update();
        const engine::Mat4 expected =
            original.world_matrix(original.registry().get<sc::Hierarchy>(root).first_child);

        sc::World loaded;
        check(sc::load_scene(sc::save_scene(original, registry), loaded, registry),
              "the document loads");
        loaded.update();

        // Find the same entity by walking the same path.
        entt::entity loaded_root = entt::null;
        for (const auto [entity, node] : loaded.registry().view<const sc::Hierarchy>().each()) {
            if (node.parent == entt::null &&
                loaded.registry().get<sc::Name>(entity).value == "root") {
                loaded_root = entity;
            }
        }
        check(loaded_root != entt::null, "the root came back");

        const engine::Mat4 actual =
            loaded.world_matrix(loaded.registry().get<sc::Hierarchy>(loaded_root).first_child);
        check(expected == actual, "a child ends up at the same place after a round trip");
    }

    void test_dirty_after_load() {
        const sc::ComponentRegistry registry = make_registry();

        sc::World original;
        build_world(original);

        sc::World loaded;
        check(sc::load_scene(sc::save_scene(original, registry), loaded, registry),
              "the document loads");

        // The components arrived through the registry, not through set_local(),
        // so load_scene has to mark them itself. Without that the matrices
        // would stay at the identity and nothing would look wrong until a
        // camera moved.
        loaded.update();
        check(loaded.rebuilt_last_update() == loaded.size(),
              "a freshly loaded world builds every matrix");
        loaded.update();
        check(loaded.rebuilt_last_update() == 0, "and settles on the next frame");
    }

    void test_unknown_component_is_a_warning() {
        sc::ComponentRegistry full = make_registry();
        sc::World original;
        build_world(original);
        const nlohmann::json document = sc::save_scene(original, full);

        // A build that never heard of Health must still open the file.
        sc::ComponentRegistry partial;
        sc::register_builtin_components(partial);

        sc::World loaded;
        check(sc::load_scene(document, loaded, partial), "an unknown component does not fail");
        check(loaded.size() == original.size(), "every entity still loaded");

        std::size_t named = 0;
        for (const auto entity : loaded.registry().view<const sc::Name>()) {
            (void)entity;
            ++named;
        }
        check(named == original.size(), "the components it does know all arrived");
    }

    void test_bad_documents() {
        const sc::ComponentRegistry registry = make_registry();

        sc::World world;
        check(!sc::load_scene(nlohmann::json::array(), world, registry),
              "an array is not a scene");

        nlohmann::json no_version = nlohmann::json::object();
        no_version["entities"] = nlohmann::json::array();
        check(!sc::load_scene(no_version, world, registry), "a scene must carry a version");

        nlohmann::json future = nlohmann::json::object();
        future["__version"] = sc::kSceneVersion + 1;
        future["entities"] = nlohmann::json::array();
        check(!sc::load_scene(future, world, registry), "a newer schema is refused");

        nlohmann::json no_entities = nlohmann::json::object();
        no_entities["__version"] = sc::kSceneVersion;
        check(!sc::load_scene(no_entities, world, registry), "a scene must carry an entity array");

        nlohmann::json bad_parent = nlohmann::json::object();
        bad_parent["__version"] = sc::kSceneVersion;
        bad_parent["entities"] = nlohmann::json::array({ { { "parent", 7 } } });
        sc::World fresh;
        check(!sc::load_scene(bad_parent, fresh, registry),
              "a parent index outside the file is refused");

        // A world that already holds entities must not take a second scene.
        sc::World busy;
        build_world(busy);
        check(!sc::load_scene(sc::save_scene(busy, registry), busy, registry),
              "load_scene refuses a world that is not empty");
    }

    void test_file_round_trip() {
        const sc::ComponentRegistry registry = make_registry();
        const std::filesystem::path path =
            std::filesystem::temp_directory_path() / "camina_test.scene";
        std::filesystem::remove(path);

        sc::World original;
        build_world(original);
        check(sc::save_scene_file(path, original, registry), "the scene writes to disk");
        check(std::filesystem::exists(path), "the file is there");

        sc::World loaded;
        check(sc::load_scene_file(path, loaded, registry), "the scene reads back");
        check(sc::save_scene(original, registry) == sc::save_scene(loaded, registry),
              "the file made the trip unchanged");

        std::filesystem::remove(path);

        sc::World missing;
        check(!sc::load_scene_file(path, missing, registry),
              "a file that is not there is reported");
    }

} // namespace

int main() {
    std::printf("component registry\n");
    test_registry();
    std::printf("round trip\n");
    test_round_trip();
    test_hierarchy_survives();
    test_transforms_match();
    test_dirty_after_load();
    std::printf("tolerance\n");
    test_unknown_component_is_a_warning();
    std::printf("bad input\n");
    test_bad_documents();
    std::printf("files\n");
    test_file_round_trip();
    return test::report();
}
