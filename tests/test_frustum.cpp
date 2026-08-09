// M5.7a tests for the frustum extraction and the sphere test.
//
// Every one of these runs with no device, for the reason test_render_graph.cpp
// gives: the arithmetic is the part most worth testing and it needs no GPU. A
// sign error in one plane culls the whole scene or culls nothing, and neither
// failure points back here. See issue #62.

#include "check.h"
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
    return test::report();
}
