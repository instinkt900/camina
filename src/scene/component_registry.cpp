#include "scene/component_registry.h"

#include "core/log.h"
#include "math/transform.h"
#include "scene/components.h"

namespace engine::scene {

    const ComponentOps* ComponentRegistry::find(std::string_view name) const {
        for (const ComponentOps& entry : entries_) {
            if (name == entry.name) {
                return &entry;
            }
        }
        return nullptr;
    }

    ComponentRegistry& components() {
        // A function-local static builds on the first call, so this is ready
        // before main() runs and needs no start-up order rule.
        static ComponentRegistry registry;
        return registry;
    }

    nlohmann::json save_components(const entt::registry& registry, entt::entity entity,
                                   const ComponentRegistry& types) {
        nlohmann::json parts = nlohmann::json::object();
        for (const ComponentOps& ops : types.all()) {
            if (ops.has(registry, entity)) {
                parts[ops.name] = ops.save(registry, entity);
            }
        }
        return parts;
    }

    bool load_components(const nlohmann::json& parts, entt::registry& registry,
                         entt::entity entity, const ComponentRegistry& types,
                         std::string_view where) {
        if (!parts.is_object()) {
            ENGINE_LOG_ERROR("{} holds components that are not an object.", where);
            return false;
        }

        bool ok = true;
        for (const auto& [name, value] : parts.items()) {
            const ComponentOps* ops = types.find(name);
            if (ops == nullptr) {
                // An older build reading a newer file lands here. Keep the rest
                // of the entity rather than refuse the whole document.
                ENGINE_LOG_WARN("{} carries component {}, which this build does not know. "
                                "Skipping it.",
                                where, name);
                continue;
            }
            if (!ops->load(registry, entity, value)) {
                ENGINE_LOG_ERROR("{} could not read its {} component.", where, name);
                ok = false;
            }
        }
        return ok;
    }

    void register_builtin_components(ComponentRegistry& registry) {
        registry.add<Transform>();
        registry.add<Name>();
        registry.add<MeshRenderer>();
        registry.add<DirectionalLight>();
        registry.add<PointLight>();
        registry.add<Environment>();
        // Hierarchy and WorldTransform stay out on purpose. A scene file stores
        // the parent link itself, and World rebuilds both from it.
    }

} // namespace engine::scene
