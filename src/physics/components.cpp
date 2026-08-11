#include "physics/components.h"

namespace engine::physics {

    void register_components(scene::ComponentRegistry& registry) {
        registry.add<RigidBody>();
        registry.add<BoxCollider>();
        registry.add<SphereCollider>();
    }

} // namespace engine::physics
