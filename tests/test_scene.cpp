// M3.1 tests. The hierarchy is where the bugs live, so these check the update
// order, the dirty propagation, and the link surgery in both directions.

#include "check.h"
#include "math/conventions.h"
#include "math/transform.h"
#include "scene/world.h"

#include <array>
#include <cstddef>
#include <vector>

namespace {

    using test::check;
    namespace sc = engine::scene;

    constexpr float kEpsilon = 1e-4F;

    bool near_equal(const engine::Vec3& left, const engine::Vec3& right) {
        return glm::all(glm::epsilonEqual(left, right, kEpsilon));
    }

    /// The world-space position a matrix puts the origin at.
    engine::Vec3 origin_of(const engine::Mat4& matrix) {
        return engine::Vec3{ matrix[3] };
    }

    /// Walks a child list from the front, in order.
    std::vector<entt::entity> children_of(const sc::World& world, entt::entity parent) {
        std::vector<entt::entity> found;
        const auto& node = world.registry().get<sc::Hierarchy>(parent);
        for (entt::entity walk = node.first_child; walk != entt::null;
             walk = world.registry().get<sc::Hierarchy>(walk).next_sibling) {
            found.push_back(walk);
        }
        return found;
    }

    void test_create_and_defaults() {
        sc::World world;
        const entt::entity entity = world.create();

        check(world.size() == 1, "create adds one entity");
        check(near_equal(world.local(entity).position, engine::Vec3{ 0.0F, 0.0F, 0.0F }),
              "a new entity sits at the origin");
        check(world.local(entity).scale == engine::Vec3{ 1.0F, 1.0F, 1.0F },
              "a new entity is at natural size");
        check(world.local(entity).rotation.w == 1.0F, "the identity quaternion stores w first");

        world.update();
        check(world.rebuilt_last_update() == 1, "the first update builds the new matrix");
        check(near_equal(origin_of(world.world_matrix(entity)), engine::Vec3{ 0.0F, 0.0F, 0.0F }),
              "the matrix of an untouched entity is the identity");
    }

    void test_parenting_moves_the_child() {
        sc::World world;
        const entt::entity parent = world.create();
        const entt::entity child = world.create();

        check(world.set_parent(child, parent), "set_parent links the two");
        world.set_local(parent, { .position = { 0.0F, 2.0F, 0.0F } });
        world.set_local(child, { .position = { 1.0F, 0.0F, 0.0F } });
        world.update();

        check(near_equal(origin_of(world.world_matrix(child)), engine::Vec3{ 1.0F, 2.0F, 0.0F }),
              "a child adds its parent transform");
        check(near_equal(origin_of(world.world_matrix(parent)), engine::Vec3{ 0.0F, 2.0F, 0.0F }),
              "the parent keeps its own transform");
    }

    void test_scale_reaches_the_child() {
        sc::World world;
        const entt::entity parent = world.create();
        const entt::entity child = world.create();
        check(world.set_parent(child, parent), "the child attaches");

        world.set_local(parent, { .scale = { 2.0F, 2.0F, 2.0F } });
        world.set_local(child, { .position = { 1.0F, 0.0F, 0.0F } });
        world.update();

        check(near_equal(origin_of(world.world_matrix(child)), engine::Vec3{ 2.0F, 0.0F, 0.0F }),
              "a parent scale moves the child further out");
    }

    /// The order test. A deep chain must resolve in one pass, not in one pass
    /// for each level.
    void test_deep_chain_in_one_pass() {
        constexpr std::size_t kDepth = 64;

        sc::World world;
        std::vector<entt::entity> chain;
        chain.reserve(kDepth);
        for (std::size_t i = 0; i < kDepth; ++i) {
            const entt::entity entity = world.create();
            if (i > 0) {
                check(world.set_parent(entity, chain[i - 1]), "the chain links");
            }
            chain.push_back(entity);
            world.set_local(entity, { .position = { 1.0F, 0.0F, 0.0F } });
        }

        world.update();
        check(world.rebuilt_last_update() == kDepth, "one pass rebuilds every level");

        const engine::Vec3 expected{ static_cast<float>(kDepth), 0.0F, 0.0F };
        check(near_equal(origin_of(world.world_matrix(chain.back())), expected),
              "every level of a deep chain added its own offset");

        // Moving the root must reach the far end in a single pass. If the walk
        // ran in the wrong order, the leaf would read a stale parent and lag by
        // one update.
        world.set_local(chain.front(), { .position = { 0.0F, 10.0F, 0.0F } });
        world.update();
        const engine::Vec3 moved{ static_cast<float>(kDepth) - 1.0F, 10.0F, 0.0F };
        check(near_equal(origin_of(world.world_matrix(chain.back())), moved),
              "moving the root reaches the leaf in the same pass");
    }

    void test_dirty_propagation() {
        sc::World world;
        const entt::entity root = world.create();
        const entt::entity middle = world.create();
        const entt::entity leaf = world.create();
        check(world.set_parent(middle, root), "middle attaches to root");
        check(world.set_parent(leaf, middle), "leaf attaches to middle");

        world.update();
        check(world.rebuilt_last_update() == 3, "the first update builds all three");

        world.update();
        check(world.rebuilt_last_update() == 0, "a frame that moved nothing rebuilds nothing");

        world.set_local(root, { .position = { 0.0F, 1.0F, 0.0F } });
        world.update();
        check(world.rebuilt_last_update() == 3, "moving the root marks the whole subtree");

        world.set_local(leaf, { .position = { 1.0F, 0.0F, 0.0F } });
        world.update();
        check(world.rebuilt_last_update() == 1, "moving a leaf marks only the leaf");

        world.update();
        check(world.rebuilt_last_update() == 0, "the world settles again");
    }

    void test_reparent_and_detach() {
        sc::World world;
        const entt::entity first = world.create();
        const entt::entity second = world.create();
        const entt::entity child = world.create();

        world.set_local(first, { .position = { 5.0F, 0.0F, 0.0F } });
        world.set_local(second, { .position = { 0.0F, 0.0F, 7.0F } });

        check(world.set_parent(child, first), "the child joins the first parent");
        world.update();
        check(near_equal(origin_of(world.world_matrix(child)), engine::Vec3{ 5.0F, 0.0F, 0.0F }),
              "the child follows its first parent");

        check(world.set_parent(child, second), "the child moves to the second parent");
        world.update();
        check(near_equal(origin_of(world.world_matrix(child)), engine::Vec3{ 0.0F, 0.0F, 7.0F }),
              "the child follows its new parent");

        check(world.set_parent(child, entt::null), "null detaches the child");
        world.update();
        check(near_equal(origin_of(world.world_matrix(child)), engine::Vec3{ 0.0F, 0.0F, 0.0F }),
              "a detached child keeps its local transform and loses the parent");
    }

    /// Detaching the middle of a sibling list must not lose the ends.
    void test_sibling_list_surgery() {
        sc::World world;
        const entt::entity parent = world.create();
        std::array<entt::entity, 3> kids{};
        for (entt::entity& kid : kids) {
            kid = world.create();
            check(world.set_parent(kid, parent), "a sibling attaches");
        }

        check(world.registry().get<sc::Hierarchy>(parent).child_count == 3,
              "the parent counts three children");

        // The list must read in attach order. A scene file depends on this, and
        // without it a save and a load reverse every sibling list.
        check(children_of(world, parent) == std::vector<entt::entity>({ kids[0], kids[1],
                                                                        kids[2] }),
              "the child list keeps the order they attached in");

        check(world.set_parent(kids[1], entt::null), "the middle sibling detaches");
        check(world.registry().get<sc::Hierarchy>(parent).child_count == 2,
              "the count drops with it");
        check(children_of(world, parent) == std::vector<entt::entity>({ kids[0], kids[2] }),
              "removing the middle joins the two ends");

        // Both ends matter too. first_child and last_child must follow.
        check(world.set_parent(kids[0], entt::null), "the first sibling detaches");
        check(children_of(world, parent) == std::vector<entt::entity>({ kids[2] }),
              "removing the head leaves the tail");
        check(world.set_parent(kids[2], entt::null), "the last sibling detaches");
        check(children_of(world, parent).empty(), "removing the tail empties the list");
        check(world.registry().get<sc::Hierarchy>(parent).first_child == entt::null &&
                  world.registry().get<sc::Hierarchy>(parent).last_child == entt::null,
              "an empty list holds neither end");
    }

    void test_cycles_are_refused() {
        sc::World world;
        const entt::entity root = world.create();
        const entt::entity middle = world.create();
        const entt::entity leaf = world.create();
        check(world.set_parent(middle, root), "middle attaches");
        check(world.set_parent(leaf, middle), "leaf attaches");

        check(!world.set_parent(root, leaf), "a link that closes a loop is refused");
        check(!world.set_parent(root, root), "an entity cannot parent itself");
        check(world.registry().get<sc::Hierarchy>(root).parent == entt::null,
              "the refused link left the hierarchy alone");

        check(world.is_ancestor(root, leaf), "is_ancestor walks the whole way up");
        check(!world.is_ancestor(leaf, root), "is_ancestor does not walk down");
    }

    void test_destroy_takes_the_subtree() {
        sc::World world;
        const entt::entity root = world.create();
        const entt::entity middle = world.create();
        const entt::entity leaf = world.create();
        const entt::entity other = world.create();
        check(world.set_parent(middle, root), "middle attaches");
        check(world.set_parent(leaf, middle), "leaf attaches");

        check(world.size() == 4, "four entities exist");

        world.destroy(middle);
        check(world.size() == 2, "destroying a parent takes its children");
        check(world.registry().valid(root), "the root above it survives");
        check(world.registry().valid(other), "an unrelated entity survives");
        check(world.registry().get<sc::Hierarchy>(root).child_count == 0,
              "the parent count drops to zero");

        // The world must still walk without touching a dead entity.
        world.update();
        check(true, "update runs after a subtree is destroyed");
    }

    void test_conventions() {
        // DESIGN.md section 3. A quarter turn about +Y sends +Z to +X in a
        // right-handed system with +Y up.
        const engine::Quat turn =
            glm::angleAxis(glm::radians(90.0F), engine::Vec3{ 0.0F, 1.0F, 0.0F });

        sc::World world;
        const entt::entity parent = world.create();
        const entt::entity child = world.create();
        check(world.set_parent(child, parent), "the child attaches");

        world.set_local(parent, { .rotation = turn });
        world.set_local(child, { .position = { 0.0F, 0.0F, 1.0F } });
        world.update();

        check(near_equal(origin_of(world.world_matrix(child)), engine::Vec3{ 1.0F, 0.0F, 0.0F }),
              "a right-handed turn about +Y sends +Z to +X");
    }

} // namespace

int main() {
    std::printf("entities\n");
    test_create_and_defaults();
    std::printf("transforms\n");
    test_parenting_moves_the_child();
    test_scale_reaches_the_child();
    std::printf("update order\n");
    test_deep_chain_in_one_pass();
    std::printf("dirty propagation\n");
    test_dirty_propagation();
    std::printf("links\n");
    test_reparent_and_detach();
    test_sibling_list_surgery();
    test_cycles_are_refused();
    test_destroy_takes_the_subtree();
    std::printf("conventions\n");
    test_conventions();
    return test::report();
}
