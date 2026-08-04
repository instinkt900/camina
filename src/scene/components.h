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
#include "core/guid.h"
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

    /**
     * @brief The mesh an entity draws, named by identity rather than by path.
     *
     * The GUID is what the cooker gave the mesh, which for a glTF file is the
     * identity `Guid::derive` worked out for that one mesh inside it. A rename
     * inside the content tree therefore changes nothing here.
     *
     * An entity with this and a WorldTransform is what MeshPass draws.
     */
    struct MeshRenderer {
        /// @brief The cooked mesh. A null GUID draws nothing.
        Guid mesh;
    };

    /**
     * @brief A light with no position, only a direction. The sun.
     *
     * The direction is the entity's forward, which is its local −Z turned into
     * world space. See DESIGN.md section 3. So a light is aimed by turning it,
     * the same way a camera is, and it needs no direction field of its own.
     *
     * Moving one does nothing, which is correct for a light that is infinitely
     * far away.
     */
    struct DirectionalLight {
        /// @brief The color it emits. Linear, not sRGB.
        Vec3 color{ 1.0F, 1.0F, 1.0F };
        /// @brief How bright it is. The color is multiplied by this.
        float intensity = 1.0F;
    };

    /// @brief How far a new PointLight reaches, in meters.
    inline constexpr float kDefaultLightRange = 10.0F;

    /**
     * @brief A light at a point, shining in every direction.
     *
     * The position is the entity's world position, so a point light is moved by
     * moving its entity. Turning one does nothing.
     */
    struct PointLight {
        /// @brief The color it emits. Linear, not sRGB.
        Vec3 color{ 1.0F, 1.0F, 1.0F };
        /// @brief How bright it is at the source.
        float intensity = 1.0F;
        /**
         * @brief How far it reaches, in meters.
         *
         * The falloff is the inverse square, windowed so it reaches zero at this
         * distance rather than going on forever. Without the window every light
         * would touch every surface, and a scene could not cull one.
         */
        float range = kDefaultLightRange;
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

/// @brief Field descriptors for the mesh an entity draws.
template <>
struct engine::reflect::Describe<engine::scene::MeshRenderer> {
    static constexpr const char* name = "MeshRenderer"; ///< The name a scene file stores.
    /// @brief The one field.
    /// @return A tuple of field descriptors.
    static constexpr auto fields() {
        return std::make_tuple(
            ENGINE_FIELD(engine::scene::MeshRenderer, mesh,
                         engine::reflect::Tooltip{ "The cooked mesh this entity draws." }));
    }
};

/// @brief Field descriptors for a directional light.
template <>
struct engine::reflect::Describe<engine::scene::DirectionalLight> {
    static constexpr const char* name = "DirectionalLight"; ///< The name a scene file stores.
    /// @brief The two fields. The direction comes from the transform.
    /// @return A tuple of field descriptors.
    static constexpr auto fields() {
        return std::make_tuple(
            ENGINE_FIELD(engine::scene::DirectionalLight, color,
                         engine::reflect::Tooltip{ "The color it emits. Linear, not sRGB." }),
            ENGINE_FIELD(engine::scene::DirectionalLight, intensity,
                         engine::reflect::Range{ 0.0, 20.0, 0.01 },
                         engine::reflect::Tooltip{
                             "Aim it by turning the entity. Its direction is local −Z." }));
    }
};

/// @brief Field descriptors for a point light.
template <>
struct engine::reflect::Describe<engine::scene::PointLight> {
    static constexpr const char* name = "PointLight"; ///< The name a scene file stores.
    /// @brief The three fields. The position comes from the transform.
    /// @return A tuple of field descriptors.
    static constexpr auto fields() {
        return std::make_tuple(
            ENGINE_FIELD(engine::scene::PointLight, color,
                         engine::reflect::Tooltip{ "The color it emits. Linear, not sRGB." }),
            ENGINE_FIELD(engine::scene::PointLight, intensity,
                         engine::reflect::Range{ 0.0, 100.0, 0.01 },
                         engine::reflect::Tooltip{ "How bright it is at the source." }),
            ENGINE_FIELD(engine::scene::PointLight, range,
                         engine::reflect::Range{ 0.0, 100.0, 0.05 },
                         engine::reflect::Tooltip{ "How far it reaches, in meters." }));
    }
};
