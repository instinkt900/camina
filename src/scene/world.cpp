#include "scene/world.h"

#include "core/assert.h"
#include "core/log.h"

#include <algorithm>

namespace engine::scene {

    entt::entity World::create() {
        const entt::entity entity = registry_.create();
        registry_.emplace<Transform>(entity);
        registry_.emplace<WorldTransform>(entity);
        registry_.emplace<Hierarchy>(entity);

        // Every entity gets an identity here, so nothing in the engine has to
        // remember to give it one. A scene file replaces it with the one the
        // entity was saved under.
        const Guid id = Guid::generate();
        // The order goes up on every create, so entities built by a load are
        // numbered in the order the file listed them. See Id::order and #353.
        registry_.emplace<Id>(entity, Id{ .value = id, .order = next_order_++ });
        by_id_.emplace(id, entity);
        return entity;
    }

    entt::entity World::find(Guid id) const {
        const auto found = by_id_.find(id);
        return found != by_id_.end() ? found->second : entt::null;
    }

    Guid World::identity(entt::entity entity) const {
        // A stale or null entity answers with a null identity rather than
        // asserting. Asking what a selection is called, when the selection is
        // nothing, is a normal thing for a caller to do.
        if (entity == entt::null || !registry_.valid(entity)) {
            return {};
        }
        const auto* held = registry_.try_get<const Id>(entity);
        return held != nullptr ? held->value : Guid{};
    }

    std::uint64_t World::root_order(entt::entity entity) const {
        const auto* held = registry_.try_get<Id>(entity);
        return held != nullptr ? held->order : 0;
    }

    void World::set_root_order(entt::entity entity, std::uint64_t order) {
        auto* held = registry_.try_get<Id>(entity);
        if (held == nullptr) {
            return;
        }
        held->order = order;
        next_order_ = std::max(next_order_, order + 1);
    }

    bool World::set_identity(entt::entity entity, Guid id) {
        if (!id.valid()) {
            ENGINE_LOG_ERROR("An entity cannot be given a null identity.");
            return false;
        }

        const auto taken = by_id_.find(id);
        if (taken != by_id_.end() && taken->second != entity) {
            // Two entities answering to one identity is worse than one losing
            // it: every undo entry naming that identity would then reach
            // whichever of them the map happened to hold.
            ENGINE_LOG_ERROR("Two entities cannot share the identity {}.", to_text(id));
            return false;
        }

        auto* held = registry_.try_get<Id>(entity);
        if (held == nullptr) {
            // World::create() gives every entity an Id, so this means the
            // entity came from the registry directly. Say so rather than
            // failing quietly, because the caller then reads back an identity
            // it never set and nothing points at the cause.
            ENGINE_LOG_ERROR("An entity with no identity cannot be given one. It was not "
                             "created through World::create.");
            return false;
        }

        by_id_.erase(held->value);
        held->value = id;
        by_id_[id] = entity;
        return true;
    }

    std::size_t World::size() const {
        return registry_.view<const Hierarchy>().size();
    }

    void World::clear() {
        registry_.clear();
        by_id_.clear();
        // The walk buffer holds entities that no longer exist, and the count
        // describes an update that no longer means anything.
        stack_.clear();
        rebuilt_ = 0;
    }

    bool World::is_ancestor(entt::entity ancestor, entt::entity entity) const {
        for (entt::entity walk = entity; walk != entt::null;
             walk = registry_.get<Hierarchy>(walk).parent) {
            if (walk == ancestor) {
                return true;
            }
        }
        return false;
    }

    void World::detach(entt::entity child) {
        Hierarchy& node = registry_.get<Hierarchy>(child);
        if (node.parent == entt::null) {
            return;
        }

        Hierarchy& parent = registry_.get<Hierarchy>(node.parent);
        if (parent.first_child == child) {
            parent.first_child = node.next_sibling;
        }
        if (parent.last_child == child) {
            parent.last_child = node.prev_sibling;
        }
        if (node.prev_sibling != entt::null) {
            registry_.get<Hierarchy>(node.prev_sibling).next_sibling = node.next_sibling;
        }
        if (node.next_sibling != entt::null) {
            registry_.get<Hierarchy>(node.next_sibling).prev_sibling = node.prev_sibling;
        }
        --parent.child_count;

        node.parent = entt::null;
        node.prev_sibling = entt::null;
        node.next_sibling = entt::null;
    }

    void World::attach(entt::entity child, entt::entity parent, entt::entity before) {
        Hierarchy& node = registry_.get<Hierarchy>(child);
        Hierarchy& head = registry_.get<Hierarchy>(parent);

        node.parent = parent;
        ++head.child_count;

        if (before == entt::null) {
            // Join at the end, so the list keeps the order the caller attached in.
            node.next_sibling = entt::null;
            node.prev_sibling = head.last_child;
            if (head.last_child != entt::null) {
                registry_.get<Hierarchy>(head.last_child).next_sibling = child;
            } else {
                head.first_child = child;
            }
            head.last_child = child;
            return;
        }

        Hierarchy& next = registry_.get<Hierarchy>(before);
        node.next_sibling = before;
        node.prev_sibling = next.prev_sibling;
        if (next.prev_sibling != entt::null) {
            registry_.get<Hierarchy>(next.prev_sibling).next_sibling = child;
        } else {
            head.first_child = child;
        }
        next.prev_sibling = child;
    }

    bool World::set_parent(entt::entity child, entt::entity parent) {
        return set_parent(child, parent, entt::null);
    }

    bool World::set_parent(entt::entity child, entt::entity parent, entt::entity before) {
        ENGINE_CHECK(registry_.valid(child), "set_parent needs a live child.");

        if (parent != entt::null) {
            ENGINE_CHECK(registry_.valid(parent), "set_parent needs a live parent.");
            // Walking up from the wanted parent must not meet the child. If it
            // does, the link would close a loop and every walk would run forever.
            if (is_ancestor(child, parent)) {
                ENGINE_LOG_ERROR("set_parent would build a cycle. The link was not made.");
                return false;
            }
        }

        if (before != entt::null) {
            ENGINE_CHECK(registry_.valid(before), "set_parent needs a live sibling.");
            // Refused rather than treated as "at the end". A caller that names
            // the wrong sibling has the wrong list, and putting the child
            // somewhere else quietly is how an undo moves what nobody watched.
            // A root has no sibling list to sit in either: the roots come out
            // sorted by entity value, so there is no position to ask for.
            if (parent == entt::null || before == child ||
                registry_.get<Hierarchy>(before).parent != parent) {
                ENGINE_LOG_ERROR("set_parent was given a sibling that is not a child of the "
                                 "wanted parent. The link was not made.");
                return false;
            }
        }

        detach(child);
        if (parent != entt::null) {
            attach(child, parent, before);
        }
        mark_dirty(child);
        return true;
    }

    const Transform& World::local(entt::entity entity) const {
        return registry_.get<Transform>(entity);
    }

    void World::set_local(entt::entity entity, const Transform& transform) {
        registry_.get<Transform>(entity) = transform;
        mark_dirty(entity);
    }

    const Mat4& World::world_matrix(entt::entity entity) const {
        return registry_.get<WorldTransform>(entity).matrix;
    }

    void World::mark_dirty(entt::entity entity) {
        ENGINE_CHECK(registry_.valid(entity), "mark_dirty needs a live entity.");

        stack_.clear();
        stack_.push_back(entity);
        while (!stack_.empty()) {
            const entt::entity current = stack_.back();
            stack_.pop_back();

            registry_.get<WorldTransform>(current).dirty = true;

            const Hierarchy& node = registry_.get<Hierarchy>(current);
            for (entt::entity child = node.first_child; child != entt::null;
                 child = registry_.get<Hierarchy>(child).next_sibling) {
                stack_.push_back(child);
            }
        }
    }

    void World::update() {
        rebuilt_ = 0;

        stack_.clear();
        for (const auto [entity, node] : registry_.view<const Hierarchy>().each()) {
            if (node.parent == entt::null) {
                stack_.push_back(entity);
            }
        }

        // A node leaves the stack before its children join it, so every parent
        // is current by the time a child reads it. That is the whole ordering
        // guarantee, and it holds for any depth.
        while (!stack_.empty()) {
            const entt::entity current = stack_.back();
            stack_.pop_back();

            const Hierarchy& node = registry_.get<Hierarchy>(current);
            WorldTransform& world = registry_.get<WorldTransform>(current);

            if (world.dirty) {
                const Mat4 local_matrix = to_matrix(registry_.get<Transform>(current));
                world.matrix = node.parent == entt::null
                                   ? local_matrix
                                   : registry_.get<WorldTransform>(node.parent).matrix *
                                         local_matrix;
                world.dirty = false;
                ++rebuilt_;
            }

            for (entt::entity child = node.first_child; child != entt::null;
                 child = registry_.get<Hierarchy>(child).next_sibling) {
                stack_.push_back(child);
            }
        }
    }

    void World::destroy(entt::entity entity) {
        if (!registry_.valid(entity)) {
            return;
        }

        detach(entity);

        // Collect the whole subtree first. Destroying while walking would read
        // a link out of an entity that no longer exists.
        std::vector<entt::entity> doomed;
        stack_.clear();
        stack_.push_back(entity);
        while (!stack_.empty()) {
            const entt::entity current = stack_.back();
            stack_.pop_back();
            doomed.push_back(current);

            const Hierarchy& node = registry_.get<Hierarchy>(current);
            for (entt::entity child = node.first_child; child != entt::null;
                 child = registry_.get<Hierarchy>(child).next_sibling) {
                stack_.push_back(child);
            }
        }

        for (const entt::entity dead : doomed) {
            // The identity goes with the entity. An identity left behind would
            // answer with an entity number that EnTT is about to hand to
            // somebody else.
            by_id_.erase(identity(dead));
            registry_.destroy(dead);
        }
    }

} // namespace engine::scene
