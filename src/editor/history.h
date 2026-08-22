#pragma once

/**
 * @file
 * @brief The undo stack, and what one undoable change looks like.
 *
 * **Transactional rather than a snapshot of the world.** Each edit records what
 * it changed and how to put it back, so a step costs what the edit cost rather
 * than what the level costs. A stack of whole-world documents was measured and
 * rejected: on the sandbox scene, which is 43 entities on purpose, a document is
 * 7.2 KiB and a step 0.62 ms, and that number says nothing about a level worth
 * building. See `DESIGN.md` §10 M12.
 *
 * This header names no ImGui type. What a person clicks is `apps/editor/`, and
 * the stack underneath it is driven by a test with no window at all.
 */

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace engine::scene {
    class World;
}

namespace engine::editor {

    /**
     * @brief One change to a world that can be put back.
     *
     * **The caller makes the change and then records it.** A gizmo drag has
     * already moved the entity by the time the mouse comes up, and a menu action
     * is simpler to write as "do it, then say what was done". So `apply` is the
     * redo path rather than the first run.
     *
     * That leaves one rule an implementation has to keep: **`apply` must land
     * where the original change landed**. An edit whose redo differs from what a
     * person did is worse than no undo, because it moves something they were not
     * looking at.
     */
    class Edit {
    public:
        Edit() = default;

        Edit(const Edit&) = delete;
        Edit& operator=(const Edit&) = delete;
        Edit(Edit&&) = delete;
        Edit& operator=(Edit&&) = delete;

        virtual ~Edit() = default;

        /// @brief Does the change again, on redo.
        /// @param world The world to change.
        virtual void apply(scene::World& world) = 0;

        /// @brief Puts the world back as it was before the change.
        /// @param world The world to change.
        virtual void revert(scene::World& world) = 0;

        /**
         * @brief What the change was, for the menu to show.
         *
         * "Undo" alone is a stack a person cannot read. Every entry says what it
         * is, so the menu can say "Undo move crate" and somebody can tell how
         * far back they are about to go.
         *
         * @return A short name that lives as long as the edit does. A literal,
         * or a string the edit holds. It cannot be a temporary: the menu reads
         * it while the edit sits on the stack, and an edit that names the
         * component it changed has to build that text rather than quote it.
         */
        [[nodiscard]] virtual const char* name() const = 0;

        /**
         * @brief Whether this edit still fits the world in front of it.
         *
         * An edit names its entity by `engine::Guid`, and a rebuild of the
         * world is what can take that entity away. A scene the editor saved
         * carries every identity, so a reload gives the same entities back and
         * every entry still reaches its own. A scene that carries none does
         * not, and then each entry names something that is not there.
         *
         * **It is not only "the entity is present".** A delete expects its
         * entity to be absent, and a create expects it to be present. An edit
         * whose expectation the world no longer meets is stale whichever way it
         * disagrees, so each implementation answers for its own shape.
         *
         * @param world The world after the rebuild.
         * @return True when an undo and a redo of this edit would still reach
         * what they name.
         */
        [[nodiscard]] virtual bool fits(const scene::World& world) const = 0;
    };

    /// @brief How many edits a history keeps before it drops the oldest.
    inline constexpr std::size_t kDefaultHistoryDepth = 128;

    /**
     * @brief The edits made to a world, and the way back through them.
     *
     * @code
     * engine::editor::History history;
     * move_the_entity();                       // the change happens first
     * history.record(std::make_unique<MoveEdit>(entity, before, after));
     * history.undo(world);                     // and this puts it back
     * @endcode
     */
    class History {
    public:
        /// @brief Builds a history that keeps @p depth edits.
        /// @param depth How many to keep. Zero is refused and reads as one.
        explicit History(std::size_t depth = kDefaultHistoryDepth);

        /**
         * @brief Takes an edit that has already happened.
         *
         * **Recording drops whatever was ahead.** A person who undoes three
         * steps and then edits has chosen a new future, and every editor throws
         * the old one away at that point.
         *
         * The oldest edit goes when the history is full, so a long session
         * costs a bounded amount rather than everything it ever did.
         *
         * @param edit What was done. A null edit is ignored.
         */
        void record(std::unique_ptr<Edit> edit);

        /**
         * @brief Puts the last edit back.
         * @param world The world to change.
         * @return False when there was nothing to undo.
         */
        bool undo(scene::World& world);

        /**
         * @brief Does again the edit that undo last put back.
         * @param world The world to change.
         * @return False when there was nothing to redo.
         */
        bool redo(scene::World& world);

        /// @brief Whether there is an edit to undo.
        /// @return True when undo() would do something.
        [[nodiscard]] bool can_undo() const { return next_ > 0; }

        /// @brief Whether there is an edit to redo.
        /// @return True when redo() would do something.
        [[nodiscard]] bool can_redo() const { return next_ < edits_.size(); }

        /**
         * @brief Whether every edit still fits @p world.
         *
         * The editor asks this after a reload rebuilt the world. A stack whose
         * entries name entities that are gone is worse than an empty one: each
         * undo writes an error and nothing moves. See issue #371.
         *
         * Both directions are checked, because a redo is as much a promise as
         * an undo.
         *
         * @param world The world after the rebuild.
         * @return True when every entry still reaches what it names, and for an
         * empty history.
         */
        [[nodiscard]] bool fits(const scene::World& world) const;

        /// @brief What undo would put back, or null when it would do nothing.
        /// @return The name of that edit.
        [[nodiscard]] const char* undo_name() const;

        /// @brief What redo would do again, or null when it would do nothing.
        /// @return The name of that edit.
        [[nodiscard]] const char* redo_name() const;

        /**
         * @brief Forgets everything.
         *
         * A scene that is loaded again builds every entity from nothing, so
         * every edit on the stack names something that no longer exists. The
         * history goes with it. See `DESIGN.md` §10 M12 for what does not: a
         * delete keeps its entity, and a play session has to keep the stack.
         */
        void clear();

        /// @brief How many edits are held, on both sides of the position.
        /// @return The count.
        [[nodiscard]] std::size_t size() const { return edits_.size(); }

    private:
        /// The edits, oldest first. Everything from next_ on is a redo.
        std::vector<std::unique_ptr<Edit>> edits_;

        /// Where the person is: the number of edits that have been applied.
        std::size_t next_ = 0;

        /// How many to keep.
        std::size_t depth_ = kDefaultHistoryDepth;
    };

    /**
     * @brief What the Edit menu shows, and whether either half can be clicked.
     *
     * The rule lives here rather than beside the ImGui calls, so a test settles
     * it with no window. See `DESIGN.md` §10 M12.
     */
    struct UndoMenu {
        /// @brief What the undo item says, for example "Undo move crate".
        std::string undo_label;
        /// @brief What the redo item says.
        std::string redo_label;
        /// @brief Whether the undo item can be clicked.
        bool can_undo = false;
        /// @brief Whether the redo item can be clicked.
        bool can_redo = false;
    };

    /**
     * @brief Works out what the Edit menu should show right now.
     *
     * **Both halves are off while a session runs.** The world under a session
     * is a game part way through a step, and every entry on the stack belongs
     * to the scene somebody authored. Undoing into a running game would move an
     * entity the simulation owns, and a stop throws that world away and reads
     * the snapshot back, so the change would not survive the session either.
     * The save button is already off for the same reason.
     *
     * The labels name what the entry is, because "Undo" on its own is a stack
     * a person cannot read.
     *
     * @param history The stack to read.
     * @param session_running True while a play session is playing or paused.
     * @return What to draw.
     */
    [[nodiscard]] UndoMenu undo_menu(const History& history, bool session_running);

} // namespace engine::editor
