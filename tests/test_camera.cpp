// M9.5a tests. The scene owns the camera it plays through, so which entity that
// is, and what matrix it gives, are both answerable with no GPU and no window.
//
// The fly camera is here too. It is the other half of the same question: the
// runtime flies an entity now, so the angles it keeps and the rotation it writes
// have to agree with what the camera reads back out.

#include "check.h"
#include "editor/fly_camera.h"
#include "math/transform.h"
#include "scene/camera.h"
#include "scene/components.h"
#include "scene/world.h"

#include <cmath>

namespace {

    using test::check;
    using test::section;
    namespace sc = engine::scene;

    /// How close two floats have to be. Degrees and meters, not ULPs.
    constexpr float kTolerance = 1.0e-3F;

    [[nodiscard]] bool near_enough(float a, float b) { return std::fabs(a - b) < kTolerance; }

    [[nodiscard]] bool near_enough(const engine::Vec3& a, const engine::Vec3& b) {
        return near_enough(a.x, b.x) && near_enough(a.y, b.y) && near_enough(a.z, b.z);
    }

    /// Adds a camera entity at a pose, and returns it.
    entt::entity add_camera(sc::World& world, const engine::Transform& pose, bool primary) {
        const entt::entity entity = world.create();
        world.set_local(entity, pose);
        world.registry().emplace<sc::Camera>(entity, sc::Camera{ .primary = primary });
        world.update();
        return entity;
    }

    void test_a_world_with_no_camera() {
        section("a world with no camera");

        sc::World world;
        check(sc::primary_camera(world) == entt::null, "there is no camera to find");

        // An entity that is not a camera is not a camera, however many there are.
        (void)world.create();
        (void)world.create();
        check(sc::primary_camera(world) == entt::null, "other entities are not cameras");
    }

    void test_the_primary_flag_chooses() {
        section("which camera the game plays through");

        {
            sc::World world;
            const entt::entity only = add_camera(world, {}, true);
            check(sc::primary_camera(world) == only, "one camera is the one");
        }
        {
            // The flag cleared on the only camera. Refusing to draw would look
            // like a broken renderer, so the first camera still wins.
            sc::World world;
            const entt::entity only = add_camera(world, {}, false);
            check(sc::primary_camera(world) == only,
                  "a camera with the flag cleared is still used when it is the only one");
        }
        {
            sc::World world;
            (void)add_camera(world, {}, false);
            const entt::entity wanted = add_camera(world, {}, true);
            check(sc::primary_camera(world) == wanted, "the primary one wins over an earlier one");
        }
        {
            // Two claim it. The first is taken, and primary_camera reports that
            // it had a choice to make.
            sc::World world;
            const entt::entity first = add_camera(world, {}, true);
            (void)add_camera(world, {}, true);
            check(sc::primary_camera(world) == first, "the first of two primaries wins");
        }
    }

    void test_the_pose_comes_from_the_entity() {
        section("where a camera stands and which way it looks");

        sc::World world;
        const entt::entity camera = add_camera(world, { .position = { 1.0F, 2.0F, 3.0F } }, true);

        engine::Vec3 position{};
        engine::Vec3 forward{};
        sc::camera_pose(world, camera, position, forward);
        check(near_enough(position, engine::Vec3{ 1.0F, 2.0F, 3.0F }), "the position is the entity's");
        check(near_enough(forward, engine::Vec3{ 0.0F, 0.0F, -1.0F }),
              "an unturned camera looks down -Z, per DESIGN.md section 3");

        // A quarter turn to the left about up. -Z turns into -X.
        const engine::Quat quarter =
            glm::angleAxis(glm::radians(90.0F), engine::Vec3{ 0.0F, 1.0F, 0.0F });
        world.set_local(camera, { .position = { 1.0F, 2.0F, 3.0F }, .rotation = quarter });
        world.update();
        sc::camera_pose(world, camera, position, forward);
        check(near_enough(forward, engine::Vec3{ -1.0F, 0.0F, 0.0F }),
              "a quarter turn about up looks down -X");
    }

    /**
     * A parented camera reads its world pose, not its local one.
     *
     * This is what makes a camera on a moving rig work, and it is the case a
     * view matrix built from the local transform would get wrong while every
     * test above still passed.
     */
    void test_a_parented_camera_uses_its_world_pose() {
        section("a camera under a parent");

        sc::World world;
        const entt::entity rig = world.create();
        world.set_local(rig, { .position = { 10.0F, 0.0F, 0.0F } });

        const entt::entity camera = add_camera(world, { .position = { 0.0F, 1.0F, 0.0F } }, true);
        check(world.set_parent(camera, rig), "the camera attaches to the rig");
        world.update();

        engine::Vec3 position{};
        engine::Vec3 forward{};
        sc::camera_pose(world, camera, position, forward);
        check(near_enough(position, engine::Vec3{ 10.0F, 1.0F, 0.0F }),
              "the pose is where the rig put it");
    }

    /**
     * The matrix puts what is in front of the camera on screen.
     *
     * Reverse-Z means the near plane is depth 1 and distance falls towards 0,
     * which is the convention every pass in the engine reads. A matrix built
     * the other way up would draw a picture that looks right until something
     * needs the depth.
     */
    void test_the_matrix_projects() {
        section("the matrix a camera gives");

        sc::World world;
        const entt::entity camera = add_camera(world, { .position = { 0.0F, 0.0F, 0.0F } }, true);
        const engine::Mat4 clip = sc::clip_from_world(world, camera, 16.0F / 9.0F);

        // Two metres in front, which is down -Z.
        const engine::Vec4 near_point = clip * engine::Vec4{ 0.0F, 0.0F, -2.0F, 1.0F };
        const engine::Vec4 far_point = clip * engine::Vec4{ 0.0F, 0.0F, -200.0F, 1.0F };
        check(near_point.w > 0.0F, "a point in front has a positive w");
        check(near_enough(near_point.x / near_point.w, 0.0F) &&
                  near_enough(near_point.y / near_point.w, 0.0F),
              "a point straight ahead lands in the middle of the screen");

        const float near_depth = near_point.z / near_point.w;
        const float far_depth = far_point.z / far_point.w;
        check(near_depth > far_depth, "reverse-Z: nearer is a larger depth");
        check(near_depth < 1.0F && far_depth > 0.0F, "both sit inside the depth range");

        // Behind the camera. A negative w is what the clip stage rejects.
        const engine::Vec4 behind = clip * engine::Vec4{ 0.0F, 0.0F, 2.0F, 1.0F };
        check(behind.w < 0.0F, "a point behind the camera has a negative w");
    }

    /// The field of view is the vertical one, so a wider one sees more.
    void test_the_field_of_view_widens() {
        section("the field of view");

        sc::World world;
        const entt::entity camera = add_camera(world, {}, true);

        auto height_at = [&world, camera](float fov) {
            world.registry().get<sc::Camera>(camera).fov_degrees = fov;
            const engine::Mat4 clip = sc::clip_from_world(world, camera, 1.0F);
            const engine::Vec4 point = clip * engine::Vec4{ 0.0F, 1.0F, -5.0F, 1.0F };
            return point.y / point.w;
        };

        const float narrow = height_at(30.0F);
        const float wide = height_at(90.0F);
        check(std::fabs(wide) < std::fabs(narrow),
              "the same point sits nearer the middle through a wider lens");
    }

    /**
     * A fly camera writes a rotation it can read back.
     *
     * The runtime flies a camera entity now: the angles go out as a quaternion
     * and come back the next time something seeds from that entity. A wrong
     * multiply order rolls the camera, and this is what catches it.
     */
    void test_the_fly_camera_round_trips() {
        section("the fly camera and the rotation it writes");

        for (const float yaw : { 0.0F, 35.0F, -125.0F, 179.0F }) {
            for (const float pitch : { 0.0F, -8.0F, 42.0F, -70.0F }) {
                engine::editor::FlyCamera written{ .position = { 1.0F, 2.0F, 3.0F },
                                                   .yaw = yaw,
                                                   .pitch = pitch };
                engine::editor::FlyCamera read;
                engine::editor::seed_fly_camera(read, engine::editor::fly_transform(written));

                check(near_enough(read.position, written.position), "the position came back");
                check(near_enough(read.yaw, yaw) && near_enough(read.pitch, pitch),
                      "the angles came back");
            }
        }
    }

    /**
     * The fly camera and the scene camera agree about which way is forward.
     *
     * Two ways of saying the same thing: the fly camera builds a rotation from
     * two angles, and `camera_pose` reads a direction out of a matrix. If they
     * disagreed, flying the runtime camera would aim the throw somewhere else.
     */
    void test_both_forwards_agree() {
        section("the fly camera and the scene camera agree");

        sc::World world;
        const entt::entity camera = add_camera(world, {}, true);

        const engine::editor::FlyCamera fly{ .yaw = 35.0F, .pitch = -8.0F };
        world.set_local(camera, engine::editor::fly_transform(fly));
        world.update();

        engine::Vec3 position{};
        engine::Vec3 forward{};
        sc::camera_pose(world, camera, position, forward);
        check(near_enough(forward, engine::editor::fly_forward(fly)),
              "the entity looks where the fly camera says it does");
    }

} // namespace

int main() {
    test_a_world_with_no_camera();
    test_the_primary_flag_chooses();
    test_the_pose_comes_from_the_entity();
    test_a_parented_camera_uses_its_world_pose();
    test_the_matrix_projects();
    test_the_field_of_view_widens();
    test_the_fly_camera_round_trips();
    test_both_forwards_agree();
    return test::report();
}
