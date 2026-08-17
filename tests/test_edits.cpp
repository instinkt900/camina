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
#include "scene/world.h"

#include <string>

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

} // namespace

int main() {
    std::printf("component edits\n");
    test_a_changed_field_goes_back_and_forward();
    test_an_added_component_goes_away_and_comes_back();
    test_a_removed_component_keeps_its_values();
    std::printf("identity and side effects\n");
    test_an_edit_survives_the_entity_being_rebuilt();
    test_an_undone_transform_rebuilds_the_matrices();
    return test::report();
}
