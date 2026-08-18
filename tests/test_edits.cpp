// M12.3 tests for the component edits.
//
// The stack itself is settled in test_history.cpp. What is checked here is that
// each edit puts the world back: applied, reverted, and applied again, with the
// values it had rather than the defaults.
//
// No window and no device. Every edit is a document and an identity, so all of
// it runs on a bare World.

#include "check.h"
#include "editor/edits.h"
#include "editor/history.h"
#include "scene/component_registry.h"
#include "scene/components.h"
#include "scene/document.h"
#include "scene/prefab.h"
#include "scene/scene_file.h"
#include "scene/world.h"

#include <string>
#include <vector>

namespace {

    using test::check;
    using test::section;

    namespace ed = engine::editor;
    namespace sc = engine::scene;

    sc::ComponentRegistry make_registry() {
        sc::ComponentRegistry registry;
        sc::register_builtin_components(registry);
        return registry;
    }

    /// A world holding one entity that carries a Name.
    struct Fixture {
        sc::ComponentRegistry types = make_registry();
        sc::World world;
        entt::entity entity;
        engine::Guid id;

        Fixture()
            : entity(world.create()) {
            world.registry().emplace<sc::Name>(entity, sc::Name{ "crate" });
            id = world.identity(entity);
        }

        /// What the named component saves right now, or null when it has none.
        [[nodiscard]] nlohmann::json state(const std::string& component) const {
            const sc::ComponentOps* ops = types.find(component);
            if (ops == nullptr || !ops->has(world.registry(), entity)) {
                return {};
            }
            return ops->save(world.registry(), entity);
        }
    };

    void test_a_changed_field_goes_back_and_forward() {
        section("a component whose fields changed");

        Fixture fixture;
        const nlohmann::json before = fixture.state("Name");

        fixture.world.registry().get<sc::Name>(fixture.entity).value = "barrel";
        const nlohmann::json after = fixture.state("Name");

        ed::History history;
        history.record(ed::component_changed(fixture.id, "Name", before, after, &fixture.types));
        check(std::string(history.undo_name()) == "change Name",
              "the menu can say what it would undo");

        check(history.undo(fixture.world), "undo runs");
        check(fixture.world.registry().get<sc::Name>(fixture.entity).value == "crate",
              "and the old value is back");

        check(history.redo(fixture.world), "redo runs");
        check(fixture.world.registry().get<sc::Name>(fixture.entity).value == "barrel",
              "and the new value is back");
    }

    void test_an_added_component_goes_away_and_comes_back() {
        section("a component somebody added");

        Fixture fixture;
        const sc::ComponentOps* ops = fixture.types.find("PointLight");
        check(ops != nullptr, "the registry holds PointLight");
        if (ops == nullptr) {
            return;
        }

        ops->create(fixture.world.registry(), fixture.entity);
        ed::History history;
        history.record(ed::component_added(fixture.id, "PointLight", fixture.state("PointLight"),
                                           &fixture.types));
        check(std::string(history.undo_name()) == "add PointLight", "the menu says what it is");

        check(history.undo(fixture.world), "undo runs");
        check(!ops->has(fixture.world.registry(), fixture.entity), "and the component is gone");

        check(history.redo(fixture.world), "redo runs");
        check(ops->has(fixture.world.registry(), fixture.entity), "and it is back");
    }

    /**
     * A removed component comes back with what it held, not with the defaults.
     *
     * This is the one the issue calls out. Bringing back a default component
     * looks like it worked, and quietly throws away every value somebody set.
     */
    void test_a_removed_component_keeps_its_values() {
        section("a component somebody removed");

        Fixture fixture;
        const sc::ComponentOps* ops = fixture.types.find("PointLight");
        check(ops != nullptr, "the registry holds PointLight");
        if (ops == nullptr) {
            return;
        }

        ops->create(fixture.world.registry(), fixture.entity);
        auto& light = fixture.world.registry().get<sc::PointLight>(fixture.entity);
        const float wanted = light.range + 7.0F;
        light.range = wanted;

        const nlohmann::json removed = fixture.state("PointLight");
        ops->remove(fixture.world.registry(), fixture.entity);

        ed::History history;
        history.record(ed::component_removed(fixture.id, "PointLight", removed, &fixture.types));
        check(std::string(history.undo_name()) == "remove PointLight",
              "the menu says what it is");

        check(history.undo(fixture.world), "undo runs");
        check(ops->has(fixture.world.registry(), fixture.entity), "the component is back");
        check(fixture.world.registry().get<sc::PointLight>(fixture.entity).range == wanted,
              "and it kept the value it had, rather than the default");

        check(history.redo(fixture.world), "redo runs");
        check(!ops->has(fixture.world.registry(), fixture.entity), "and it is gone again");
    }

    /**
     * An edit reaches its entity after that entity has been built again.
     *
     * This is what the identity is for. The entity is destroyed and made again,
     * so EnTT hands out a different slot, and the edit still lands.
     */
    void test_an_edit_survives_the_entity_being_rebuilt() {
        section("an edit outliving the entity number");

        Fixture fixture;
        const nlohmann::json before = fixture.state("Name");
        fixture.world.registry().get<sc::Name>(fixture.entity).value = "barrel";
        const nlohmann::json after = fixture.state("Name");

        ed::History history;
        history.record(ed::component_changed(fixture.id, "Name", before, after, &fixture.types));

        // Destroy it and build a decoy first, so the new entity is unlikely to
        // land on the old number and a test that passes by luck is less likely.
        fixture.world.destroy(fixture.entity);
        const entt::entity decoy = fixture.world.create();
        fixture.world.registry().emplace<sc::Name>(decoy, sc::Name{ "decoy" });

        const entt::entity again = fixture.world.create();
        fixture.world.registry().emplace<sc::Name>(again, sc::Name{ "barrel" });
        check(fixture.world.set_identity(again, fixture.id), "the entity takes its identity back");

        check(history.undo(fixture.world), "undo runs");
        check(fixture.world.registry().get<sc::Name>(again).value == "crate",
              "and it reached the entity through its identity");
        check(fixture.world.registry().get<sc::Name>(decoy).value == "decoy",
              "and it left the decoy alone");
    }

    /**
     * A Transform edit marks the subtree, or the world matrices stay stale.
     *
     * The write goes through the registry rather than through set_local, so
     * nothing marks it dirty on its own. Without this an undo moves the entity
     * and the picture does not follow until something else moves.
     */
    void test_an_undone_transform_rebuilds_the_matrices() {
        section("a Transform going back");

        Fixture fixture;
        const entt::entity child = fixture.world.create();
        check(fixture.world.set_parent(child, fixture.entity), "a child attaches");

        const nlohmann::json before = fixture.state("Transform");
        fixture.world.set_local(fixture.entity,
                                engine::Transform{ .position = { 5.0F, 0.0F, 0.0F } });
        const nlohmann::json after = fixture.state("Transform");

        fixture.world.update();
        check(fixture.world.world_matrix(child)[3][0] == 5.0F, "the child followed the move");

        ed::History history;
        history.record(ed::component_changed(fixture.id, "Transform", before, after, &fixture.types));
        check(history.undo(fixture.world), "undo runs");

        fixture.world.update();
        check(fixture.world.world_matrix(fixture.entity)[3][0] == 0.0F, "the entity went back");
        check(fixture.world.world_matrix(child)[3][0] == 0.0F, "and so did the child");
    }


    /// A crate with a lid on it, so an instance has a member to delete.
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

    /// The child of an entity that carries a given name, or entt::null.
    entt::entity child_named(const sc::World& world, entt::entity parent,
                             const char* wanted) {
        const auto& registry = world.registry();
        for (entt::entity walk = registry.get<sc::Hierarchy>(parent).first_child;
             walk != entt::null; walk = registry.get<sc::Hierarchy>(walk).next_sibling) {
            const auto* name = registry.try_get<sc::Name>(walk);
            if (name != nullptr && name->value == wanted) {
                return walk;
            }
        }
        return entt::null;
    }

    /// A named entity under a parent, so a test can build a sibling list.
    entt::entity named_child(sc::World& world, entt::entity parent, const char* name) {
        const entt::entity entity = world.create();
        world.registry().emplace<sc::Name>(entity, sc::Name{ name });
        check(world.set_parent(entity, parent), "the child attaches");
        return entity;
    }

    /**
     * A deleted entity comes back whole, and the scene reads as it did.
     *
     * The check is the whole scene document, byte for byte. A weaker one passes
     * while the subtree comes back at the end of its sibling list, which is the
     * failure that matters: every entity is there and the file has changed.
     */
    void test_a_deleted_entity_comes_back() {
        sc::ComponentRegistry types = make_registry();
        sc::PrefabLibrary library;

        sc::World world;
        const entt::entity root = world.create();
        world.registry().emplace<sc::Name>(root, sc::Name{ "root" });
        (void)named_child(world, root, "first");
        const entt::entity middle = named_child(world, root, "middle");
        (void)named_child(world, root, "last");

        // Children of its own, so the delete takes a subtree rather than one.
        const entt::entity kid = named_child(world, middle, "kid");
        check(kid != entt::null, "the subtree has a child in it");

        const nlohmann::json before = sc::save_scene(world, types, library);

        ed::History history;
        auto edit = ed::entity_deleted(world, middle, &types, &library);
        check(edit != nullptr, "the delete records");
        check(std::string(edit->name()) == "delete middle",
              "and the menu can say what it would undo");

        world.destroy(middle);
        history.record(std::move(edit));
        check(sc::save_scene(world, types, library) != before, "the delete changed the scene");

        check(history.undo(world), "undo runs");
        check(sc::save_scene(world, types, library) == before,
              "and the scene reads as it did, sibling order included");

        check(history.redo(world), "redo runs");
        check(sc::save_scene(world, types, library) != before, "and the delete is back");
    }

    /**
     * An edit still names its entity after that entity has come back.
     *
     * This is the box on the milestone. Undo builds the entity again with the
     * identity it had, so an entry recorded before the delete still reaches it.
     */
    void test_an_edit_outlives_a_delete_and_an_undo() {
        sc::ComponentRegistry types = make_registry();
        sc::PrefabLibrary library;

        sc::World world;
        const entt::entity root = world.create();
        world.registry().emplace<sc::Name>(root, sc::Name{ "root" });
        const entt::entity crate = named_child(world, root, "crate");
        const engine::Guid crate_id = world.identity(crate);

        // A component edit recorded first, then the delete on top of it.
        const sc::ComponentOps* ops = types.find("Name");
        check(ops != nullptr, "the registry holds Name");
        if (ops == nullptr) {
            return;
        }
        const nlohmann::json was = ops->save(world.registry(), crate);
        world.registry().get<sc::Name>(crate).value = "barrel";
        const nlohmann::json now = ops->save(world.registry(), crate);

        ed::History history;
        history.record(ed::component_changed(crate_id, "Name", was, now, &types));

        auto edit = ed::entity_deleted(world, crate, &types, &library);
        check(edit != nullptr, "the delete records");
        world.destroy(crate);
        history.record(std::move(edit));

        // EnTT hands the number out again, so something else takes it first.
        const entt::entity filler = world.create();
        check(filler != entt::null, "another entity takes the free number");

        check(history.undo(world), "the delete is undone");
        const entt::entity back = world.find(crate_id);
        check(back != entt::null, "the entity answers to its identity again");
        check(back != filler, "and it is not the entity that took its number");
        check(world.registry().get<sc::Name>(back).value == "barrel",
              "it comes back with the value it had");

        check(history.undo(world), "the component edit under it still runs");
        check(world.registry().get<sc::Name>(world.find(crate_id)).value == "crate",
              "and it reached the entity that was rebuilt");
    }

    /// A created entity goes away and comes back, which is what a drop needs.
    void test_a_created_entity_goes_away_and_comes_back() {
        sc::ComponentRegistry types = make_registry();
        const sc::PrefabLibrary library = crate_library();
        const sc::Prefab* crate = library.find("crate");
        check(crate != nullptr, "the library holds the crate");
        if (crate == nullptr) {
            return;
        }

        sc::World world;
        const nlohmann::json empty = sc::save_scene(world, types, library);

        const entt::entity instance = sc::instantiate(world, *crate, {}, types);
        check(instance != entt::null, "the crate instances");
        const engine::Guid id = world.identity(instance);

        ed::History history;
        history.record(ed::entity_created(world, instance, &types, &library));
        check(std::string(history.undo_name()) == "create crate", "the menu says what it is");

        const nlohmann::json dropped = sc::save_scene(world, types, library);

        check(history.undo(world), "undo runs");
        check(sc::save_scene(world, types, library) == empty, "and the world is empty again");

        check(history.redo(world), "redo runs");
        check(sc::save_scene(world, types, library) == dropped,
              "and the instance is back, an instance");
        check(world.find(id) != entt::null, "with the identity it had");
    }

    /**
     * A deleted prefab member comes back a member, not a loose entity.
     *
     * The instance records what it lost, so a delete gives the scene a removed
     * list. Undo has to take that list away again.
     */
    void test_a_deleted_member_comes_back_a_member() {
        sc::ComponentRegistry types = make_registry();
        const sc::PrefabLibrary library = crate_library();
        const sc::Prefab* crate = library.find("crate");
        check(crate != nullptr, "the library holds the crate");
        if (crate == nullptr) {
            return;
        }

        sc::World world;
        const entt::entity instance = sc::instantiate(world, *crate, {}, types);
        check(instance != entt::null, "the crate instances");
        const entt::entity lid = child_named(world, instance, "lid");
        check(lid != entt::null, "the instance has a lid");
        if (lid == entt::null) {
            return;
        }

        const nlohmann::json before = sc::save_scene(world, types, library);

        ed::History history;
        auto edit = ed::entity_deleted(world, lid, &types, &library);
        check(edit != nullptr, "the delete records");
        world.destroy(lid);
        history.record(std::move(edit));

        const nlohmann::json without = sc::save_scene(world, types, library);
        check(without.at(sc::kEntitiesKey).at(0).contains(sc::kRemovedKey),
              "the instance records the member it lost");

        check(history.undo(world), "undo runs");
        check(sc::save_scene(world, types, library) == before,
              "and the instance is whole again, with no removed list");
    }

    /// An entity moved to another parent goes back to the place it had.
    void test_a_reparented_entity_goes_back() {
        sc::ComponentRegistry types = make_registry();
        sc::PrefabLibrary library;

        sc::World world;
        const entt::entity first = world.create();
        world.registry().emplace<sc::Name>(first, sc::Name{ "first" });
        const entt::entity second = world.create();
        world.registry().emplace<sc::Name>(second, sc::Name{ "second" });

        (void)named_child(world, first, "a");
        const entt::entity mover = named_child(world, first, "mover");
        (void)named_child(world, first, "c");
        (void)named_child(world, second, "d");

        const nlohmann::json before = sc::save_scene(world, types, library);

        // Read where it hangs, move it, then record. The same order a gizmo
        // drag uses for a transform.
        const ed::Place from = ed::place_of(world, mover);
        check(from.parent == world.identity(first), "it hangs under the first parent");
        check(from.before.valid(), "and it sits in front of a sibling");

        check(world.set_parent(mover, second), "the move runs");
        ed::History history;
        history.record(ed::entity_reparented(world, mover, from));
        check(std::string(history.undo_name()) == "move mover", "the menu says what moved");

        const nlohmann::json moved = sc::save_scene(world, types, library);
        check(moved != before, "the move changed the scene");

        check(history.undo(world), "undo runs");
        check(sc::save_scene(world, types, library) == before,
              "and it is back under the parent it had, in the place it had");

        check(history.redo(world), "redo runs");
        check(sc::save_scene(world, types, library) == moved, "and the move is back");
    }

    /// What the factories do with an entity that is not in the world.
    void test_a_missing_entity_records_nothing() {
        sc::ComponentRegistry types = make_registry();
        sc::PrefabLibrary library;

        sc::World world;
        check(ed::entity_deleted(world, entt::null, &types, &library) == nullptr,
              "a delete of nothing records nothing");
        check(ed::entity_created(world, entt::null, &types, &library) == nullptr,
              "a create of nothing records nothing");
        check(ed::entity_reparented(world, entt::null, {}) == nullptr,
              "a move of nothing records nothing");

        const ed::Place nowhere = ed::place_of(world, entt::null);
        check(!nowhere.parent.valid() && !nowhere.before.valid(),
              "and it hangs nowhere");
    }

} // namespace

int main() {
    std::printf("component edits\n");
    test_a_changed_field_goes_back_and_forward();
    test_an_added_component_goes_away_and_comes_back();
    test_a_removed_component_keeps_its_values();
    std::printf("identity and side effects\n");
    test_an_edit_survives_the_entity_being_rebuilt();
    test_an_undone_transform_rebuilds_the_matrices();
    std::printf("entity edits\n");
    test_a_deleted_entity_comes_back();
    test_an_edit_outlives_a_delete_and_an_undo();
    test_a_created_entity_goes_away_and_comes_back();
    test_a_deleted_member_comes_back_a_member();
    std::printf("the parent link\n");
    test_a_reparented_entity_goes_back();
    std::printf("bad input\n");
    test_a_missing_entity_records_nothing();
    return test::report();
}
