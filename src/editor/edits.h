#pragma once

/**
 * @file
 * @brief The undoable edits to a component, an entity, and a parent link.
 *
 * One class covers the three component edits, because a change, an add and a
 * remove are the same thing seen from different sides: a document for what the
 * entity carried before, and a document for what it carries after. A null
 * document means the entity does not carry the component at all, so an add is
 * null to something and a remove is something to null.
 *
 * One class covers a delete and a create the same way. Both hold the subtree as
 * a fragment and differ only in which side of the edit the entity exists on. So
 * a prefab dropped on the viewport is the mirror of an entity deleted, and
 * neither needs code of its own. See `DESIGN.md` §10 M12.
 *
 * **Every factory here reads the world at the moment it is called.** A delete
 * has to be recorded before the entity goes, because afterwards there is
 * nothing left to save. That is the same rule `component_changed` already
 * carries for the document it takes as `before`.
 *
 * This header names no ImGui type. What a person clicks is `apps/editor/`, and
 * the edits underneath are driven by a test with no window.
 */

// core/entt.h points ENTT_ASSERT at ENGINE_ASSERT, and it has to come before any
// EnTT header. It fails the build with a message when the order is wrong.
#include "core/entt.h"
#include "core/guid.h"
#include "editor/history.h"

#include <entt/entity/fwd.hpp>

#include <nlohmann/json.hpp>

#include <memory>
#include <string>
#include <string_view>

namespace engine::scene {
    class ComponentRegistry;
    class PrefabLibrary;
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

        /// @brief Whether the entity this edit changes is still in @p world.
        /// @param world The world after a rebuild.
        /// @return True when the entity is there. Both directions of this edit
        /// need it, because both write a component onto it.
        [[nodiscard]] bool fits(const scene::World& world) const override;

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


    /**
     * @brief An entity that was created or deleted, with the subtree it took.
     *
     * A delete is the one edit that cannot be described by what changed,
     * because everything it took is gone. So this holds the whole subtree as a
     * fragment, which `scene::save_subtree` writes and `scene::load_subtree`
     * builds again. Every entity comes back with the identity it had, so every
     * other entry on the stack still resolves. See `DESIGN.md` §10 M12.
     *
     * A create is the same edit with the sides swapped. A prefab dropped on the
     * viewport is one fragment that exists after the edit and not before, and a
     * delete is one that exists before and not after.
     *
     * **A deleted prefab member is not a deleted entity**, and it needs no case
     * of its own here. The fragment carries the link back to the instance, so
     * putting the member back takes the instance's removed list away again.
     */
    class EntityEdit : public Edit {
    public:
        /**
         * @brief Records an entity going from one side of an edit to the other.
         *
         * @param entity The identity of the entity, which is the root of the
         * fragment.
         * @param fragment The subtree, from `scene::save_subtree`.
         * @param exists_after True for a create, false for a delete.
         * @param label What the menu shows, for example "delete crate".
         * @param types Where to look a component name up. The process-wide
         * registry by default.
         * @param library The prefabs to build an instance from. The
         * process-wide library by default.
         */
        EntityEdit(Guid entity, nlohmann::json fragment, bool exists_after, std::string label,
                   const scene::ComponentRegistry* types = nullptr,
                   const scene::PrefabLibrary* library = nullptr);

        /// @brief Puts the world into the state it had after the edit.
        /// @param world The world to change.
        void apply(scene::World& world) override;

        /// @brief Puts the world back into the state it had before the edit.
        /// @param world The world to change.
        void revert(scene::World& world) override;

        /// @brief What the change was, for the menu.
        /// @return "delete" or "create", and what it names.
        [[nodiscard]] const char* name() const override { return label_.c_str(); }

        /**
         * @brief Whether @p world holds what this edit expects it to.
         *
         * A create expects its entity to be there and a delete expects it to be
         * gone, so this asks for the state the edit last left behind rather
         * than for the entity. A rebuild that disagrees either way has replaced
         * the world this edit was recorded against.
         *
         * @param world The world after a rebuild.
         * @return True when the world matches.
         */
        [[nodiscard]] bool fits(const scene::World& world) const override;

    private:
        /// Builds the subtree again, or destroys it.
        void put(scene::World& world, bool exists);

        Guid entity_;
        nlohmann::json fragment_;
        bool exists_after_ = false;
        const scene::ComponentRegistry* types_ = nullptr;
        const scene::PrefabLibrary* library_ = nullptr;
        std::string label_;
    };

    /**
     * @brief Records an entity that is about to be deleted.
     *
     * @warning **Call this before the delete, not after.** It reads the subtree
     * out of the world, and after the delete there is nothing left to read. The
     * caller then destroys the entity and records what comes back.
     *
     * @code
     * auto edit = editor::entity_deleted(world, crate);
     * world.destroy(crate);
     * history.record(std::move(edit));
     * @endcode
     *
     * @param world The world holding the entity.
     * @param entity The entity to be deleted. Every descendant goes with it.
     * @param types Where to look a component name up, or null for the
     * process-wide registry.
     * @param library The prefabs to collapse against, or null for the
     * process-wide library.
     * @return The edit, or null when @p entity is not in the world.
     */
    [[nodiscard]] std::unique_ptr<Edit> entity_deleted(
        const scene::World& world, entt::entity entity,
        const scene::ComponentRegistry* types = nullptr,
        const scene::PrefabLibrary* library = nullptr);

    /**
     * @brief Records an entity somebody just created.
     *
     * What a prefab dropped on the viewport does. Call it after the create,
     * because the fragment is what the new entity holds rather than what it was
     * asked for. A redo then builds the same thing rather than instancing the
     * prefab again, so an instance that was edited and undone twice does not
     * lose the edit.
     *
     * @param world The world holding the entity.
     * @param entity The entity that was created. Every descendant goes with it,
     * so a prefab instance is one edit rather than one for each member.
     * @param types Where to look a component name up, or null for the
     * process-wide registry.
     * @param library The prefabs to collapse against, or null for the
     * process-wide library.
     * @return The edit, or null when @p entity is not in the world.
     */
    [[nodiscard]] std::unique_ptr<Edit> entity_created(
        const scene::World& world, entt::entity entity,
        const scene::ComponentRegistry* types = nullptr,
        const scene::PrefabLibrary* library = nullptr);

    /**
     * @brief Deletes an entity and records how to bring it back.
     *
     * The two steps have an order and it is easy to get wrong, because the
     * subtree has to be read before it goes. This is the pair, so no caller has
     * to remember it and a test can drive it with no window.
     *
     * @param world The world to delete from.
     * @param entity The entity to delete. Every descendant goes with it.
     * @param history Where the entry goes, or null to delete with no way back.
     * The runtime debug overlay passes null.
     * @param types Where to look a component name up, or null for the
     * process-wide registry.
     * @param library The prefabs to collapse against, or null for the
     * process-wide library.
     * @return True when the entity was deleted.
     */
    bool delete_entity(scene::World& world, entt::entity entity, History* history,
                       const scene::ComponentRegistry* types = nullptr,
                       const scene::PrefabLibrary* library = nullptr);

    /**
     * @brief Puts a component on an entity and records it.
     *
     * The document is read after the component is there, so a redo brings back
     * the same values. See `component_added`.
     *
     * @param world The world holding the entity.
     * @param entity The entity to add to.
     * @param component The component name, as a scene file stores it.
     * @param history Where the entry goes, or null for no undo.
     * @param types Where to look the component name up, or null for the
     * process-wide registry.
     * @return True when the component was added. False when the entity already
     * carried it, or when nothing registered that name.
     */
    bool add_component(scene::World& world, entt::entity entity, std::string_view component,
                       History* history, const scene::ComponentRegistry* types = nullptr);

    /**
     * @brief Takes a component off an entity and records it.
     *
     * The document is read before the component goes, so an undo brings back
     * the values it had rather than the defaults.
     *
     * @param world The world holding the entity.
     * @param entity The entity to take it off.
     * @param component The component name.
     * @param history Where the entry goes, or null for no undo.
     * @param types Where to look the component name up, or null for the
     * process-wide registry.
     * @return True when the component was removed.
     */
    bool remove_component(scene::World& world, entt::entity entity, std::string_view component,
                          History* history, const scene::ComponentRegistry* types = nullptr);

    /**
     * @brief Where an entity hangs in the hierarchy.
     *
     * Both halves are identities, because an `entt::entity` is a slot number
     * that EnTT hands out again and an undo entry outlives one edit.
     */
    struct Place {
        /// @brief What it hangs under. Null for a root of the world.
        Guid parent;
        /// @brief The sibling it sits in front of. Null when it is the last child.
        Guid before;
    };

    /**
     * @brief Reads where an entity hangs right now.
     *
     * The caller reads this before it moves the entity, and hands it to
     * `entity_reparented` afterwards. The same shape `component_changed` uses
     * for the document it takes as `before`.
     *
     * @param world The world holding the entity.
     * @param entity The entity to read.
     * @return Where it hangs. Both halves are null for an entity that is not in
     * the world.
     */
    [[nodiscard]] Place place_of(const scene::World& world, entt::entity entity);

    /**
     * @brief Records an entity somebody moved to another parent.
     *
     * The sibling matters as much as the parent. Child order is what a scene
     * file writes and what the hierarchy panel shows, so an undo that puts the
     * entity back under the right parent at the wrong place has not put it
     * back.
     *
     * @warning Call this after the move, with the place read before it.
     *
     * @param world The world holding the entity, read for where it hangs now.
     * @param entity The entity that moved.
     * @param from Where it hung before, from `place_of`.
     * @return The edit, or null when @p entity is not in the world.
     */
    [[nodiscard]] std::unique_ptr<Edit> entity_reparented(const scene::World& world,
                                                          entt::entity entity, Place from);

} // namespace engine::editor
