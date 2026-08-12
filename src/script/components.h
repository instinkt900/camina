#pragma once

/**
 * @file
 * @brief The component that puts a script on an entity.
 *
 * An ordinary reflected component, like `physics::RigidBody`. Hard rule 4.5
 * does the rest: the inspector draws the field, the `.scene` format reads and
 * writes it, and a prefab instance can override it. None of that needed code
 * written for it.
 *
 * Nothing here names a Lua type or a sol2 type, and nothing here includes one.
 * A component says which script an entity runs, and `script/host.h` is what
 * runs it. So a scene, a tool, or a test can carry a script reference with no
 * interpreter anywhere near it.
 */

#include "core/guid.h"
#include "reflect/attributes.h"
#include "reflect/reflect.h"

#include <tuple>

/// @cond
// Forward declared rather than included, the same way physics/components.h
// does it. A component has no business pulling the scene registry in behind it.
namespace engine::scene {
    class ComponentRegistry;
}
/// @endcond

/// @brief Lua scripting: the host, the component, and the bindings.
namespace engine::script {

    /**
     * @brief Runs a Lua script on this entity.
     *
     * The script is named by GUID, the same way `scene::MeshRenderer` names a
     * mesh. Authored content writes `asset:scripts/spin.lua` and the cooker
     * turns that into the identity before it writes the file. See
     * `assets/reference.h`.
     *
     * The host gives each entity carrying this its own Lua table, so two
     * entities running one script keep separate state.
     */
    struct ScriptComponent {
        /// @brief The cooked script this entity runs.
        Guid script;
    };

    /**
     * @brief Registers the script components with a scene registry.
     *
     * The engine registers its own built-ins in
     * `scene::register_builtin_components()`, and this is the same call for the
     * script subsystem. It stays separate so that `scene/` needs no script
     * header.
     *
     * @param registry Where to register them.
     */
    void register_components(scene::ComponentRegistry& registry);

    /**
     * @brief Registers the script components with the global scene registry.
     *
     * An overload rather than a default argument, so that naming the global
     * registry does not force this header to include it.
     */
    void register_components();

} // namespace engine::script

/// @brief Describes the script an entity runs, for the inspector and scene files.
template <>
struct engine::reflect::Describe<engine::script::ScriptComponent> {
    static constexpr const char* name = "ScriptComponent"; ///< The name a scene file stores.
    /// @brief The one field.
    /// @return A tuple of field descriptors.
    static constexpr auto fields() {
        return std::make_tuple(ENGINE_FIELD(
            engine::script::ScriptComponent, script, engine::reflect::AssetRef{},
            engine::reflect::Tooltip{ "The cooked Lua script this entity runs." }));
    }
};
