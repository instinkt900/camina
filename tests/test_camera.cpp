// M9.5a tests. The scene owns the camera it plays through, so which entity that
// is, and what matrix it gives, are both answerable with no GPU and no window.
//
// The fly camera is here too. It is the other half of the same question: the
// runtime flies an entity now, so the angles it keeps and the rotation it writes
// have to agree with what the camera reads back out.

#include "check.h"
#include "editor/camera_lines.h"
#include "editor/fly_camera.h"
#include "math/ray.h"
#include "math/transform.h"
#include "scene/camera.h"
#include "scene/components.h"
#include "platform/input.h"
#include "platform/window.h"
#include "scene/world.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

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

    /**
     * The interface does not take the mouse from a camera in a viewport.
     *
     * ImGui claims the mouse for every window it owns, and an editor viewport
     * is one of its windows. Taking that claim at face value means the camera
     * can never be turned, which is what M9.5b shipped: the look button was
     * discarded on every frame the pointer was over the picture.
     */
    void test_who_gets_the_mouse() {
        section("who gets the mouse over a viewport");

        using engine::editor::mouse_consumed_by_ui;

        check(!mouse_consumed_by_ui(false, false, false),
              "with no claim the camera has it anyway");
        check(mouse_consumed_by_ui(true, false, false),
              "a claim away from the viewport holds, so a panel keeps its drag");
        check(!mouse_consumed_by_ui(true, true, false),
              "a claim over the viewport does not, or the camera cannot turn");
        check(!mouse_consumed_by_ui(true, false, true),
              "a look already in progress keeps the mouse off the panel");
        check(!mouse_consumed_by_ui(true, true, true), "and both together as well");
    }

    /// Presses a set of keys and a button, and hands them to an Input.
    [[nodiscard]] engine::platform::Input pressed_input(bool look, engine::platform::Key key) {
        engine::platform::Input input;
        engine::editor::bind_fly_actions(input);

        engine::platform::InputFrame frame;
        frame.focused = true;
        frame.keys.at(static_cast<std::size_t>(key)) = true;
        frame.mouse_buttons.at(static_cast<std::size_t>(engine::platform::MouseButton::Right)) =
            look;
        input.update(frame);
        return input;
    }

    /**
     * The editor moves only while the look button is held, and the runtime
     * moves whenever the keys are down.
     *
     * Two rules in one function, chosen by one field. The editor needs the
     * letter keys free while the button is up, and a debug camera in a game has
     * nothing else those keys could mean.
     */
    void test_movement_needs_the_look_button() {
        section("what the movement keys need");

        const engine::platform::Window no_window;
        constexpr float kStep = 1.0F / 60.0F;

        {
            // The runtime rule. Forward moves with no button held.
            engine::editor::FlyCamera camera;
            const engine::platform::Input input = pressed_input(false, engine::platform::Key::W);
            check(engine::editor::update_fly_camera(camera, no_window, input, kStep),
                  "the runtime camera moves on the key alone");
            check(camera.position.z < 0.0F, "and it moved forward, which is -Z");
        }
        {
            // The editor rule. The same key does nothing on its own.
            engine::editor::FlyCamera camera{ .move_needs_look = true };
            const engine::platform::Input input = pressed_input(false, engine::platform::Key::W);
            check(!engine::editor::update_fly_camera(camera, no_window, input, kStep),
                  "the editor camera stands still without the look button");
            check(camera.position == engine::Vec3{ 0.0F, 0.0F, 0.0F }, "nothing moved at all");
        }
        {
            // And moves once the button is held.
            engine::editor::FlyCamera camera{ .move_needs_look = true };
            const engine::platform::Input input = pressed_input(true, engine::platform::Key::W);
            check(engine::editor::update_fly_camera(camera, no_window, input, kStep),
                  "the editor camera moves while the look is held");
            check(camera.position.z < 0.0F, "and it moved forward");
        }
    }

    /**
     * The middle of the far end of a wireframe.
     *
     * The four lines that start at the camera end on the four corners, so the
     * mean of those ends is the middle. Measuring the furthest point instead
     * would measure the spread, which is the aspect rather than the direction.
     *
     * @param lines The wireframe to read.
     * @param apex Where the camera stands.
     * @return The middle of the far end.
     */
    [[nodiscard]] engine::Vec3 far_centre(const std::vector<engine::physics::DebugLine>& lines,
                                          const engine::Vec3& apex) {
        engine::Vec3 total{ 0.0F, 0.0F, 0.0F };
        float count = 0.0F;
        for (const engine::physics::DebugLine& line : lines) {
            if (near_enough(line.from, apex)) {
                total += line.to;
                count += 1.0F;
            }
        }
        return count > 0.0F ? total / count : total;
    }

    /**
     * The wireframe stands where the camera stands and opens the way it looks.
     *
     * A picture cannot say this. A frustum drawn at the wrong scale, pointing
     * backwards, or centred on the origin all look like a yellow shape in a
     * room, and the mistake only shows when somebody tries to line a camera up
     * with it.
     */
    void test_the_camera_wireframe() {
        section("the wireframe of a camera");

        sc::World world;
        const engine::Vec3 where{ 2.0F, 1.5F, -3.0F };
        const entt::entity camera = add_camera(world, { .position = where }, true);
        world.registry().get<sc::Camera>(camera).fov_degrees = 60.0F;

        std::vector<engine::physics::DebugLine> lines;
        engine::editor::camera_lines(world, camera, 1.0F, lines);
        check(lines.size() == 10, "four edges, four sides of the far end, and the up bar");

        // The four lines that start at the camera are the edges of the pyramid.
        std::size_t from_apex = 0;
        for (const engine::physics::DebugLine& line : lines) {
            if (near_enough(line.from, where)) {
                ++from_apex;
            }
        }
        check(from_apex == 4, "four lines start where the camera stands");

        // The far end sits one length down -Z, which is forward for an unturned
        // camera, and its half height is the tangent of half the field of view.
        const float expected_half =
            engine::editor::kCameraLinesLength * std::tan(glm::radians(30.0F));
        float highest = 0.0F;
        for (const engine::physics::DebugLine& line : lines) {
            highest = std::max(highest, line.to.y - where.y);
        }
        check(near_enough(far_centre(lines, where),
                          where - engine::Vec3{ 0.0F, 0.0F, engine::editor::kCameraLinesLength }),
              "the far end is one length ahead, down -Z");
        check(highest > expected_half,
              "the up bar rises above the top of the far end, so up is readable");
        check(near_enough(highest, expected_half * 1.25F), "and it rises by a quarter of it");

        // A wider lens makes a wider shape. This is what catches a frustum built
        // from a fixed angle rather than from the camera.
        world.registry().get<sc::Camera>(camera).fov_degrees = 90.0F;
        std::vector<engine::physics::DebugLine> wider;
        engine::editor::camera_lines(world, camera, 1.0F, wider);

        const auto spread = [](const std::vector<engine::physics::DebugLine>& of) {
            float widest = 0.0F;
            for (const engine::physics::DebugLine& line : of) {
                widest = std::max(widest, std::fabs(line.to.x));
            }
            return widest;
        };
        check(spread(wider) > spread(lines), "a wider field of view draws a wider shape");
    }

    /// A turned camera takes its wireframe with it.
    void test_the_wireframe_turns_with_the_camera() {
        section("the wireframe of a turned camera");

        sc::World world;
        const engine::Quat quarter =
            glm::angleAxis(glm::radians(90.0F), engine::Vec3{ 0.0F, 1.0F, 0.0F });
        const entt::entity camera = add_camera(world, { .rotation = quarter }, true);

        std::vector<engine::physics::DebugLine> lines;
        engine::editor::camera_lines(world, camera, 1.0F, lines);

        // A quarter turn to the left looks down -X, so the middle of the far
        // end is there. The corners spread sideways from it, which after this
        // turn is along Z, so the middle is the thing to measure and the
        // furthest point is not.
        check(near_enough(far_centre(lines, engine::Vec3{ 0.0F, 0.0F, 0.0F }),
                          engine::Vec3{ -engine::editor::kCameraLinesLength, 0.0F, 0.0F }),
              "the far end followed the camera round to -X");
    }

    /**
     * A ray through the middle of the screen goes where the camera looks.
     *
     * Every mistake available here is silent. A ray built from the wrong depth
     * points backwards and picks nothing, and a Y that is flipped once too often
     * picks whatever is above what somebody clicked.
     */
    void test_a_ray_through_the_screen() {
        section("the ray through a point on the screen");

        sc::World world;
        const engine::Vec3 where{ 0.0F, 1.0F, 5.0F };
        const entt::entity camera = add_camera(world, { .position = where }, true);

        const engine::Mat4 clip = sc::clip_from_world(world, camera, 16.0F / 9.0F);
        const engine::Mat4 world_from_clip = glm::inverse(clip);

        const engine::Ray middle =
            engine::ray_through_ndc(world_from_clip, where, 0.0F, 0.0F);
        check(near_enough(middle.origin, where), "the ray starts at the camera");
        check(near_enough(middle.direction, engine::Vec3{ 0.0F, 0.0F, -1.0F }),
              "and the middle of the screen looks down -Z");

        // Vulkan puts -1 at the top of the picture, so a click there is a ray
        // that goes up in the world. This is the flip that is easy to get wrong.
        const engine::Ray top = engine::ray_through_ndc(world_from_clip, where, 0.0F, -1.0F);
        check(top.direction.y > 0.0F, "the top of the picture points up in the world");

        const engine::Ray bottom = engine::ray_through_ndc(world_from_clip, where, 0.0F, 1.0F);
        check(bottom.direction.y < 0.0F, "and the bottom points down");

        const engine::Ray right = engine::ray_through_ndc(world_from_clip, where, 1.0F, 0.0F);
        check(right.direction.x > 0.0F, "the right of the picture points right");
    }

    /// A pixel in a panel becomes a point on the picture.
    void test_a_pixel_becomes_a_point() {
        section("a pixel inside a panel");

        // A picture 800 by 400, drawn at 100, 50 inside the window.
        constexpr float kX = 100.0F;
        constexpr float kY = 50.0F;
        constexpr float kWidth = 800.0F;
        constexpr float kHeight = 400.0F;

        const engine::Vec2 middle =
            engine::ndc_from_pixel(kX + (kWidth / 2.0F), kY + (kHeight / 2.0F), kX, kY, kWidth,
                                   kHeight);
        check(near_enough(middle.x, 0.0F) && near_enough(middle.y, 0.0F),
              "the middle of the picture is the middle of the screen");

        const engine::Vec2 top_left = engine::ndc_from_pixel(kX, kY, kX, kY, kWidth, kHeight);
        check(near_enough(top_left.x, -1.0F), "the left edge is -1");
        check(near_enough(top_left.y, -1.0F), "and the top edge is -1, because Vulkan Y runs down");

        const engine::Vec2 bottom_right =
            engine::ndc_from_pixel(kX + kWidth, kY + kHeight, kX, kY, kWidth, kHeight);
        check(near_enough(bottom_right.x, 1.0F) && near_enough(bottom_right.y, 1.0F),
              "and the far corner is 1, 1");

        // A panel collapsed to nothing must not divide by it.
        const engine::Vec2 nothing = engine::ndc_from_pixel(kX, kY, kX, kY, 0.0F, 0.0F);
        check(near_enough(nothing.x, 0.0F) && near_enough(nothing.y, 0.0F),
              "a picture of no size gives the middle rather than an infinity");
    }

    /// The slab test, which is what a pick asks about every candidate.
    void test_the_box_test() {
        section("a ray against a box");

        const engine::Vec3 min{ -1.0F, -1.0F, -1.0F };
        const engine::Vec3 max{ 1.0F, 1.0F, 1.0F };
        float distance = 0.0F;

        const engine::Ray at_it{ .origin = { 0.0F, 0.0F, 5.0F },
                                 .direction = { 0.0F, 0.0F, -1.0F } };
        check(engine::ray_hits_box(at_it, min, max, distance), "a ray at the box hits it");
        check(near_enough(distance, 4.0F), "at the near face, four along");

        const engine::Ray away{ .origin = { 0.0F, 0.0F, 5.0F },
                                .direction = { 0.0F, 0.0F, 1.0F } };
        check(!engine::ray_hits_box(away, min, max, distance),
              "a ray pointing away misses, rather than hitting behind itself");

        const engine::Ray beside{ .origin = { 5.0F, 0.0F, 5.0F },
                                  .direction = { 0.0F, 0.0F, -1.0F } };
        check(!engine::ray_hits_box(beside, min, max, distance), "a ray beside the box misses");

        // Inside the box, the surface ahead is the one it leaves by. A hit at
        // zero here is what made every click in a room select the room: nothing
        // can be nearer than zero, so the room won every time.
        const engine::Ray inside{ .origin = { 0.0F, 0.0F, 0.0F },
                                  .direction = { 0.0F, 0.0F, -1.0F } };
        check(engine::ray_hits_box(inside, min, max, distance), "a ray inside the box hits");
        check(near_enough(distance, 1.0F), "at the far side, not at zero");

        // Behind the origin altogether, which the far side rule must not turn
        // into a hit.
        const engine::Ray past_it{ .origin = { 0.0F, 0.0F, -9.0F },
                                   .direction = { 0.0F, 0.0F, -1.0F } };
        check(!engine::ray_hits_box(past_it, min, max, distance),
              "a box entirely behind the origin is still a miss");

        // Parallel to two faces and outside them. Dividing by that direction
        // would give an infinity, and an infinity compares as a hit.
        const engine::Ray parallel{ .origin = { 0.0F, 9.0F, 5.0F },
                                    .direction = { 0.0F, 0.0F, -1.0F } };
        check(!engine::ray_hits_box(parallel, min, max, distance),
              "a ray parallel to a pair of faces and outside them misses");
    }

} // namespace

int main() {
    test_a_ray_through_the_screen();
    test_the_box_test();
    test_a_pixel_becomes_a_point();

    test_who_gets_the_mouse();
    test_movement_needs_the_look_button();

    test_the_camera_wireframe();
    test_the_wireframe_turns_with_the_camera();

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
