// M3.3 tests. A prefab is a scene fragment, and an instance keeps only the
// fields it changed.
//
// The check that carries the milestone is test_prefab_edit_reaches_instances().
// It changes the prefab under two instances and shows that the one which
// overrode a field keeps that field, and takes every other change.

#include "check.h"
#include "math/transform.h"
#include "reflect/json.h"
#include "scene/component_registry.h"
#include "scene/components.h"
#include "scene/prefab.h"
#include "scene/scene_file.h"
#include "scene/world.h"

#include <algorithm>
#include <string>
#include <vector>

namespace {

    using test::check;
    namespace sc = engine::scene;

    sc::ComponentRegistry make_registry() {
        sc::ComponentRegistry registry;
        sc::register_builtin_components(registry);
        return registry;
    }

    /// A crate with a lid on it: one root, one child, two components each.
    nlohmann::json crate_document(const std::string& crate_name, float crate_scale) {
        nlohmann::json root = nlohmann::json::object();
        root["parent"] = -1;
        root["components"]["Name"] = engine::reflect::to_json(sc::Name{ crate_name });
        root["components"]["Transform"] = engine::reflect::to_json(
            engine::Transform{ .scale = { crate_scale, crate_scale, crate_scale } });

        nlohmann::json lid = nlohmann::json::object();
        lid["parent"] = 0;
        lid["components"]["Name"] = engine::reflect::to_json(sc::Name{ "lid" });
        lid["components"]["Transform"] =
            engine::reflect::to_json(engine::Transform{ .position = { 0.0F, 1.0F, 0.0F } });

        nlohmann::json document = nlohmann::json::object();
        document["__version"] = sc::kPrefabVersion;
        document["entities"] = nlohmann::json::array({ root, lid });
        return document;
    }

    sc::PrefabLibrary crate_library(const std::string& crate_name = "crate",
                                    float crate_scale = 1.0F) {
        sc::PrefabLibrary library;
        check(library.add("crate", crate_document(crate_name, crate_scale)),
              "the crate prefab parses");
        return library;
    }

    /// The one override this suite uses: move the instance root sideways.
    nlohmann::json moved_to(float x) {
        nlohmann::json patch = nlohmann::json::object();
        patch["0"]["Transform"]["position"] = nlohmann::json::array({ x, 0.0F, 0.0F });
        return patch;
    }

    void test_parse() {
        sc::Prefab prefab;
        check(sc::Prefab::parse("crate", crate_document("crate", 1.0F), prefab),
              "a good document parses");
        check(prefab.name() == "crate", "the prefab keeps its name");
        check(prefab.size() == 2, "the prefab holds two entities");
        check(prefab.entities().at(0).parent == -1, "entity 0 is the root");
        check(prefab.entities().at(1).parent == 0, "entity 1 hangs off the root");

        check(!sc::Prefab::parse("bad", nlohmann::json::array(), prefab),
              "an array is not a prefab");

        nlohmann::json no_version = crate_document("crate", 1.0F);
        no_version.erase("__version");
        check(!sc::Prefab::parse("bad", no_version, prefab), "a prefab must carry a version");

        nlohmann::json future = crate_document("crate", 1.0F);
        future["__version"] = sc::kPrefabVersion + 1;
        check(!sc::Prefab::parse("bad", future, prefab), "a newer schema is refused");

        nlohmann::json no_entities = nlohmann::json::object();
        no_entities["__version"] = sc::kPrefabVersion;
        check(!sc::Prefab::parse("bad", no_entities, prefab),
              "a prefab must carry an entity array");

        nlohmann::json empty = no_entities;
        empty["entities"] = nlohmann::json::array();
        check(!sc::Prefab::parse("bad", empty, prefab), "a prefab needs a root");

        nlohmann::json parented_root = crate_document("crate", 1.0F);
        parented_root["entities"][0]["parent"] = 1;
        check(!sc::Prefab::parse("bad", parented_root, prefab),
              "the first entity must be the root");

        nlohmann::json second_root = crate_document("crate", 1.0F);
        second_root["entities"][1]["parent"] = -1;
        check(!sc::Prefab::parse("bad", second_root, prefab), "a prefab holds one root only");

        nlohmann::json forward = crate_document("crate", 1.0F);
        forward["entities"][1]["parent"] = 5;
        check(!sc::Prefab::parse("bad", forward, prefab),
              "a parent must be an earlier entity");

        nlohmann::json bad_entity = crate_document("crate", 1.0F);
        bad_entity["entities"][1] = 7;
        check(!sc::Prefab::parse("bad", bad_entity, prefab), "an entity must be an object");

        nlohmann::json bad_parts = crate_document("crate", 1.0F);
        bad_parts["entities"][0]["components"] = 7;
        check(!sc::Prefab::parse("bad", bad_parts, prefab), "components must be an object");
    }

    void test_library() {
        sc::PrefabLibrary library;
        check(library.size() == 0, "a new library is empty");
        check(library.find("crate") == nullptr, "an empty library finds nothing");

        check(library.add("crate", crate_document("crate", 1.0F)), "a prefab goes in");
        check(library.size() == 1, "the library holds one prefab");
        check(library.find("crate") != nullptr, "the prefab is findable by name");

        // A second prefab under the same name replaces the first, so a reload
        // needs no remove call.
        check(library.add("crate", crate_document("heavy crate", 1.0F)), "a reload goes in");
        check(library.size() == 1, "a reload replaces rather than adds");
        check(library.find("crate")->entities().at(0).components["Name"]["value"] ==
                  "heavy crate",
              "the reload won");

        check(!library.add("broken", nlohmann::json::array()), "a bad document is refused");
        check(library.size() == 1, "a refused document leaves the library alone");

        library.clear();
        check(library.size() == 0, "clear empties the library");
    }

    void test_override_patch() {
        const nlohmann::json base = { { "a", 1 }, { "b", 2 } };

        check(sc::override_patch(base, base).empty(), "two equal documents need no patch");

        const nlohmann::json changed = { { "a", 1 }, { "b", 9 } };
        const nlohmann::json patch = sc::override_patch(base, changed);
        check(patch.size() == 1 && patch["b"] == 9, "a patch names only the key that changed");

        const nlohmann::json added = { { "a", 1 }, { "b", 2 }, { "c", 3 } };
        check(sc::override_patch(base, added) == nlohmann::json{ { "c", 3 } },
              "a new key goes into the patch");

        const nlohmann::json dropped = { { "a", 1 } };
        const nlohmann::json removal = sc::override_patch(base, dropped);
        check(removal.size() == 1 && removal["b"].is_null(),
              "a dropped key becomes a null, which a merge patch reads as remove");

        // The walk goes into a nested object, so two components that differ in
        // one field give a patch that names that one field.
        const nlohmann::json outer = { { "T", { { "x", 1 }, { "y", 2 } } } };
        const nlohmann::json moved = { { "T", { { "x", 1 }, { "y", 5 } } } };
        const nlohmann::json deep = sc::override_patch(outer, moved);
        check(deep == nlohmann::json{ { "T", { { "y", 5 } } } },
              "a nested change gives a nested patch");

        // An array is a value. Half a position is not a useful override.
        const nlohmann::json list = { { "p", { 1, 2, 3 } } };
        const nlohmann::json other = { { "p", { 1, 9, 3 } } };
        check(sc::override_patch(list, other) == nlohmann::json{ { "p", { 1, 9, 3 } } },
              "an array goes into the patch whole");
    }

    void test_instantiate() {
        const sc::ComponentRegistry registry = make_registry();
        const sc::PrefabLibrary library = crate_library();
        const sc::Prefab& crate = *library.find("crate");

        sc::World world;
        const entt::entity root = sc::instantiate(world, crate, nlohmann::json::object(),
                                                  registry);
        check(root != entt::null, "the instance builds");
        check(world.size() == 2, "the instance created both entities");

        const entt::registry& entities = world.registry();
        check(entities.all_of<sc::PrefabInstance>(root), "the root carries the link");
        check(entities.get<sc::PrefabInstance>(root).prefab == "crate", "the link names the prefab");
        check(entities.get<sc::Name>(root).value == "crate", "the root took its name");

        const entt::entity lid = entities.get<sc::Hierarchy>(root).first_child;
        check(lid != entt::null, "the child is attached");
        check(!entities.all_of<sc::PrefabInstance>(lid), "only the root carries the link");
        check(entities.get<sc::Name>(lid).value == "lid", "the child took its name");
        check(entities.get<sc::PrefabMember>(lid).root == root, "the child points at its root");
        check(entities.get<sc::PrefabMember>(lid).index == 1, "the child knows its index");
        check(entities.get<sc::PrefabMember>(root).index == 0, "the root knows its index");

        // The prefab drives the world matrix, so the child ends up above the root.
        world.update();
        check(world.world_matrix(lid)[3][1] == 1.0F, "the child sits where the prefab put it");
    }

    void test_overrides_are_per_field() {
        const sc::ComponentRegistry registry = make_registry();
        const sc::PrefabLibrary library = crate_library("crate", 3.0F);
        const sc::Prefab& crate = *library.find("crate");

        sc::World world;
        const entt::entity root = sc::instantiate(world, crate, moved_to(7.0F), registry);
        check(root != entt::null, "an overriding instance builds");

        const engine::Transform& local = world.local(root);
        check(local.position.x == 7.0F, "the overridden field took the new value");
        check(local.scale.x == 3.0F, "the field next to it still comes from the prefab");
        check(world.registry().get<sc::Name>(root).value == "crate",
              "a component the patch did not name still comes from the prefab");

        // Reading the overrides back must name that one field and nothing else.
        const nlohmann::json found = sc::instance_overrides(world, root, crate, registry);
        check(found == moved_to(7.0F), "the instance reports the one field it changed");

        sc::World clean;
        const entt::entity plain =
            sc::instantiate(clean, crate, nlohmann::json::object(), registry);
        check(sc::instance_overrides(clean, plain, crate, registry).empty(),
              "an instance that changed nothing reports nothing");
    }

    void test_instantiate_refuses_bad_input() {
        const sc::ComponentRegistry registry = make_registry();
        const sc::PrefabLibrary library = crate_library();
        const sc::Prefab& crate = *library.find("crate");

        sc::World world;
        check(sc::instantiate(world, crate, nlohmann::json::array(), registry) == entt::null,
              "overrides that are not an object are refused");
        check(world.size() == 0, "a refused build leaves nothing behind");

        // A patch that puts the wrong type in a field fails the component read.
        nlohmann::json wrong = nlohmann::json::object();
        wrong["0"]["Transform"]["position"] = "over there";
        check(sc::instantiate(world, crate, wrong, registry) == entt::null,
              "a field of the wrong type fails the build");
        check(world.size() == 0, "a failed build destroys the whole instance");
    }

    void test_unknown_component_is_a_warning() {
        // A build that never heard of a component in the prefab must still
        // instantiate it, in the same way a scene file tolerates one.
        nlohmann::json document = crate_document("crate", 1.0F);
        document["entities"][0]["components"]["Health"] = { { "current", 5 } };

        sc::PrefabLibrary library;
        check(library.add("crate", document), "the prefab parses");

        sc::World world;
        const entt::entity root = sc::instantiate(world, *library.find("crate"),
                                                  nlohmann::json::object(), make_registry());
        check(root != entt::null, "an unknown component does not fail the build");
        check(world.registry().get<sc::Name>(root).value == "crate",
              "the components it does know all arrived");
    }

    /// Builds a scene of two crates. The second one is moved, the first is not.
    nlohmann::json two_crates(const sc::ComponentRegistry& registry,
                              const sc::PrefabLibrary& library) {
        sc::World world;
        const sc::Prefab& crate = *library.find("crate");

        check(sc::instantiate(world, crate, nlohmann::json::object(), registry) != entt::null,
              "the plain crate builds");
        check(sc::instantiate(world, crate, moved_to(5.0F), registry) != entt::null,
              "the moved crate builds");

        return sc::save_scene(world, registry, library);
    }

    /// The instance roots of a loaded scene, in file order.
    std::vector<entt::entity> instance_roots(const sc::World& world) {
        std::vector<entt::entity> found;
        for (const auto [entity, link] : world.registry().view<const sc::PrefabInstance>().each()) {
            (void)link;
            found.push_back(entity);
        }
        std::sort(found.begin(), found.end(), [](entt::entity left, entt::entity right) {
            return entt::to_integral(left) < entt::to_integral(right);
        });
        return found;
    }

    void test_scene_collapses_an_instance() {
        const sc::ComponentRegistry registry = make_registry();
        const sc::PrefabLibrary library = crate_library();
        const nlohmann::json document = two_crates(registry, library);

        check(document["__version"] == sc::kSceneVersion, "the scene carries version 2");
        check(document["entities"].size() == 2,
              "two instances of a two-entity prefab give two records, not four");

        const nlohmann::json& plain = document["entities"][0];
        check(plain["prefab"] == "crate", "a record names its prefab");
        check(!plain.contains("components"),
              "an instance stores no components of its own");
        check(!plain.contains("overrides"),
              "an instance that changed nothing stores no overrides");

        check(document["entities"][1]["overrides"] == moved_to(5.0F),
              "the moved instance stores the one field it changed");
    }

    void test_scene_round_trip() {
        const sc::ComponentRegistry registry = make_registry();
        const sc::PrefabLibrary library = crate_library();
        const nlohmann::json first = two_crates(registry, library);

        sc::World loaded;
        check(sc::load_scene(first, loaded, registry, library), "the scene loads");
        check(loaded.size() == 4, "both instances expanded again");

        const nlohmann::json second = sc::save_scene(loaded, registry, library);
        check(first == second, "saving what was loaded gives the same document");

        const std::vector<entt::entity> roots = instance_roots(loaded);
        check(roots.size() == 2, "the world holds two instances");
        check(loaded.local(roots.at(0)).position.x == 0.0F, "the plain crate is at the origin");
        check(loaded.local(roots.at(1)).position.x == 5.0F, "the moved crate kept its override");
    }

    void test_prefab_edit_reaches_instances() {
        const sc::ComponentRegistry registry = make_registry();

        // Save a scene against the first version of the prefab.
        const sc::PrefabLibrary before = crate_library("crate", 1.0F);
        const nlohmann::json document = two_crates(registry, before);

        // Now the prefab changes: a new name and a new scale. Neither instance
        // ever named those fields.
        const sc::PrefabLibrary after = crate_library("reinforced crate", 4.0F);

        sc::World loaded;
        check(sc::load_scene(document, loaded, registry, after),
              "the same scene loads against the edited prefab");

        const std::vector<entt::entity> roots = instance_roots(loaded);
        check(roots.size() == 2, "both instances came back");

        for (const entt::entity root : roots) {
            check(loaded.registry().get<sc::Name>(root).value == "reinforced crate",
                  "an instance took the new name from the prefab");
            check(loaded.local(root).scale.x == 4.0F,
                  "an instance took the new scale from the prefab");
        }

        check(loaded.local(roots.at(0)).position.x == 0.0F,
              "the instance that overrode nothing follows the prefab");
        check(loaded.local(roots.at(1)).position.x == 5.0F,
              "the instance that overrode a field keeps that field");
    }

    void test_missing_prefab() {
        const sc::ComponentRegistry registry = make_registry();
        const sc::PrefabLibrary library = crate_library();
        const nlohmann::json document = two_crates(registry, library);

        // A build with no prefabs cannot invent the entities.
        const sc::PrefabLibrary empty;
        sc::World loaded;
        check(!sc::load_scene(document, loaded, registry, empty),
              "a scene that names a prefab the library does not hold is an error");
        check(loaded.size() == 2, "the reader still left one entity for each record");

        // Saving an instance without its prefab writes the entities one by one,
        // rather than throwing away every field the instance holds.
        sc::World world;
        check(sc::instantiate(world, *library.find("crate"), moved_to(9.0F), registry) !=
                  entt::null,
              "the instance builds");
        const nlohmann::json expanded = sc::save_scene(world, registry, empty);
        check(expanded["entities"].size() == 2, "the instance was written entity by entity");
        check(!expanded["entities"][0].contains("prefab"), "the link is gone");
        check(expanded["entities"][0]["components"]["Transform"]["position"][0] == 9.0F,
              "the fields the instance held all survived");
    }

    void test_version_one_still_loads() {
        const sc::ComponentRegistry registry = make_registry();

        // A scene written before prefabs existed. No record names one.
        nlohmann::json old = nlohmann::json::object();
        old["__version"] = 1;
        nlohmann::json record = nlohmann::json::object();
        record["parent"] = -1;
        record["components"]["Name"] = engine::reflect::to_json(sc::Name{ "lonely" });
        old["entities"] = nlohmann::json::array({ record });

        sc::World world;
        check(sc::load_scene(old, world, registry, sc::PrefabLibrary{}),
              "a version 1 scene still reads");
        check(world.size() == 1, "the entity arrived");
    }

} // namespace

int main() {
    std::printf("parsing\n");
    test_parse();
    test_library();
    std::printf("overrides\n");
    test_override_patch();
    std::printf("instances\n");
    test_instantiate();
    test_overrides_are_per_field();
    test_instantiate_refuses_bad_input();
    test_unknown_component_is_a_warning();
    std::printf("scene files\n");
    test_scene_collapses_an_instance();
    test_scene_round_trip();
    test_prefab_edit_reaches_instances();
    std::printf("tolerance\n");
    test_missing_prefab();
    test_version_one_still_loads();
    return test::report();
}
