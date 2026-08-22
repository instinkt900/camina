// M5.7a tests for the frustum extraction and the sphere test.
//
// Every one of these runs with no device, for the reason test_render_graph.cpp
// gives: the arithmetic is the part most worth testing and it needs no GPU. A
// sign error in one plane culls the whole scene or culls nothing, and neither
// failure points back here. See issue #62.

#include "check.h"
#include "math/bounds.h"
#include "math/frustum.h"

#include <cstdlib>

namespace {

    using engine::Frustum;
    using engine::frustum_contains_sphere;
    using engine::frustum_from_view_projection;
    using engine::kFrustumPlanes;
    using engine::Mat4;
    using engine::Vec3;
    using test::check;

    /// The camera the runtime uses: infinite reverse-Z, 60 degrees, 16 by 9.
    constexpr float kNear = 0.1F;
    constexpr float kFovY = 1.0471975512F; // 60 degrees in radians.
    constexpr float kAspect = 16.0F / 9.0F;

    /// A camera at the origin looking down -Z, which is forward per DESIGN.md
    /// section 3.
    [[nodiscard]] Mat4 camera_at_origin() {
        const Mat4 projection = engine::perspective_reverse_z(kFovY, kAspect, kNear);
        const Mat4 view = glm::lookAt(Vec3{ 0.0F, 0.0F, 0.0F }, Vec3{ 0.0F, 0.0F, -1.0F },
                                      Vec3{ 0.0F, 1.0F, 0.0F });
        return projection * view;
    }

    /**
     * Every plane comes out unit length, apart from a degenerate one.
     *
     * The sphere test compares a signed distance against a radius in world
     * units. A plane that is not normalized makes that comparison meaningless
     * rather than wrong in an obvious way.
     */
    void planes_are_normalized() {
        const Frustum frustum = frustum_from_view_projection(camera_at_origin());
        for (std::size_t i = 0; i < kFrustumPlanes; ++i) {
            const float length = glm::length(frustum.planes[i].normal);
            // The far plane of an infinite projection is degenerate and carries
            // a zero normal on purpose. See frustum_from_view_projection.
            const bool ok = std::abs(length - 1.0F) < 1.0e-4F || length == 0.0F;
            check(ok, "a frustum plane is neither unit length nor degenerate");
        }
    }

    /**
     * A point straight ahead is inside, and one straight behind is not.
     *
     * This is the test that catches the reverse-Z mistake. Depth runs from 1 at
     * the near plane to 0 at infinity, so the row that gives the near plane
     * under a conventional range gives the far plane here. Swapping the two
     * leaves a frustum that contains nothing, and this fails.
     */
    void ahead_is_inside_and_behind_is_not() {
        const Frustum frustum = frustum_from_view_projection(camera_at_origin());
        check(frustum_contains_sphere(frustum, Vec3{ 0.0F, 0.0F, -10.0F }, 0.0F),
              "a point 10 meters ahead is not inside the frustum");
        check(!frustum_contains_sphere(frustum, Vec3{ 0.0F, 0.0F, 10.0F }, 0.0F),
              "a point 10 meters behind the camera is inside the frustum");
    }

    /**
     * A point nearer than the near plane is outside.
     *
     * The near plane is the one an infinite projection still bounds, so this is
     * the half of the depth pair that has to be right.
     */
    void nearer_than_near_is_outside() {
        const Frustum frustum = frustum_from_view_projection(camera_at_origin());
        check(!frustum_contains_sphere(frustum, Vec3{ 0.0F, 0.0F, -0.01F }, 0.0F),
              "a point in front of the near plane is inside the frustum");
    }

    /**
     * A far away point straight ahead is still inside.
     *
     * The projection is infinite, so there is no far plane to fall outside of.
     * A degenerate plane that culled would show up here.
     */
    void far_away_is_still_inside() {
        const Frustum frustum = frustum_from_view_projection(camera_at_origin());
        check(frustum_contains_sphere(frustum, Vec3{ 0.0F, 0.0F, -100000.0F }, 0.0F),
              "an infinite projection culled a distant point");
    }

    /**
     * A point far to one side is outside, and the sides are not swapped.
     *
     * Testing both sides separately is what catches a left and right that are
     * the wrong way round. A single-sided test passes with them swapped.
     */
    void wide_of_the_view_is_outside() {
        const Frustum frustum = frustum_from_view_projection(camera_at_origin());
        // At 10 meters ahead with a 60 degree vertical field and 16 by 9, the
        // half width is about 10 meters. So 40 is well outside on either side.
        check(!frustum_contains_sphere(frustum, Vec3{ 40.0F, 0.0F, -10.0F }, 0.0F),
              "a point far to the right is inside the frustum");
        check(!frustum_contains_sphere(frustum, Vec3{ -40.0F, 0.0F, -10.0F }, 0.0F),
              "a point far to the left is inside the frustum");
        check(!frustum_contains_sphere(frustum, Vec3{ 0.0F, 40.0F, -10.0F }, 0.0F),
              "a point far above is inside the frustum");
        check(!frustum_contains_sphere(frustum, Vec3{ 0.0F, -40.0F, -10.0F }, 0.0F),
              "a point far below is inside the frustum");
    }

    /**
     * A radius large enough to reach the frustum keeps the sphere.
     *
     * This is the whole point of testing a sphere rather than a point. A lamp
     * whose center is off screen still lights what is on screen. Culling it by
     * its center would put a dark band at the edge of the view.
     */
    void a_radius_that_reaches_is_kept() {
        const Frustum frustum = frustum_from_view_projection(camera_at_origin());
        const Vec3 off_to_the_side{ 40.0F, 0.0F, -10.0F };
        check(!frustum_contains_sphere(frustum, off_to_the_side, 0.0F),
              "the center should be outside for this test to mean anything");
        check(frustum_contains_sphere(frustum, off_to_the_side, 60.0F),
              "a sphere wide enough to reach the frustum was culled");
    }

    /**
     * The camera turning moves what is inside.
     *
     * The planes come out of the view projection, so a rotated camera has to
     * produce rotated planes. A frustum built from the projection alone would
     * pass every test above and fail this one.
     */
    void turning_the_camera_moves_the_frustum() {
        const Mat4 projection = engine::perspective_reverse_z(kFovY, kAspect, kNear);
        // Looking down +X instead of -Z.
        const Mat4 view = glm::lookAt(Vec3{ 0.0F, 0.0F, 0.0F }, Vec3{ 1.0F, 0.0F, 0.0F },
                                      Vec3{ 0.0F, 1.0F, 0.0F });
        const Frustum frustum = frustum_from_view_projection(projection * view);
        check(frustum_contains_sphere(frustum, Vec3{ 10.0F, 0.0F, 0.0F }, 0.0F),
              "a point along the new forward is not inside");
        check(!frustum_contains_sphere(frustum, Vec3{ 0.0F, 0.0F, -10.0F }, 0.0F),
              "a point along the old forward is still inside");
    }

    /**
     * The camera moving moves what is inside.
     *
     * Together with the test above this covers the whole view matrix. A frustum
     * that ignored the translation would keep a light behind the camera.
     */
    void moving_the_camera_moves_the_frustum() {
        const Mat4 projection = engine::perspective_reverse_z(kFovY, kAspect, kNear);
        const Mat4 view = glm::lookAt(Vec3{ 0.0F, 0.0F, 100.0F }, Vec3{ 0.0F, 0.0F, 99.0F },
                                      Vec3{ 0.0F, 1.0F, 0.0F });
        const Frustum frustum = frustum_from_view_projection(projection * view);
        // The camera sits at z = 100 and looks down -Z. So z = 50 is 50 meters
        // ahead of it, and z = 150 is 50 meters behind it. Both would fall on
        // the other side for a camera at the origin, which is what makes this a
        // test of the translation.
        check(frustum_contains_sphere(frustum, Vec3{ 0.0F, 0.0F, 50.0F }, 0.0F),
              "a point ahead of the moved camera is not inside");
        check(!frustum_contains_sphere(frustum, Vec3{ 0.0F, 0.0F, 150.0F }, 0.0F),
              "a point behind the moved camera is inside");
    }

    /**
     * An orthographic reverse-Z volume gives six real planes.
     *
     * The shadow cascades are orthographic, and every test above drives a
     * perspective matrix. The two differ in the part most likely to be wrong:
     * under an infinite perspective the far plane is degenerate and keeps
     * everything, and here it is a real plane that has to cull. A near or far
     * plane that came out degenerate for the ortho case would cull nothing along
     * depth, and the shadow map would look right while the pass did no work.
     *
     * The light stands 10 meters back along +Z and looks down -Z. Its volume is
     * 4 meters across and runs from 1 meter to 20 meters in front of it.
     */
    [[nodiscard]] Frustum light_volume() {
        const Mat4 projection = engine::orthographic_reverse_z(2.0F, 2.0F, 1.0F, 20.0F);
        const Mat4 view = glm::lookAt(Vec3{ 0.0F, 0.0F, 10.0F }, Vec3{ 0.0F, 0.0F, 9.0F },
                                      engine::world_up);
        return frustum_from_view_projection(projection * view);
    }

    void an_orthographic_volume_has_six_real_planes() {
        const Frustum frustum = light_volume();
        for (std::size_t i = 0; i < kFrustumPlanes; ++i) {
            const float length = glm::length(frustum.planes[i].normal);
            check(std::abs(length - 1.0F) < 1.0e-4F,
                  "an orthographic plane is degenerate, so it culls nothing");
        }
    }

    /**
     * The orthographic volume culls along depth and across, and keeps the middle.
     *
     * The depth pair is the reason this test exists. Under reverse-Z the row that
     * gives the near plane is not the one a conventional range would use, and an
     * orthographic matrix is where both depth planes are real, so both can be
     * wrong here in a way the perspective tests cannot see.
     */
    void an_orthographic_volume_culls_outside_itself() {
        const Frustum frustum = light_volume();
        // The light is at z = 10 looking down -Z, so its volume covers z from 9
        // down to -10, and x and y from -2 to 2.
        check(frustum_contains_sphere(frustum, Vec3{ 0.0F, 0.0F, 0.0F }, 0.0F),
              "the middle of the light volume is outside it");
        check(!frustum_contains_sphere(frustum, Vec3{ 0.0F, 0.0F, 20.0F }, 0.0F),
              "a point behind the light is inside its volume");
        check(!frustum_contains_sphere(frustum, Vec3{ 0.0F, 0.0F, -50.0F }, 0.0F),
              "a point past the far plane is inside the volume");
        check(!frustum_contains_sphere(frustum, Vec3{ 10.0F, 0.0F, 0.0F }, 0.0F),
              "a point wide of the volume is inside it");
        check(!frustum_contains_sphere(frustum, Vec3{ 0.0F, 10.0F, 0.0F }, 0.0F),
              "a point above the volume is inside it");
    }

    /**
     * An orthographic volume does not narrow with distance.
     *
     * This is what separates it from a perspective one, and it is the property a
     * cascade depends on. A perspective matrix in place of the ortho one would
     * pass every check above and fail this.
     */
    void an_orthographic_volume_does_not_narrow() {
        const Frustum frustum = light_volume();
        // Both are 1.5 meters off the axis, which is inside the 2 meter half
        // width, and they sit at opposite ends of the depth range.
        check(frustum_contains_sphere(frustum, Vec3{ 1.5F, 0.0F, 8.0F }, 0.0F),
              "a point near the front of the volume is outside it");
        check(frustum_contains_sphere(frustum, Vec3{ 1.5F, 0.0F, -9.0F }, 0.0F),
              "the volume narrows with distance, so it is not orthographic");
    }

    /**
     * The whole reason the box test exists.
     *
     * A long thin slab, well off to one side of the light volume but reaching
     * along it. The sphere around that slab is huge and overlaps the volume, so
     * the sphere test keeps it. The box does not overlap, so the box test drops
     * it. Measured on Intel Sponza, this is 493 thousand triangles a frame.
     */
    void a_long_box_beside_the_volume_is_dropped() {
        const Frustum frustum = light_volume();
        // Centered 6 meters to the right of a volume 2 meters wide, and 18
        // meters long along Z.
        const Vec3 center{ 6.0F, 0.0F, 0.0F };
        const Vec3 axis_x{ 0.5F, 0.0F, 0.0F };
        const Vec3 axis_y{ 0.0F, 0.5F, 0.0F };
        const Vec3 axis_z{ 0.0F, 0.0F, 9.0F };

        // The sphere around it reaches back past the volume, so the loose test
        // cannot reject it. This check is what makes the next one mean
        // something: without it a box test that dropped everything would pass.
        const float radius = glm::length(axis_x + axis_y + axis_z);
        check(frustum_contains_sphere(frustum, center, radius),
              "the sphere around the slab overlaps the volume");
        check(!frustum_contains_box(frustum, center, axis_x, axis_y, axis_z),
              "the box beside the volume is dropped");
    }

    /// A box inside the volume is kept, which is the case that must never break.
    void a_box_inside_the_volume_is_kept() {
        const Frustum frustum = light_volume();
        check(frustum_contains_box(frustum, Vec3{ 0.0F, 0.0F, 0.0F }, Vec3{ 0.5F, 0.0F, 0.0F },
                                   Vec3{ 0.0F, 0.5F, 0.0F }, Vec3{ 0.0F, 0.0F, 0.5F }),
              "a small box at the middle of the volume is kept");
    }

    /**
     * A box that pokes in through one face is kept.
     *
     * The test is wrong in the cheap direction on purpose, the same way the
     * sphere test is. Dropping this one would be the dangerous failure: the
     * mesh is partly in view and the hole shows at the edge of the frame.
     */
    void a_box_that_straddles_a_plane_is_kept() {
        const Frustum frustum = light_volume();
        // Centered outside the 2 meter half width, reaching back inside it.
        check(frustum_contains_box(frustum, Vec3{ 2.5F, 0.0F, 0.0F }, Vec3{ 1.0F, 0.0F, 0.0F },
                                   Vec3{ 0.0F, 0.5F, 0.0F }, Vec3{ 0.0F, 0.0F, 0.5F }),
              "a box reaching in through the side is kept");
    }

    /**
     * Rotation is carried by the axes rather than by a separate matrix.
     *
     * The same slab turned to lie along the volume instead of beside it has to
     * come back inside. A test that read only the axis lengths, or that took
     * the longest axis as a radius, would answer the same for both and this is
     * what separates them.
     */
    void turning_a_box_changes_the_answer() {
        const Frustum frustum = light_volume();
        const Vec3 center{ 6.0F, 0.0F, 0.0F };
        const Vec3 thin_x{ 0.5F, 0.0F, 0.0F };
        const Vec3 thin_y{ 0.0F, 0.5F, 0.0F };
        check(!frustum_contains_box(frustum, center, thin_x, thin_y, Vec3{ 0.0F, 0.0F, 9.0F }),
              "the slab reaching along Z stays outside");
        // The same box turned a quarter turn, so its long axis now points at
        // the volume and reaches into it.
        check(frustum_contains_box(frustum, center, Vec3{ 9.0F, 0.0F, 0.0F }, thin_y, thin_x),
              "the slab turned to reach the volume is kept");
    }

    /**
     * Each of the three axes has to reach on its own.
     *
     * Three boxes, each centered outside one face of the volume and reaching
     * back in along a different axis. Dropping any one axis from the sum makes
     * that box look thin and culls it, and the other two checks still pass.
     * A first version of these tests summed the axes in a way that let the Z
     * term go missing with nothing failing.
     */
    void every_axis_reaches_on_its_own() {
        const Frustum frustum = light_volume();
        const Vec3 none{ 0.0F, 0.0F, 0.0F };

        // Past the right face, reaching back through it along X.
        check(frustum_contains_box(frustum, Vec3{ 5.0F, 0.0F, 0.0F }, Vec3{ 4.0F, 0.0F, 0.0F },
                                   none, none),
              "the X axis reaches into the volume");
        // Past the top face, reaching back down along Y.
        check(frustum_contains_box(frustum, Vec3{ 0.0F, 5.0F, 0.0F }, none,
                                   Vec3{ 0.0F, 4.0F, 0.0F }, none),
              "the Y axis reaches into the volume");
        // Past the far face, reaching back along Z. The volume looks down -Z
        // from z = 10 and ends 20 meters along, so -14 is outside it.
        check(frustum_contains_box(frustum, Vec3{ 0.0F, 0.0F, -14.0F }, none, none,
                                   Vec3{ 0.0F, 0.0F, 6.0F }),
              "the Z axis reaches into the volume");
    }

    /// A box with no extent is a point, and answers like one.
    void a_degenerate_box_is_a_point() {
        const Frustum frustum = light_volume();
        const Vec3 none{ 0.0F, 0.0F, 0.0F };
        check(frustum_contains_box(frustum, Vec3{ 0.0F, 0.0F, 0.0F }, none, none, none),
              "a point inside the volume is kept");
        check(!frustum_contains_box(frustum, Vec3{ 9.0F, 0.0F, 0.0F }, none, none, none),
              "a point outside the volume is dropped");
    }

    /**
     * The world box and the world sphere agree about where the mesh is.
     *
     * They are two bounds on one box, so the sphere has to hold every corner
     * the box describes. A sign error in either one shows up as a corner
     * outside the sphere.
     */
    void the_world_box_sits_inside_the_world_sphere() {
        const Mat4 world = glm::translate(Mat4{ 1.0F }, Vec3{ 3.0F, -2.0F, 1.0F }) *
                           glm::rotate(Mat4{ 1.0F }, 0.7F, Vec3{ 0.3F, 1.0F, 0.2F }) *
                           glm::scale(Mat4{ 1.0F }, Vec3{ 2.0F, 0.5F, 3.0F });
        const Vec3 min{ -1.0F, -2.0F, -0.5F };
        const Vec3 max{ 4.0F, 1.0F, 2.5F };

        const engine::Obb box = engine::world_box_from_bounds(world, min, max);
        const engine::Sphere sphere = engine::world_sphere_from_bounds(world, min, max);

        check(glm::length(box.center - sphere.center) < 1e-4F,
              "the box and the sphere share a center");

        bool every_corner_inside = true;
        for (int corner = 0; corner < 8; ++corner) {
            const float sx = (corner & 1) != 0 ? 1.0F : -1.0F;
            const float sy = (corner & 2) != 0 ? 1.0F : -1.0F;
            const float sz = (corner & 4) != 0 ? 1.0F : -1.0F;
            const Vec3 point =
                box.center + (box.axis_x * sx) + (box.axis_y * sy) + (box.axis_z * sz);
            // A small tolerance, because the sphere radius is the longest of
            // four corner offsets and the compare is against that same length.
            if (glm::length(point - sphere.center) > sphere.radius + 1e-3F) {
                every_corner_inside = false;
            }
        }
        check(every_corner_inside, "every corner of the box is inside the sphere");
    }

} // namespace

int main() {
    planes_are_normalized();
    ahead_is_inside_and_behind_is_not();
    nearer_than_near_is_outside();
    far_away_is_still_inside();
    wide_of_the_view_is_outside();
    a_radius_that_reaches_is_kept();
    turning_the_camera_moves_the_frustum();
    moving_the_camera_moves_the_frustum();
    an_orthographic_volume_has_six_real_planes();
    an_orthographic_volume_culls_outside_itself();
    an_orthographic_volume_does_not_narrow();
    a_long_box_beside_the_volume_is_dropped();
    a_box_inside_the_volume_is_kept();
    a_box_that_straddles_a_plane_is_kept();
    turning_a_box_changes_the_answer();
    every_axis_reaches_on_its_own();
    a_degenerate_box_is_a_point();
    the_world_box_sits_inside_the_world_sphere();
    return test::report();
}
