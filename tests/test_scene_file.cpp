// M3.2 tests. The scene file is the third consumer of the reflection
// descriptors, after the inspector and the field serializer.
//
// The check that matters is the round trip: save, load, save, and compare the
// two documents. It catches a field the writer drops, a link the reader
// rebuilds in the wrong order, and a sibling list that comes back reversed.

#include "check.h"
#include "math/transform.h"
#include "scene/component_registry.h"
#include "scene/document.h"
#include "scene/components.h"
#include "scene/prefab.h"
#include "scene/scene_file.h"
#include "scene/world.h"

#include <cstddef>
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


    /// A crate with a lid on it, so a fragment has a prefab to collapse.
    sc::PrefabLibrary crate_library() {
        nlohmann::json root = nlohmann::json::object();
        root["parent"] = sc::kNoParent;
        root["components"]["Name"] = engine::reflect::to_json(sc::Name{ "crate" });

        nlohmann::json lid = nlohmann::json::object();
        lid["parent"] = 0;
        lid["components"]["Name"] = engine::reflect::to_json(sc::Name{ "lid" });

        nlohmann::json document = nlohmann::json::object();
        document[sc::kVersionKey] = sc::kPrefabVersion;
        document[sc::kEntitiesKey] = nlohmann::json::array({ root, lid });

        sc::PrefabLibrary library;
        check(library.add("crate", document), "the crate prefab parses");
        return library;
    }

    /// The one child of an entity that carries the given name.
    entt::entity child_named(const sc::World& world, entt::entity parent,
                             const std::string& wanted) {
        const auto& registry = world.registry();
        for (entt::entity walk = registry.get<sc::Hierarchy>(parent).first_child;
             walk != entt::null; walk = registry.get<sc::Hierarchy>(walk).next_sibling) {
            const auto* name = registry.try_get<sc::Name>(walk);
            if (name != nullptr && name->value == wanted) {
                return walk;
            }
        }
        check(false, "the world holds the child the test asked for");
        return entt::null;
    }

    /**
     * The prefab entity index one fragment record links to.
     *
     * `at()` throws on a key that is not there, which ends the process and
     * takes every later result with it. A missing link is a result, so it comes
     * back as a number the caller can compare.
     *
     * @return The index, or -1 when the record carries no link.
     */
    int member_index_of(const nlohmann::json& record) {
        const auto link = record.find(sc::kMemberKey);
        if (link == record.end() || !link->is_object()) {
            return -1;
        }
        const auto at = link->find(sc::kIndexKey);
        if (at == link->end() || !at->is_number_unsigned()) {
            return -1;
        }
        return at->get<int>();
    }

    /**
     * A subtree comes back where it was, and the scene reads as it did.
     *
     * The check is the whole scene document, byte for byte. A weaker check
     * passes while the subtree comes back at the end of its sibling list, and
     * that is exactly the failure this has to catch: the entities are all there
     * and the file has changed.
     */
    void test_a_subtree_goes_back_where_it_was() {
        const sc::ComponentRegistry registry = make_registry();

        sc::World world;
        const entt::entity root = build_world(world);
        const entt::entity middle = child_named(world, root, "middle");
        const entt::entity leaf = child_named(world, middle, "leaf1");

        const nlohmann::json before = sc::save_scene(world, registry);

        // The middle of a sibling list, which is the case that has a position
        // to lose. The last one would pass whatever the writer did.
        const nlohmann::json fragment = sc::save_subtree(world, leaf, registry);
        check(fragment.contains(sc::kUnderKey), "the fragment says what it hung under");
        check(fragment.contains(sc::kBeforeKey), "and which sibling it sat in front of");

        world.destroy(leaf);
        check(sc::save_scene(world, registry) != before, "deleting it changes the scene");

        check(sc::load_subtree(fragment, world, registry) != entt::null, "the fragment loads");
        check(sc::save_scene(world, registry) == before,
              "and the scene reads as it did before the delete");
    }

    /// A subtree of several entities, each with its identity back.
    void test_a_deleted_subtree_keeps_its_identities() {
        const sc::ComponentRegistry registry = make_registry();

        sc::World world;
        const entt::entity root = build_world(world);
        const entt::entity middle = child_named(world, root, "middle");

        std::vector<engine::Guid> identities{ world.identity(middle) };
        for (int i = 0; i < 3; ++i) {
            identities.push_back(
                world.identity(child_named(world, middle, "leaf" + std::to_string(i))));
        }

        const nlohmann::json before = sc::save_scene(world, registry);
        const nlohmann::json fragment = sc::save_subtree(world, middle, registry);

        world.destroy(middle);
        for (const engine::Guid& id : identities) {
            check(world.find(id) == entt::null, "the delete took the identity with it");
        }

        // A fresh entity in between, so EnTT hands a recycled number to
        // something else and the restore cannot lean on the old numbers.
        const entt::entity filler = world.create();
        check(filler != entt::null, "an unrelated entity takes a number first");

        check(sc::load_subtree(fragment, world, registry) != entt::null, "the fragment loads");
        for (const engine::Guid& id : identities) {
            check(world.find(id) != entt::null, "and every identity answers again");
        }

        world.destroy(filler);
        check(sc::save_scene(world, registry) == before, "the scene reads as it did");
    }

    /// An instance comes back an instance, not four loose entities.
    void test_a_deleted_instance_comes_back_an_instance() {
        const sc::ComponentRegistry registry = make_registry();
        const sc::PrefabLibrary library = crate_library();
        const sc::Prefab* crate = library.find("crate");
        check(crate != nullptr, "the library holds the crate");
        if (crate == nullptr) {
            return;
        }

        sc::World world;
        const entt::entity root = world.create();
        world.registry().emplace<sc::Name>(root, sc::Name{ "root" });

        const entt::entity instance = sc::instantiate(world, *crate, {}, registry);
        check(instance != entt::null, "the crate instances");
        check(world.set_parent(instance, root), "and it attaches");

        const nlohmann::json before = sc::save_scene(world, registry, library);
        const nlohmann::json fragment = sc::save_subtree(world, instance, registry, library);
        check(fragment.at(sc::kEntitiesKey).size() == 1,
              "an instance collapses to one record, the lid included");

        world.destroy(instance);
        check(sc::load_subtree(fragment, world, registry, library) != entt::null,
              "the fragment loads");
        check(sc::save_scene(world, registry, library) == before,
              "and the scene holds the instance it held before");
    }

    /**
     * A deleted prefab member is not a deleted entity.
     *
     * The instance records what it lost, so the scene document gains a
     * "removed" list. Bringing the member back has to take that list away
     * again, which needs the link from the member to its instance.
     */
    void test_a_deleted_member_comes_back_a_member() {
        const sc::ComponentRegistry registry = make_registry();
        const sc::PrefabLibrary library = crate_library();
        const sc::Prefab* crate = library.find("crate");
        check(crate != nullptr, "the library holds the crate");
        if (crate == nullptr) {
            return;
        }

        sc::World world;
        const entt::entity instance = sc::instantiate(world, *crate, {}, registry);
        check(instance != entt::null, "the crate instances");
        const entt::entity lid = child_named(world, instance, "lid");

        const nlohmann::json before = sc::save_scene(world, registry, library);
        const nlohmann::json fragment = sc::save_subtree(world, lid, registry, library);
        check(member_index_of(fragment.at(sc::kEntitiesKey).at(0)) == 1,
              "the fragment carries the link back to the instance");

        world.destroy(lid);
        const nlohmann::json without = sc::save_scene(world, registry, library);
        check(without.at(sc::kEntitiesKey).at(0).contains(sc::kRemovedKey),
              "the instance records the member it lost");

        check(sc::load_subtree(fragment, world, registry, library) != entt::null,
              "the fragment loads");
        check(sc::save_scene(world, registry, library) == before,
              "and the instance is whole again, with no removed list");
    }

    /// A fragment whose parent has gone is refused, and changes nothing.
    void test_a_fragment_with_no_parent_is_refused() {
        const sc::ComponentRegistry registry = make_registry();

        sc::World world;
        const entt::entity root = build_world(world);
        const entt::entity middle = child_named(world, root, "middle");
        const nlohmann::json fragment = sc::save_subtree(world, middle, registry);

        world.destroy(root);
        const nlohmann::json before = sc::save_scene(world, registry);
        const std::size_t size = world.size();

        // Putting it at the root of the world instead would move it somewhere
        // nobody asked for, and nothing would say so.
        check(sc::load_subtree(fragment, world, registry) == entt::null,
              "a fragment whose parent has gone is refused");
        check(world.size() == size, "and nothing is left behind in the world");
        check(sc::save_scene(world, registry) == before, "so the scene did not change");
    }

    /// What save_subtree and load_subtree do with input they cannot use.
    void test_bad_fragments() {
        const sc::ComponentRegistry registry = make_registry();

        sc::World world;
        const entt::entity root = build_world(world);
        check(sc::save_subtree(world, entt::null, registry).is_null(),
              "a null entity writes no fragment");

        check(sc::load_subtree(nlohmann::json::object(), world, registry) == entt::null,
              "a document with no version is refused");

        nlohmann::json empty = nlohmann::json::object();
        empty[sc::kVersionKey] = sc::kSceneVersion;
        empty[sc::kEntitiesKey] = nlohmann::json::array();
        check(sc::load_subtree(empty, world, registry) == entt::null,
              "a fragment with no entity is refused");

        nlohmann::json orphan = sc::save_subtree(world, root, registry);
        orphan[sc::kBeforeKey] = to_text(world.identity(root));
        check(sc::load_subtree(orphan, world, registry) == entt::null,
              "a root that asks for a place among its siblings is refused");
    }

    void test_registry() {
        sc::ComponentRegistry registry;
        check(registry.size() == 0, "a new component registry is empty");

        sc::register_builtin_components(registry);
        check(registry.size() == 9,
              "the engine registers Transform, Name, MeshRenderer, the two lights, the "
              "camera, Environment, and the two audio components");
        check(registry.find("Transform") != nullptr, "Transform is findable by name");
        check(registry.find("WorldTransform") == nullptr,
              "a derived component stays out of the file");
        check(registry.find("Hierarchy") == nullptr, "the parent link is not a component in the file");

        sc::register_builtin_components(registry);
        check(registry.size() == 9, "registering twice does nothing");

        registry.add<Health>();
        check(registry.size() == 10, "a game component joins the same registry");
        check(registry.find("Nothing") == nullptr, "an unknown name finds nothing");

        // Registering a type has to wire every operation, not only the two the
        // scene file uses. The inspector reaches a component through the same
        // entry, and a null there would take the editor down on the first click.
        for (const sc::ComponentOps& ops : registry.all()) {
            check(ops.has != nullptr && ops.save != nullptr && ops.load != nullptr &&
                      ops.inspect != nullptr && ops.create != nullptr && ops.remove != nullptr,
                  "every registered type carries every operation");
        }
    }

    /**
     * A component can be put on an entity and taken off again by name.
     *
     * This is what the inspector's add and remove call. By name rather than by
     * type, because the editor holds a registry entry and never a C++ type: a
     * game component it has never heard of has to work the same way.
     */
    void test_add_and_remove_by_name() {
        const sc::ComponentRegistry registry = make_registry();
        sc::World world;
        const entt::entity entity = world.create();

        const sc::ComponentOps* health = registry.find("Health");
        check(health != nullptr, "the registry holds the game component");
        if (health == nullptr) {
            // Everything below reads through it, and a test that carries on
            // here reports a crash rather than the missing registration.
            return;
        }
        check(!health->has(world.registry(), entity), "a new entity carries none of it");

        health->create(world.registry(), entity);
        check(health->has(world.registry(), entity), "adding one puts it there");
        check(world.registry().get<Health>(entity).maximum == 100.0F,
              "and it arrives with the defaults its type declares");

        // An add over one that is already there keeps what somebody set. A
        // misclick must not be a way to lose values.
        world.registry().get<Health>(entity).current = 25.0F;
        health->create(world.registry(), entity);
        check(world.registry().get<Health>(entity).current == 25.0F,
              "adding it twice does not reset it");

        health->remove(world.registry(), entity);
        check(!health->has(world.registry(), entity), "removing it takes it off");
        health->remove(world.registry(), entity);
        check(!health->has(world.registry(), entity), "and removing it again does nothing");

        // Transform says so itself, which is how the inspector knows to refuse
        // it without comparing a name against a spelling.
        const sc::ComponentOps* transform = registry.find("Transform");
        check(transform != nullptr && transform->owns_transform,
              "Transform is marked as the one that moves an entity");
    }

    /**
     * An entity answers to the same identity after a save and a load.
     *
     * This is what an undo entry holds. An `entt::entity` is handed out again,
     * so an entry that held one would name whoever took it after a delete or a
     * scene reload. The identity is the thing that does not move.
     */
    void test_identity_survives_the_file() {
        const sc::ComponentRegistry registry = make_registry();

        sc::World original;
        const entt::entity root = build_world(original);
        const engine::Guid root_id = original.identity(root);
        check(root_id.valid(), "a created entity carries an identity");
        check(original.find(root_id) == root, "and the world finds it by that identity");

        sc::World loaded;
        check(sc::load_scene(sc::save_scene(original, registry), loaded, registry),
              "the document loads");

        const entt::entity same = loaded.find(root_id);
        check(same != entt::null, "the identity is in the world that was read back");
        check(loaded.registry().get<sc::Name>(same).value == "root",
              "and it names the same entity it did before");

        // Two worlds built by hand hold different identities, because each is
        // generated. Only a file carries one from a world to another.
        sc::World other;
        (void)build_world(other);
        check(other.find(root_id) == entt::null,
              "a different world does not answer to that identity");
    }

    /// A document from before version 4 has no identities, and still opens.
    void test_an_older_document_still_loads() {
        const sc::ComponentRegistry registry = make_registry();

        sc::World original;
        (void)build_world(original);
        nlohmann::json older = sc::save_scene(original, registry);
        older[engine::reflect::kVersionKey] = 3;
        for (auto& record : older.at(sc::kEntitiesKey)) {
            record.erase(sc::kIdKey);
        }

        sc::World loaded;
        check(sc::load_scene(older, loaded, registry), "a version 3 document still reads");
        check(loaded.size() == original.size(), "with every entity");

        // Every entity still has an identity. It is a new one, because the file
        // held none to give it.
        std::size_t named = 0;
        for (const auto [entity, unused] : loaded.registry().view<const sc::Hierarchy>().each()) {
            (void)unused;
            if (loaded.identity(entity).valid()) {
                ++named;
            }
        }
        check(named == loaded.size(), "and each one was given an identity as it was read");
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

    /**
     * An Environment names its cubemap by GUID, and that GUID has to survive a
     * round trip through the file.
     *
     * A component the registry never heard of loads as a warning and not as an
     * error, so a missing registration would leave the scene opening cleanly
     * with no environment in it. The picture would then fall back to grey and
     * nothing would say why.
     */
    void test_environment_round_trip() {
        const sc::ComponentRegistry registry = make_registry();
        engine::Guid cubemap;
        check(engine::Guid::parse("89a25488-04ab-401d-8fd5-3b7c78c13336", cubemap),
              "the test GUID parses");

        sc::World original;
        const entt::entity entity = original.create();
        original.registry().emplace<sc::Name>(entity, sc::Name{ .value = "environment" });
        original.registry().emplace<sc::Environment>(entity, sc::Environment{ .cubemap = cubemap });

        sc::World loaded;
        check(sc::load_scene(sc::save_scene(original, registry), loaded, registry),
              "a scene with an environment loads");

        std::size_t found = 0;
        for (const auto [at, environment] : loaded.registry().view<const sc::Environment>().each()) {
            ++found;
            check(environment.cubemap == cubemap, "the cubemap GUID came back unchanged");
        }
        check(found == 1, "the environment component survived the round trip");
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

        // nlohmann::json::value() converts to the type of the default, and it
        // throws when the stored type cannot convert. Nothing here catches a
        // JSON exception, so a hand-edited file with a worded parent used to end
        // the process rather than report.
        nlohmann::json worded_parent = nlohmann::json::object();
        worded_parent["__version"] = sc::kSceneVersion;
        worded_parent["entities"] = nlohmann::json::array({ { { "parent", "root" } } });
        sc::World unworded;
        check(!sc::load_scene(worded_parent, unworded, registry),
              "a parent that is not a number is refused rather than thrown");

        nlohmann::json null_parent = nlohmann::json::object();
        null_parent["__version"] = sc::kSceneVersion;
        null_parent["entities"] = nlohmann::json::array({ { { "parent", nullptr } } });
        sc::World unnulled;
        check(!sc::load_scene(null_parent, unnulled, registry), "a null parent is refused");

        // A world that already holds entities must not take a second scene.
        sc::World busy;
        build_world(busy);
        check(!sc::load_scene(sc::save_scene(busy, registry), busy, registry),
              "load_scene refuses a world that is not empty");
    }

    void test_file_round_trip() {
        const sc::ComponentRegistry registry = make_registry();
        const std::filesystem::path path =
            test::scratch("scene_file", "round_trip") / "camina_test.scene";

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
    test_add_and_remove_by_name();
    std::printf("round trip\n");
    test_identity_survives_the_file();
    test_an_older_document_still_loads();
    test_round_trip();
    test_hierarchy_survives();
    test_transforms_match();
    test_environment_round_trip();
    test_dirty_after_load();
    std::printf("tolerance\n");
    test_unknown_component_is_a_warning();
    std::printf("fragments\n");
    test_a_subtree_goes_back_where_it_was();
    test_a_deleted_subtree_keeps_its_identities();
    test_a_deleted_instance_comes_back_an_instance();
    test_a_deleted_member_comes_back_a_member();
    test_a_fragment_with_no_parent_is_refused();
    std::printf("bad input\n");
    test_bad_documents();
    test_bad_fragments();
    std::printf("files\n");
    test_file_round_trip();
    return test::report();
}
