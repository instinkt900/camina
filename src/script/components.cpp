#include "script/components.h"

#include "scene/component_registry.h"

namespace engine::script {

    void register_components(scene::ComponentRegistry& registry) {
        registry.add<ScriptComponent>();
    }

    void register_components() {
        register_components(scene::components());
    }

} // namespace engine::script
