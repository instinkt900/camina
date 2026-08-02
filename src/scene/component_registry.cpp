#include "scene/component_registry.h"

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

    void register_builtin_components(ComponentRegistry& registry) {
        registry.add<Transform>();
        registry.add<Name>();
        // Hierarchy and WorldTransform stay out on purpose. A scene file stores
        // the parent link itself, and World rebuilds both from it.
    }

} // namespace engine::scene
