// M12.1 tests for the undo stack.
//
// The stack is driven here with an edit that does nothing but count its own
// calls, so the ordering rules are checked before a single real edit exists.
// Every real edit then has one job: put the world back. The rules about what
// happens to the stack are settled here and only here.

#include "check.h"
#include "editor/history.h"
#include "scene/world.h"

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace {

    using test::check;
    using test::section;

    /// Where the edits write, so a test can read the order they ran in.
    struct Trace {
        std::vector<std::string> calls;
    };

    /// An edit that changes nothing and says what was asked of it.
    class CountingEdit final : public engine::editor::Edit {
    public:
        CountingEdit(Trace& trace, std::string label)
            : trace_(&trace)
            , label_(std::move(label)) {}

        void apply(engine::scene::World& /*world*/) override {
            trace_->calls.push_back("apply " + label_);
        }

        void revert(engine::scene::World& /*world*/) override {
            trace_->calls.push_back("revert " + label_);
        }

        [[nodiscard]] const char* name() const override { return label_.c_str(); }

    private:
        Trace* trace_;
        std::string label_;
    };

    [[nodiscard]] std::unique_ptr<engine::editor::Edit> edit(Trace& trace, std::string label) {
        return std::make_unique<CountingEdit>(trace, std::move(label));
    }

    void test_an_empty_history_does_nothing() {
        section("a history with nothing in it");

        engine::scene::World world;
        engine::editor::History history;

        check(!history.can_undo() && !history.can_redo(), "there is nothing to undo or redo");
        check(!history.undo(world) && !history.redo(world), "and asking does nothing");
        check(history.undo_name() == nullptr && history.redo_name() == nullptr,
              "so there is no name to show");
    }

    /**
     * Undo walks back through the edits, and redo walks forward.
     *
     * The order is the whole of it: the newest edit is the first to go, and the
     * one that comes back first is the one that went last.
     */
    void test_undo_and_redo_walk_the_stack() {
        section("walking back and forward");

        engine::scene::World world;
        engine::editor::History history;
        Trace trace;

        history.record(edit(trace, "first"));
        history.record(edit(trace, "second"));
        check(trace.calls.empty(), "recording does not run anything, because it already ran");
        check(history.size() == 2, "both are held");

        check(std::string{ history.undo_name() } == "second", "undo would put the newest back");
        check(history.redo_name() == nullptr, "and there is nothing ahead yet");

        check(history.undo(world) && history.undo(world), "both undo");
        check(trace.calls == std::vector<std::string>{ "revert second", "revert first" },
              "newest first");
        check(!history.can_undo(), "and now there is nothing left to undo");

        check(std::string{ history.redo_name() } == "first", "redo would do the oldest again");
        check(history.redo(world) && history.redo(world), "both redo");
        check(trace.calls == std::vector<std::string>{ "revert second", "revert first",
                                                       "apply first", "apply second" },
              "oldest first, which is the order they happened in");
    }

    /**
     * An edit made after an undo throws away what was ahead.
     *
     * Every editor does this. A person who steps back and then does something
     * else has chosen a new future, and keeping the old one would let a redo
     * apply an edit to a world it was never made against.
     */
    void test_recording_drops_the_redo_side() {
        section("editing after an undo");

        engine::scene::World world;
        engine::editor::History history;
        Trace trace;

        history.record(edit(trace, "first"));
        history.record(edit(trace, "second"));
        check(history.undo(world), "step back over the second");
        check(history.can_redo(), "which leaves it ahead");

        history.record(edit(trace, "third"));
        check(!history.can_redo(), "and recording drops it");
        check(history.size() == 2, "so the history holds the first and the third");
        check(std::string{ history.undo_name() } == "third", "with the third on top");

        check(history.undo(world), "undoing");
        check(std::string{ history.undo_name() } == "first",
              "reaches the first, and never the second again");
    }

    /// The oldest edit goes when the history is full, and the rest still work.
    void test_the_history_has_a_bottom() {
        section("a history that is full");

        engine::scene::World world;
        engine::editor::History history(3);
        Trace trace;

        for (int i = 0; i < 5; ++i) {
            history.record(edit(trace, "edit " + std::to_string(i)));
        }
        check(history.size() == 3, "it keeps the three most recent");
        check(std::string{ history.undo_name() } == "edit 4", "the newest is on top");

        check(history.undo(world) && history.undo(world) && history.undo(world),
              "all three undo");
        check(!history.can_undo(), "and the ones that fell off cannot be undone");
        check(trace.calls == std::vector<std::string>{ "revert edit 4", "revert edit 3",
                                                       "revert edit 2" },
              "the three that were kept, newest first");

        // A depth of zero would be a history that drops what it is given, so it
        // reads as one rather than as nothing.
        engine::editor::History shallow(0);
        Trace other;
        shallow.record(edit(other, "only"));
        check(shallow.can_undo(), "a history of no depth still holds one edit");
    }

    /// Clearing forgets both sides, which is what a scene reload needs.
    void test_clearing_forgets_everything() {
        section("clearing the history");

        engine::scene::World world;
        engine::editor::History history;
        Trace trace;

        history.record(edit(trace, "first"));
        history.record(edit(trace, "second"));
        check(history.undo(world), "step back once");

        history.clear();
        check(history.size() == 0, "nothing is held");
        check(!history.can_undo() && !history.can_redo(), "and neither way goes anywhere");
        check(!history.undo(world) && !history.redo(world), "asking does nothing");
    }

} // namespace

/**
 * The Edit menu names what it will do, and goes off while a session runs.
 *
 * "Undo" on its own is a stack a person cannot read. And undo is off during
 * a play session because the world under one is a game part way through a
 * step: the entries belong to the scene somebody authored, and a stop reads
 * the snapshot back over anything an undo did anyway.
 */
void test_the_menu_says_what_it_will_do() {
    section("the Edit menu");

    Trace trace;
    engine::editor::History history;
    {
        const engine::editor::UndoMenu empty = engine::editor::undo_menu(history, false);
        check(empty.undo_label == "Undo", "an empty stack names nothing to undo");
        check(empty.redo_label == "Redo", "and nothing to redo");
        check(!empty.can_undo && !empty.can_redo, "and neither can be clicked");
    }

    engine::scene::World world;
    history.record(edit(trace, "move crate"));

    {
        const engine::editor::UndoMenu one = engine::editor::undo_menu(history, false);
        check(one.undo_label == "Undo move crate", "the entry is named");
        check(one.can_undo, "and it can be clicked");
        check(!one.can_redo, "with nothing ahead to redo");
    }

    check(history.undo(world), "undo runs");
    {
        const engine::editor::UndoMenu back = engine::editor::undo_menu(history, false);
        check(back.redo_label == "Redo move crate", "the redo names it now");
        check(back.can_redo && !back.can_undo, "and the two sides swapped");
    }

    // A session is running. Both halves go off, and neither is named,
    // because naming an entry nobody can click invites somebody to try.
    {
        const engine::editor::UndoMenu playing = engine::editor::undo_menu(history, true);
        check(!playing.can_undo && !playing.can_redo, "a session turns both off");
        check(playing.undo_label == "Undo" && playing.redo_label == "Redo",
              "and takes the names off with them");
    }
}

int main() {
    test_the_menu_says_what_it_will_do();
    test_an_empty_history_does_nothing();
    test_undo_and_redo_walk_the_stack();
    test_recording_drops_the_redo_side();
    test_the_history_has_a_bottom();
    test_clearing_forgets_everything();
    return test::report();
}
