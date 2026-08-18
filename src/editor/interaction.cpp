#include "editor/interaction.h"

#include "core/log.h"
#include "editor/edits.h"
#include "scene/component_registry.h"
#include "scene/world.h"

#include <utility>

namespace engine::editor {

    bool Interaction::begin(const scene::World& world, entt::entity entity,
                            std::string_view component, const scene::ComponentRegistry* types) {
        if (active_) {
            ENGINE_LOG_WARN("An edit of {} began while an edit of {} was still open. The older "
                            "one is dropped.",
                            component, component_);
            cancel();
        }

        const scene::ComponentRegistry* registry =
            types != nullptr ? types : &scene::components();
        const scene::ComponentOps* ops = registry->find(std::string(component));
        if (ops == nullptr) {
            ENGINE_LOG_ERROR("An edit named the component {}, which nothing registered.",
                             component);
            return false;
        }
        if (entity == entt::null || !world.registry().valid(entity) ||
            !ops->has(world.registry(), entity)) {
            return false;
        }

        active_ = true;
        entity_ = world.identity(entity);
        component_ = component;
        before_ = ops->save(world.registry(), entity);
        types_ = registry;
        return true;
    }

    bool Interaction::end(const scene::World& world, History& history) {
        if (!active_) {
            return false;
        }

        // Read everything out before the state is cleared, so every path below
        // leaves the interaction closed.
        const Guid entity = entity_;
        const std::string component = component_;
        nlohmann::json before = std::move(before_);
        const scene::ComponentRegistry* types = types_;
        cancel();

        const entt::entity found = world.find(entity);
        const scene::ComponentOps* ops = types->find(component);
        if (found == entt::null || ops == nullptr || !ops->has(world.registry(), found)) {
            // The entity went away part way through, which a script or a
            // reload can do. There is nothing to record and nothing is wrong.
            return false;
        }

        nlohmann::json after = ops->save(world.registry(), found);
        if (after == before) {
            // Grabbed, moved, and put back. That is not an edit, and an entry
            // for it would cost a person an undo that does nothing.
            return false;
        }

        history.record(component_changed(entity, component, std::move(before), std::move(after),
                                         types));
        return true;
    }

    void Interaction::cancel() {
        active_ = false;
        entity_ = Guid{};
        component_.clear();
        before_ = nlohmann::json();
        types_ = nullptr;
    }

} // namespace engine::editor
