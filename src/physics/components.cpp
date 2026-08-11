#include "physics/components.h"

#include "scene/component_registry.h"

namespace engine::physics {

    void register_components(scene::ComponentRegistry& registry) {
        registry.add<RigidBody>();
        registry.add<BoxCollider>();
        registry.add<SphereCollider>();
    }

    void register_components() {
        register_components(scene::components());
    }

} // namespace engine::physics
