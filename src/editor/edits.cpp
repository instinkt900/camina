#include "editor/edits.h"

#include "core/log.h"
#include "scene/component_registry.h"
#include "scene/world.h"

#include <utility>

namespace engine::editor {

    namespace {

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

} // namespace engine::editor
