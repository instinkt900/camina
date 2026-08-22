#include "editor/history.h"

#include "core/log.h"

#include <algorithm>
#include <utility>

namespace engine::editor {

    History::History(std::size_t depth)
        : depth_(std::max<std::size_t>(depth, 1)) {}

    void History::record(std::unique_ptr<Edit> edit) {
        if (!edit) {
            return;
        }

        // Whatever was ahead is a future the person did not take.
        edits_.erase(edits_.begin() + static_cast<std::ptrdiff_t>(next_), edits_.end());

        edits_.push_back(std::move(edit));
        if (edits_.size() > depth_) {
            // The oldest goes, so a long session costs a bounded amount. What
            // it did stays done: an edit that falls off the end is one nobody
            // can undo any more, not one that is put back.
            edits_.erase(edits_.begin());
        }
        next_ = edits_.size();
    }

    bool History::fits(const scene::World& world) const {
        // Every entry, not only the ones behind the cursor. A redo is as much a
        // promise as an undo, and a stack half of which cannot run is not one
        // worth keeping.
        return std::all_of(edits_.begin(), edits_.end(),
                           [&world](const std::unique_ptr<Edit>& edit) {
                               return edit == nullptr || edit->fits(world);
                           });
    }

    bool History::undo(scene::World& world) {
        if (!can_undo()) {
            return false;
        }
        --next_;
        edits_[next_]->revert(world);
        return true;
    }

    bool History::redo(scene::World& world) {
        if (!can_redo()) {
            return false;
        }
        edits_[next_]->apply(world);
        ++next_;
        return true;
    }

    const char* History::undo_name() const {
        return can_undo() ? edits_[next_ - 1]->name() : nullptr;
    }

    const char* History::redo_name() const { return can_redo() ? edits_[next_]->name() : nullptr; }

    void History::clear() {
        edits_.clear();
        next_ = 0;
    }

    UndoMenu undo_menu(const History& history, bool session_running) {
        UndoMenu menu{ .undo_label = "Undo", .redo_label = "Redo" };
        if (session_running) {
            // Off, and with no name either. Naming an entry that cannot be
            // clicked invites somebody to try.
            return menu;
        }

        menu.can_undo = history.can_undo();
        menu.can_redo = history.can_redo();
        if (const char* name = history.undo_name(); name != nullptr) {
            menu.undo_label += " ";
            menu.undo_label += name;
        }
        if (const char* name = history.redo_name(); name != nullptr) {
            menu.redo_label += " ";
            menu.redo_label += name;
        }
        return menu;
    }

} // namespace engine::editor
