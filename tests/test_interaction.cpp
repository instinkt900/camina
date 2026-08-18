// M12.4 tests. When one edit begins and when it ends.
//
// A gizmo drag writes a transform on every frame it moves, and an inspector
// slider writes on every frame it is held. None of those frames is an edit. The
// edit is the whole drag.
//
// The two edges arrive here as plain calls, so a whole interaction runs with no
// mouse, no window and no device. That is the point of Interaction sitting in
// src/editor/ rather than beside the ImGuizmo calls.

#include "check.h"
#include "editor/history.h"
#include "editor/interaction.h"
#include "scene/component_registry.h"
#include "scene/components.h"
#include "scene/world.h"

#include <cstdio>
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

    /// A world holding one entity with a Transform and a Name.
    struct Fixture {
        sc::ComponentRegistry types = make_registry();
        sc::World world;
        entt::entity entity;

        Fixture()
            : entity(world.create()) {
            world.registry().emplace<sc::Name>(entity, sc::Name{ "crate" });
        }

        /// Moves the entity along X, the way a frame of a drag does.
        void nudge(float x) {
            engine::Transform local = world.local(entity);
            local.position.x = x;
            world.set_local(entity, local);
        }

        [[nodiscard]] float x() const { return world.local(entity).position.x; }
    };

    /**
     * A drag of any length is one entry, and one undo covers the whole of it.
     *
     * The frames in the middle are what a weaker answer records. Ten of them
     * would be ten entries, and a person would press undo ten times to get back
     * to where they started.
     */
    void test_a_long_drag_is_one_entry() {
        section("a drag of many frames");

        Fixture fixture;
        ed::History history;
        ed::Interaction drag;

        check(drag.begin(fixture.world, fixture.entity, "Transform", &fixture.types),
              "the mouse goes down and the value is kept");
        check(drag.active(), "the interaction is open");

        // Ten frames of the handle moving. Each one writes the transform.
        for (int frame = 1; frame <= 10; ++frame) {
            fixture.nudge(static_cast<float>(frame));
        }
        check(history.size() == 0, "and not one of those frames recorded anything");

        check(drag.end(fixture.world, history), "the mouse comes up and one entry is pushed");
        check(!drag.active(), "the interaction is closed");
        check(history.size() == 1, "one drag is one entry, whatever it cost to make");

        check(fixture.x() == 10.0F, "the entity sits where the drag left it");
        check(history.undo(fixture.world), "undo runs");
        check(fixture.x() == 0.0F, "and one undo covers the whole drag");

        check(history.redo(fixture.world), "redo runs");
        check(fixture.x() == 10.0F, "and puts it back where the drag left it");
    }

    /// An interaction that ends where it started pushes nothing at all.
    void test_a_drag_that_goes_nowhere_records_nothing() {
        section("a drag that ends where it started");

        Fixture fixture;
        ed::History history;
        ed::Interaction drag;

        check(drag.begin(fixture.world, fixture.entity, "Transform", &fixture.types),
              "the mouse goes down");
        fixture.nudge(5.0F);
        fixture.nudge(-3.0F);
        fixture.nudge(0.0F);

        // Grabbed, moved, and put back. An entry for this would cost a person
        // an undo that does nothing, which is worse than no entry.
        check(!drag.end(fixture.world, history), "the end reports that it recorded nothing");
        check(history.size() == 0, "and the stack is empty");
        check(!history.can_undo(), "so there is nothing to undo");
    }

    /// A widget that changes in one go opens and closes on the same frame.
    void test_an_instant_change_is_one_entry() {
        section("a widget with no drag to hold");

        Fixture fixture;
        ed::History history;
        ed::Interaction edit;

        // What a checkbox or a drop-down does: both edges land on one frame.
        check(edit.begin(fixture.world, fixture.entity, "Name", &fixture.types),
              "the edit begins");
        fixture.world.registry().get<sc::Name>(fixture.entity).value = "barrel";
        check(edit.end(fixture.world, history), "and ends on the same frame");
        check(history.size() == 1, "one change is one entry");

        check(history.undo(fixture.world), "undo runs");
        check(fixture.world.registry().get<sc::Name>(fixture.entity).value == "crate",
              "and the old value is back");
    }

    /// Two interactions in a row are two entries, each undone on its own.
    void test_two_interactions_are_two_entries() {
        section("one interaction after another");

        Fixture fixture;
        ed::History history;
        ed::Interaction drag;

        check(drag.begin(fixture.world, fixture.entity, "Transform", &fixture.types), "one");
        fixture.nudge(2.0F);
        check(drag.end(fixture.world, history), "ends");

        check(drag.begin(fixture.world, fixture.entity, "Transform", &fixture.types), "two");
        fixture.nudge(7.0F);
        check(drag.end(fixture.world, history), "ends");

        check(history.size() == 2, "two drags are two entries");
        check(history.undo(fixture.world), "the first undo runs");
        check(fixture.x() == 2.0F, "and goes back one drag, not both");
        check(history.undo(fixture.world), "the second undo runs");
        check(fixture.x() == 0.0F, "and goes back the rest of the way");
    }

    /// An end with nothing open, and a begin that names nothing.
    void test_edges_that_do_not_pair() {
        section("edges that do not pair");

        Fixture fixture;
        ed::History history;
        ed::Interaction drag;

        check(!drag.end(fixture.world, history), "an end with nothing open records nothing");
        check(!drag.begin(fixture.world, entt::null, "Transform", &fixture.types),
              "a begin on no entity opens nothing");
        check(!drag.active(), "so nothing is open");
        check(!drag.begin(fixture.world, fixture.entity, "Nothing", &fixture.types),
              "a begin on an unregistered component opens nothing");
        check(!drag.begin(fixture.world, fixture.entity, "PointLight", &fixture.types),
              "a begin on a component the entity does not carry opens nothing");

        // A lost end. The newer value has to win, or the next entry carries a
        // before from an interaction nobody finished.
        check(drag.begin(fixture.world, fixture.entity, "Transform", &fixture.types),
              "one interaction opens");
        fixture.nudge(4.0F);
        check(drag.begin(fixture.world, fixture.entity, "Transform", &fixture.types),
              "and another opens over it");
        fixture.nudge(9.0F);
        check(drag.end(fixture.world, history), "the second one ends");

        check(history.undo(fixture.world), "undo runs");
        check(fixture.x() == 4.0F, "and goes back to where the second one began");
    }

    /// The entity going away part way through is not an error.
    void test_an_entity_that_goes_away_records_nothing() {
        section("the entity goes away under the drag");

        Fixture fixture;
        ed::History history;
        ed::Interaction drag;

        check(drag.begin(fixture.world, fixture.entity, "Transform", &fixture.types),
              "the drag opens");
        fixture.world.destroy(fixture.entity);

        check(!drag.end(fixture.world, history), "the end records nothing");
        check(!drag.active(), "and the interaction is closed rather than stuck open");
        check(history.size() == 0, "the stack is empty");
    }

    /// Cancel drops the interaction and leaves the stack alone.
    void test_cancel_records_nothing() {
        section("cancel");

        Fixture fixture;
        ed::History history;
        ed::Interaction drag;

        check(drag.begin(fixture.world, fixture.entity, "Transform", &fixture.types),
              "the drag opens");
        fixture.nudge(6.0F);
        drag.cancel();

        check(!drag.active(), "cancel closes it");
        check(!drag.end(fixture.world, history), "and there is nothing left to end");
        check(history.size() == 0, "so nothing was recorded");
        check(fixture.x() == 6.0F, "cancel does not put the value back, it only forgets");
    }

} // namespace

int main() {
    std::printf("when an edit begins and ends\n");
    test_a_long_drag_is_one_entry();
    test_a_drag_that_goes_nowhere_records_nothing();
    test_an_instant_change_is_one_entry();
    test_two_interactions_are_two_entries();
    std::printf("edges that do not pair\n");
    test_edges_that_do_not_pair();
    test_an_entity_that_goes_away_records_nothing();
    test_cancel_records_nothing();
    return test::report();
}
