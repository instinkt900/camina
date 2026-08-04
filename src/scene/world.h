#pragma once

/**
 * @file
 * @brief The entity registry and the transform hierarchy over it.
 *
 * The hierarchy is where the bugs live. A child must read a parent that is
 * already current, and a frame that moved nothing must rebuild nothing. World
 * owns both of those rules, so no caller has to remember them.
 */

#include "core/entt.h"
#include "math/transform.h"
#include "scene/components.h"

#include <entt/entity/registry.hpp>

#include <cstddef>
#include <vector>

namespace engine::scene {

    /**
     * @brief An EnTT registry with a transform hierarchy that stays correct.
     *
     * Every entity World creates carries a Transform, a WorldTransform, and a
     * Hierarchy. Reach the registry directly for anything else.
     *
     * @warning Change a Transform through set_local(), or call mark_dirty()
     * after you change one another way. A transform changed behind the back of
     * this class keeps a stale world matrix.
     *
     * @code
     * engine::scene::World world;
     * const entt::entity parent = world.create();
     * const entt::entity child = world.create();
     * world.set_parent(child, parent);
     * world.set_local(parent, { .position = { 0.0F, 2.0F, 0.0F } });
     * world.update();
     * const engine::Mat4& m = world.world_matrix(child);
     * @endcode
     */
    class World {
    public:
        World() = default;

        /// @brief The registry, for components this class does not own.
        /// @return The registry. It outlives every entity in it.
        [[nodiscard]] entt::registry& registry() { return registry_; }

        /// @brief The registry, for reading.
        /// @return The registry.
        [[nodiscard]] const entt::registry& registry() const { return registry_; }

        /**
         * @brief Creates an entity at the origin, with no parent.
         * @return The new entity. Its world matrix is stale until update() runs.
         */
        [[nodiscard]] entt::entity create();

        /**
         * @brief Destroys an entity and every descendant it holds.
         *
         * Destroying a parent and leaving the children behind would leave them
         * pointing at nothing, so the whole subtree goes.
         *
         * @param entity The root of the subtree to destroy. A stale entity does nothing.
         */
        void destroy(entt::entity entity);

        /**
         * @brief Moves an entity under a new parent, or to the root.
         *
         * The local transform does not change, so the entity moves in world
         * space to follow its new parent. The subtree becomes dirty.
         *
         * @param child The entity to move.
         * @param parent The new parent, or `entt::null` to make @p child a root.
         * @return True on success. False when the call would build a cycle,
         * which happens when @p parent is @p child or one of its descendants.
         */
        [[nodiscard]] bool set_parent(entt::entity child, entt::entity parent);

        /// @brief The local transform of an entity.
        /// @param entity The entity to read.
        /// @return Its transform, in parent space.
        [[nodiscard]] const Transform& local(entt::entity entity) const;

        /**
         * @brief Sets the local transform and marks the subtree stale.
         * @param entity The entity to move.
         * @param transform The new transform, in parent space.
         */
        void set_local(entt::entity entity, const Transform& transform);

        /**
         * @brief The world matrix of an entity.
         * @param entity The entity to read.
         * @return Its local-to-world matrix, as of the last update().
         * @warning Call update() first. Otherwise this returns the matrix from
         * before the last change.
         */
        [[nodiscard]] const Mat4& world_matrix(entt::entity entity) const;

        /**
         * @brief Marks an entity and every descendant stale.
         *
         * set_local() and set_parent() already call this. Call it yourself only
         * when you change a Transform through the registry.
         *
         * @param entity The root of the subtree to mark.
         */
        void mark_dirty(entt::entity entity);

        /**
         * @brief Rebuilds every stale world matrix, parents before children.
         *
         * The walk starts at the roots and reaches a child only after it has
         * finished its parent, so a child always reads a current parent matrix.
         *
         * Call this once for each frame, after everything has moved.
         */
        void update();

        /**
         * @brief How many world matrices the last update() rebuilt.
         *
         * A frame that moved nothing reports zero. This is the number a test
         * reads, and a useful line for a debug overlay.
         *
         * @return The count from the last update(), or 0 before the first one.
         */
        [[nodiscard]] std::size_t rebuilt_last_update() const { return rebuilt_; }

        /// @brief How many entities the world holds.
        /// @return The count, including every child.
        [[nodiscard]] std::size_t size() const;

        /**
         * @brief Destroys every entity and leaves an empty world.
         *
         * M4.5 uses this to read a scene again while the program runs. It is
         * the whole world and not one subtree, so no entity is left pointing
         * at a parent that went away.
         *
         * @warning Every entity handle taken before this is stale afterward.
         * The caller has to drop what it held, a selected entity included.
         */
        void clear();

        /**
         * @brief Whether one entity is the same as, or an ancestor of, another.
         * @param ancestor The entity to look for.
         * @param entity The entity to walk up from.
         * @return True when @p ancestor is @p entity or sits above it.
         */
        [[nodiscard]] bool is_ancestor(entt::entity ancestor, entt::entity entity) const;

    private:
        void detach(entt::entity child);
        void attach(entt::entity child, entt::entity parent);

        entt::registry registry_;
        /// @brief Reused by every walk, so a deep hierarchy allocates once.
        std::vector<entt::entity> stack_;
        std::size_t rebuilt_ = 0;
    };

} // namespace engine::scene
