#pragma once

/**
 * @file
 * @brief When one edit begins and when it ends.
 *
 * A gizmo drag writes a transform on every frame it moves, and an inspector
 * slider writes on every frame it is held. **Neither of those frames is an
 * edit.** The edit is the whole drag, and it is finished when the mouse comes
 * up. So the rule is to keep the value the interaction started from and to push
 * one entry when it ends. Nothing has to be merged or collapsed afterwards.
 *
 * See `DESIGN.md` §10 M12.
 *
 * This header names no ImGui type and no ImGuizmo type. The two edges arrive as
 * plain calls, so a test drives a whole interaction with no mouse and no
 * window.
 */

// core/entt.h points ENTT_ASSERT at ENGINE_ASSERT, and it has to come before any
// EnTT header. It fails the build with a message when the order is wrong.
#include "core/entt.h"
#include "core/guid.h"
#include "editor/history.h"

#include <entt/entity/fwd.hpp>
#include <nlohmann/json.hpp>

#include <string>
#include <string_view>

namespace engine::scene {
    class ComponentRegistry;
    class World;
} // namespace engine::scene

namespace engine::editor {

    /**
     * @brief One interaction in progress, and the value it started from.
     *
     * @code
     * engine::editor::Interaction drag;
     * drag.begin(world, entity, "Transform");   // the mouse went down
     * // ... frames of the gizmo writing the transform ...
     * drag.end(world, history);                 // the mouse came up
     * @endcode
     *
     * @warning **An interaction that ends where it started records nothing.**
     * A person who grabs a handle, moves it, and puts it back has not made an
     * edit, and an entry for it would cost them an undo that does nothing.
     */
    class Interaction {
    public:
        /**
         * @brief Remembers what a component holds, before an edit changes it.
         *
         * Reads the component out of the world, so the caller does not have to
         * hold a document of its own.
         *
         * Beginning again while an interaction is open drops the open one and
         * reports it. That is a caller which lost an end, and keeping the older
         * value would put the wrong one on the next entry.
         *
         * @param world The world holding the entity.
         * @param entity The entity being edited.
         * @param component The component name, as a scene file stores it.
         * @param types Where to look the component name up. The process-wide
         * registry by default.
         * @return True when there is a value to go back to. False when the
         * entity or the component is not there, and then nothing is open.
         */
        bool begin(const scene::World& world, entt::entity entity, std::string_view component,
                   const scene::ComponentRegistry* types = nullptr);

        /**
         * @brief Closes the interaction and records what changed.
         *
         * @param world The world holding the entity, read for the value now.
         * @param history Where the entry goes.
         * @return True when an entry was recorded. False when nothing was open,
         * when the entity has gone, or when the value came back to where it
         * started.
         */
        bool end(const scene::World& world, History& history);

        /**
         * @brief Drops the interaction without recording anything.
         *
         * What a caller does when the entity goes away under it, or when the
         * selection changes part way through.
         */
        void cancel();

        /// @brief Whether an interaction is open.
        /// @return True between begin() and end().
        [[nodiscard]] bool active() const { return active_; }

        /// @brief What the open interaction is editing, for a caller to compare.
        /// @return The entity identity, or a null one when nothing is open.
        [[nodiscard]] Guid entity() const { return entity_; }

        /// @brief The component the open interaction is editing.
        /// @return The name, or an empty one when nothing is open.
        [[nodiscard]] const std::string& component() const { return component_; }

    private:
        bool active_ = false;
        Guid entity_;
        std::string component_;
        nlohmann::json before_;
        const scene::ComponentRegistry* types_ = nullptr;
    };

} // namespace engine::editor
