#pragma once

/**
 * @file
 * @brief The components the transform hierarchy stores on every entity.
 *
 * `World` in scene/world.h keeps these consistent. Read them freely. Change
 * them only through the World interface, or the hierarchy links and the dirty
 * flags stop agreeing with each other.
 */

#include "core/entt.h"
#include "math/conventions.h"
#include "reflect/attributes.h"
#include "reflect/reflect.h"

#include <entt/entity/entity.hpp>
#include <entt/entity/fwd.hpp>

#include <cstddef>
#include <string>
#include <tuple>

/// @brief The entity world, the transform hierarchy, and scene files.
namespace engine::scene {

    /**
     * @brief The parent link and the child list, stored inside the entities.
     *
     * The children form a doubly linked list, so attaching and detaching cost
     * the same whether an entity has two children or two thousand. No entity
     * allocates a container for its children.
     *
     * A new child joins at the end, so the list keeps the order the caller
     * attached in. A scene file relies on that: without it, saving and loading
     * would reverse every sibling list.
     *
     * A root has no parent. A leaf has no first child.
     */
    struct Hierarchy {
        /// @brief The parent, or `entt::null` for a root.
        entt::entity parent = entt::null;
        /// @brief The first child, or `entt::null` for a leaf.
        entt::entity first_child = entt::null;
        /// @brief The last child, so attaching at the end costs nothing.
        entt::entity last_child = entt::null;
        /// @brief The next child of the same parent, or `entt::null` at the end.
        entt::entity next_sibling = entt::null;
        /// @brief The previous child of the same parent, or `entt::null` at the start.
        entt::entity prev_sibling = entt::null;
        /// @brief How many direct children this entity has. Grandchildren do not count.
        std::size_t child_count = 0;
    };

    /**
     * @brief The composed world matrix, and whether it is stale.
     *
     * `World::update()` rebuilds a matrix only when `dirty` is true, and clears
     * the flag. A frame that moved nothing therefore rebuilds nothing.
     *
     * A new entity starts dirty, because its matrix has never been composed.
     */
    struct WorldTransform {
        /// @brief Local to world. Valid after the next World::update().
        Mat4 matrix{ 1.0F };
        /// @brief True while the matrix does not match the local transform.
        bool dirty = true;
    };

    /**
     * @brief A label for one entity.
     *
     * Nothing in the engine reads this. A person reads it, in the editor and in
     * a scene file, and that is enough reason to keep it.
     */
    struct Name {
        std::string value; ///< Free text. It does not have to be unique.
    };

} // namespace engine::scene

/// @brief Describes Name for the inspector and for scene files.
template <>
struct engine::reflect::Describe<engine::scene::Name> {
    static constexpr const char* name = "Name"; ///< The name a scene file stores.
    /// @brief The one field.
    /// @return A tuple of field descriptors.
    static constexpr auto fields() {
        return std::make_tuple(ENGINE_FIELD(engine::scene::Name, value));
    }
};
