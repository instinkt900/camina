#include "editor/edits.h"

#include "core/log.h"
#include "scene/component_registry.h"
#include "scene/components.h"
#include "scene/prefab.h"
#include "scene/scene_file.h"
#include "scene/world.h"

#include <utility>

namespace engine::editor {

    namespace {

        /**
         * What to call an entity in the menu.
         *
         * The Name component when it carries one, and the identity otherwise.
         * "Undo delete" is a stack a person cannot read, and an entity with no
         * name is still better named by something than by nothing.
         */
        [[nodiscard]] std::string label_for(const scene::World& world, entt::entity entity) {
            const auto* named = world.registry().try_get<scene::Name>(entity);
            if (named != nullptr && !named->value.empty()) {
                return named->value;
            }
            return to_text(world.identity(entity));
        }

        /// The word the menu uses for a state going to another state.
        [[nodiscard]] const char* verb_for(const nlohmann::json& before,
                                           const nlohmann::json& after) {
            if (before.is_null()) {
                return "add";
            }
            if (after.is_null()) {
                return "remove";
            }
            return "change";
        }

    } // namespace

    ComponentEdit::ComponentEdit(Guid entity, std::string_view component, nlohmann::json before,
                                 nlohmann::json after, const scene::ComponentRegistry* types)
        : entity_(entity)
        , component_(component)
        , before_(std::move(before))
        , after_(std::move(after))
        , types_(types != nullptr ? types : &scene::components())
        , label_(std::string(verb_for(before_, after_)) + " " + component_) {}

    bool ComponentEdit::fits(const scene::World& world) const {
        return world.find(entity_) != entt::null;
    }

    void ComponentEdit::put(scene::World& world, const nlohmann::json& state) {
        const entt::entity entity = world.find(entity_);
        if (entity == entt::null) {
            // The stack unwinds in order, so a delete is undone before anything
            // that names what it took. Reaching this means an edit outlived its
            // entity by some other route, and silently doing nothing would hide
            // it.
            ENGINE_LOG_ERROR("{} names the entity {}, which is not in the world.", label_,
                             to_text(entity_));
            return;
        }

        const scene::ComponentOps* ops = types_->find(component_);
        if (ops == nullptr) {
            ENGINE_LOG_ERROR("{} names the component {}, which nothing registered.", label_,
                             component_);
            return;
        }

        if (state.is_null()) {
            ops->remove(world.registry(), entity);
        } else if (!ops->load(world.registry(), entity, state)) {
            ENGINE_LOG_ERROR("{} could not read the {} it kept.", label_, component_);
            return;
        }

        if (ops->owns_transform) {
            // The write went through the registry, so it went around
            // set_local(). Without this the world matrices stay stale and only
            // a later move would put them right.
            world.mark_dirty(entity);
        }
    }

    void ComponentEdit::apply(scene::World& world) { put(world, after_); }

    void ComponentEdit::revert(scene::World& world) { put(world, before_); }

    std::unique_ptr<Edit> component_changed(Guid entity, std::string_view component,
                                            nlohmann::json before, nlohmann::json after,
                                            const scene::ComponentRegistry* types) {
        return std::make_unique<ComponentEdit>(entity, component, std::move(before),
                                               std::move(after), types);
    }

    std::unique_ptr<Edit> component_added(Guid entity, std::string_view component,
                                          nlohmann::json added,
                                          const scene::ComponentRegistry* types) {
        return std::make_unique<ComponentEdit>(entity, component, nlohmann::json(),
                                               std::move(added), types);
    }

    std::unique_ptr<Edit> component_removed(Guid entity, std::string_view component,
                                            nlohmann::json removed,
                                            const scene::ComponentRegistry* types) {
        return std::make_unique<ComponentEdit>(entity, component, std::move(removed),
                                               nlohmann::json(), types);
    }


    EntityEdit::EntityEdit(Guid entity, nlohmann::json fragment, bool exists_after,
                           std::string label, const scene::ComponentRegistry* types,
                           const scene::PrefabLibrary* library)
        : entity_(entity)
        , fragment_(std::move(fragment))
        , exists_after_(exists_after)
        , types_(types != nullptr ? types : &scene::components())
        , library_(library != nullptr ? library : &scene::prefabs())
        , label_(std::move(label)) {}

    bool EntityEdit::fits(const scene::World& world) const {
        // The state this edit last left behind. A create left the entity in the
        // world and a delete left it out, so a rebuild that disagrees either
        // way is a rebuild this edit was not recorded against.
        return (world.find(entity_) != entt::null) == exists_after_;
    }

    void EntityEdit::put(scene::World& world, bool exists) {
        const entt::entity found = world.find(entity_);

        if (!exists) {
            if (found == entt::null) {
                // The stack unwinds in order, so nothing should ask twice.
                // Doing nothing quietly would hide an entry that outlived its
                // entity by some other route.
                ENGINE_LOG_ERROR("{} names the entity {}, which is not in the world.", label_,
                                 to_text(entity_));
                return;
            }
            world.destroy(found);
            return;
        }

        if (found != entt::null) {
            ENGINE_LOG_ERROR("{} would build the entity {} again, and it is already in the "
                             "world.",
                             label_, to_text(entity_));
            return;
        }
        if (scene::load_subtree(fragment_, world, *types_, *library_) == entt::null) {
            ENGINE_LOG_ERROR("{} could not build the subtree it kept.", label_);
        }
    }

    void EntityEdit::apply(scene::World& world) { put(world, exists_after_); }

    void EntityEdit::revert(scene::World& world) { put(world, !exists_after_); }

    namespace {

        /// The one shape entity_deleted() and entity_created() share.
        [[nodiscard]] std::unique_ptr<Edit> entity_edit(const scene::World& world,
                                                        entt::entity entity, bool exists_after,
                                                        const char* verb,
                                                        const scene::ComponentRegistry* types,
                                                        const scene::PrefabLibrary* library) {
            if (entity == entt::null || !world.registry().valid(entity)) {
                ENGINE_LOG_ERROR("{} was given an entity that is not in the world.", verb);
                return nullptr;
            }

            nlohmann::json fragment = scene::save_subtree(
                world, entity, types != nullptr ? *types : scene::components(),
                library != nullptr ? *library : scene::prefabs());
            if (fragment.is_null()) {
                return nullptr;
            }

            return std::make_unique<EntityEdit>(world.identity(entity), std::move(fragment),
                                                exists_after,
                                                std::string(verb) + " " + label_for(world, entity),
                                                types, library);
        }

    } // namespace

    std::unique_ptr<Edit> entity_deleted(const scene::World& world, entt::entity entity,
                                         const scene::ComponentRegistry* types,
                                         const scene::PrefabLibrary* library) {
        return entity_edit(world, entity, false, "delete", types, library);
    }

    std::unique_ptr<Edit> entity_created(const scene::World& world, entt::entity entity,
                                         const scene::ComponentRegistry* types,
                                         const scene::PrefabLibrary* library) {
        return entity_edit(world, entity, true, "create", types, library);
    }

    bool delete_entity(scene::World& world, entt::entity entity, History* history,
                       const scene::ComponentRegistry* types,
                       const scene::PrefabLibrary* library) {
        if (entity == entt::null || !world.registry().valid(entity)) {
            return false;
        }

        // Before the destroy. Afterwards there is nothing left to read, which
        // is the whole reason this pair exists.
        std::unique_ptr<Edit> edit;
        if (history != nullptr) {
            edit = entity_deleted(world, entity, types, library);
        }

        world.destroy(entity);

        if (history != nullptr) {
            // A null edit means save_subtree refused. The delete still happened,
            // and recording nothing is better than recording an entry that
            // cannot build the entity again.
            history->record(std::move(edit));
        }
        return true;
    }

    namespace {

        /// The registry a caller named, or the process-wide one.
        [[nodiscard]] const scene::ComponentRegistry& registry_or_default(
            const scene::ComponentRegistry* types) {
            return types != nullptr ? *types : scene::components();
        }

    } // namespace

    bool add_component(scene::World& world, entt::entity entity, std::string_view component,
                       History* history, const scene::ComponentRegistry* types) {
        if (entity == entt::null || !world.registry().valid(entity)) {
            return false;
        }

        const scene::ComponentRegistry& registry = registry_or_default(types);
        const scene::ComponentOps* ops = registry.find(std::string(component));
        if (ops == nullptr || ops->create == nullptr) {
            ENGINE_LOG_ERROR("Nothing registered a component called {}, so it cannot be added.",
                             component);
            return false;
        }
        if (ops->has(world.registry(), entity)) {
            return false;
        }

        ops->create(world.registry(), entity);

        if (history != nullptr) {
            // After the create, so a redo brings back the values the component
            // arrived with rather than nothing.
            history->record(component_added(world.identity(entity), component,
                                            ops->save(world.registry(), entity), types));
        }
        if (ops->owns_transform) {
            world.mark_dirty(entity);
        }
        return true;
    }

    bool remove_component(scene::World& world, entt::entity entity, std::string_view component,
                          History* history, const scene::ComponentRegistry* types) {
        if (entity == entt::null || !world.registry().valid(entity)) {
            return false;
        }

        const scene::ComponentRegistry& registry = registry_or_default(types);
        const scene::ComponentOps* ops = registry.find(std::string(component));
        if (ops == nullptr || !ops->has(world.registry(), entity)) {
            return false;
        }

        // Before the remove, so an undo brings back the values it had rather
        // than the defaults.
        if (history != nullptr) {
            history->record(component_removed(world.identity(entity), component,
                                              ops->save(world.registry(), entity), types));
        }

        ops->remove(world.registry(), entity);
        if (ops->owns_transform) {
            world.mark_dirty(entity);
        }
        return true;
    }

    Place place_of(const scene::World& world, entt::entity entity) {
        if (entity == entt::null || !world.registry().valid(entity)) {
            return {};
        }

        const auto& node = world.registry().get<scene::Hierarchy>(entity);
        return Place{
            .parent = node.parent == entt::null ? Guid{} : world.identity(node.parent),
            .before = node.next_sibling == entt::null ? Guid{}
                                                      : world.identity(node.next_sibling),
        };
    }

    namespace {

        /**
         * An entity somebody moved to another parent, or to another place.
         *
         * Held by identity on both sides, so the edit survives the entity it names
         * being deleted and brought back. Nothing outside this file builds one, so
         * it stays here rather than in the header.
         */
        class ReparentEdit : public Edit {
        public:
            ReparentEdit(Guid entity, Place before, Place after, std::string label)
                : entity_(entity)
                , before_(before)
                , after_(after)
                , label_(std::move(label)) {}

            void apply(scene::World& world) override { put(world, after_); }

            void revert(scene::World& world) override { put(world, before_); }

            [[nodiscard]] const char* name() const override { return label_.c_str(); }

            /// The entity and both ends of the move. A reparent reaches all
            /// three, so all three have to be there.
            [[nodiscard]] bool fits(const scene::World& world) const override {
                const auto present = [&world](Guid id) {
                    return !id.valid() || world.find(id) != entt::null;
                };
                return world.find(entity_) != entt::null && present(before_.parent) &&
                       present(before_.before) && present(after_.parent) &&
                       present(after_.before);
            }

        private:
            /// Finds the entity one half of a place names, or reports why not.
            [[nodiscard]] bool resolve(const scene::World& world, Guid id, const char* what,
                                       entt::entity& out) const {
                if (!id.valid()) {
                    out = entt::null;
                    return true;
                }
                out = world.find(id);
                if (out == entt::null) {
                    ENGINE_LOG_ERROR("{} names the {} {}, which is not in the world.", label_, what,
                                     to_text(id));
                    return false;
                }
                return true;
            }

            void put(scene::World& world, Place place) {
                const entt::entity entity = world.find(entity_);
                if (entity == entt::null) {
                    ENGINE_LOG_ERROR("{} names the entity {}, which is not in the world.", label_,
                                     to_text(entity_));
                    return;
                }

                entt::entity parent = entt::null;
                entt::entity before = entt::null;
                if (!resolve(world, place.parent, "parent", parent) ||
                    !resolve(world, place.before, "sibling", before)) {
                    return;
                }

                if (!world.set_parent(entity, parent, before)) {
                    ENGINE_LOG_ERROR("{} could not put the entity back where it was.", label_);
                }
            }

            Guid entity_;
            Place before_;
            Place after_;
            std::string label_;
        };

    } // namespace

    std::unique_ptr<Edit> entity_reparented(const scene::World& world, entt::entity entity,
                                            Place from) {
        if (entity == entt::null || !world.registry().valid(entity)) {
            ENGINE_LOG_ERROR("reparent was given an entity that is not in the world.");
            return nullptr;
        }

        return std::make_unique<ReparentEdit>(world.identity(entity), from,
                                              place_of(world, entity),
                                              "move " + label_for(world, entity));
    }

} // namespace engine::editor
