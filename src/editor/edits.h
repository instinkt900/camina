#pragma once

/**
 * @file
 * @brief The undoable edits to a component.
 *
 * One class covers all three, because a change, an add and a remove are the
 * same thing seen from different sides: a document for what the entity carried
 * before, and a document for what it carries after. A null document means the
 * entity does not carry the component at all, so an add is null to something
 * and a remove is something to null. See `DESIGN.md` §10 M12.
 *
 * This header names no ImGui type. What a person clicks is `apps/editor/`, and
 * the edits underneath are driven by a test with no window.
 */

#include "core/guid.h"
#include "editor/history.h"

#include <nlohmann/json.hpp>

#include <memory>
#include <string>
#include <string_view>

namespace engine::scene {
    class ComponentRegistry;
    class World;
} // namespace engine::scene

namespace engine::editor {

    /**
     * @brief A component that was added, changed, or taken off an entity.
     *
     * **The entity is named by identity, never by `entt::entity`.** A slot
     * number is handed out again after the entity holding it is destroyed, so
     * an edit that outlived one delete would put a component on whoever took
     * the number. `scene::World::find` is the way back. See `scene::Id`.
     *
     * The component is named by the name a scene file stores, not by a pointer
     * into the registry. Registering another component type grows that vector
     * and moves every entry, so a held pointer would be a dangling one on the
     * next call to `ComponentRegistry::add`.
     */
    class ComponentEdit : public Edit {
    public:
        /**
         * @brief Records a component going from one state to another.
         *
         * @param entity The identity of the entity the component sits on.
         * @param component The component name, as a scene file stores it.
         * @param before What the entity carried, or null when it carried none.
         * @param after What it carries now, or null when it now carries none.
         * @param types Where to look the component name up. The process-wide
         * registry by default, which is what the editor uses.
         */
        ComponentEdit(Guid entity, std::string_view component, nlohmann::json before,
                      nlohmann::json after, const scene::ComponentRegistry* types = nullptr);

        /// @brief Puts the component into the state it had after the edit.
        /// @param world The world to change.
        void apply(scene::World& world) override;

        /// @brief Puts the component back into the state it had before the edit.
        /// @param world The world to change.
        void revert(scene::World& world) override;

        /// @brief What the change was, for the menu.
        /// @return "add", "remove" or "change", and the component name.
        [[nodiscard]] const char* name() const override { return label_.c_str(); }

    private:
        /// Puts the component into @p state, where a null state removes it.
        void put(scene::World& world, const nlohmann::json& state);

        Guid entity_;
        std::string component_;
        nlohmann::json before_;
        nlohmann::json after_;
        const scene::ComponentRegistry* types_ = nullptr;
        std::string label_;
    };

    /**
     * @brief Records a component whose fields somebody changed.
     *
     * @param entity The identity of the entity it sits on.
     * @param component The component name.
     * @param before The document the component saved before the change.
     * @param after The document it saves now.
     * @param types Where to look the component name up, or null for the
     * process-wide registry.
     * @return The edit, ready to hand to `History::record`.
     */
    [[nodiscard]] std::unique_ptr<Edit> component_changed(
        Guid entity, std::string_view component, nlohmann::json before, nlohmann::json after,
        const scene::ComponentRegistry* types = nullptr);

    /**
     * @brief Records a component somebody put on an entity.
     *
     * The document is what the component saves once it is there, rather than
     * nothing, so a redo brings back the same values. A component added with
     * its defaults and then edited is two entries, and redoing the first one
     * must not carry the second one's values with it.
     *
     * @param entity The identity of the entity it went on.
     * @param component The component name.
     * @param added The document the new component saves.
     * @param types Where to look the component name up, or null for the
     * process-wide registry.
     * @return The edit.
     */
    [[nodiscard]] std::unique_ptr<Edit> component_added(
        Guid entity, std::string_view component, nlohmann::json added,
        const scene::ComponentRegistry* types = nullptr);

    /**
     * @brief Records a component somebody took off an entity.
     *
     * @param entity The identity of the entity it came off.
     * @param component The component name.
     * @param removed The document it saved before it went, so undo brings back
     * the values it had rather than the defaults.
     * @param types Where to look the component name up, or null for the
     * process-wide registry.
     * @return The edit.
     */
    [[nodiscard]] std::unique_ptr<Edit> component_removed(
        Guid entity, std::string_view component, nlohmann::json removed,
        const scene::ComponentRegistry* types = nullptr);

} // namespace engine::editor
