#include "physics/conventions.h"

#include <box3d/box3d.h>

namespace engine::physics {

    Vec3 default_gravity() {
        const b3WorldDef def = b3DefaultWorldDef();
        return Vec3{ def.gravity.x, def.gravity.y, def.gravity.z };
    }

} // namespace engine::physics
