// M5.7c tests for the world-space sphere a mesh bound turns into.
//
// Every one of these runs with no device, for the reason test_frustum.cpp gives.
// The cull test itself is already covered there. What is new here is the step
// before it, and that step is where a radius comes out too small and drops a
// mesh that is on screen. See issue #176.

#include "check.h"
#include "math/bounds.h"
#include "math/frustum.h"

#include <cmath>

namespace {

    using engine::Mat4;
    using engine::Sphere;
    using engine::Vec3;
    using engine::world_sphere_from_bounds;
    using test::check;

    /// Close enough for a radius in meters, over boxes of about a meter.
    constexpr float kTolerance = 1.0e-4F;

    [[nodiscard]] bool near_equal(float a, float b) { return std::abs(a - b) < kTolerance; }

    [[nodiscard]] bool near_equal(const Vec3& a, const Vec3& b) {
        return near_equal(a.x, b.x) && near_equal(a.y, b.y) && near_equal(a.z, b.z);
    }

    /**
     * Every corner of the transformed box lands inside the sphere.
     *
     * This is the property the cull depends on, so it is checked directly rather
     * than through a radius somebody worked out by hand. A sphere that misses one
     * corner drops a mesh whose visible part is exactly that corner.
     */
    [[nodiscard]] bool sphere_holds_every_corner(const Mat4& world_from_local, const Vec3& min,
                                                 const Vec3& max) {
        const Sphere sphere = world_sphere_from_bounds(world_from_local, min, max);
        for (int i = 0; i < 8; ++i) {
            const Vec3 corner{ ((i & 1) != 0) ? max.x : min.x, ((i & 2) != 0) ? max.y : min.y,
                               ((i & 4) != 0) ? max.z : min.z };
            const Vec3 world{ world_from_local * engine::Vec4{ corner, 1.0F } };
            // A tolerance upward, because the radius is the exact distance to the
            // furthest corner and that corner sits on the surface.
            if (glm::distance(world, sphere.center) > sphere.radius + kTolerance) {
                return false;
            }
        }
        return true;
    }

    /**
     * A unit cube under the identity gives the half diagonal.
     *
     * The simplest case, and the one that catches a radius built from the whole
     * extent rather than the half extent. That mistake keeps every mesh, so
     * nothing else in the suite would see it.
     */
    void a_unit_cube_gives_its_half_diagonal() {
        const Sphere sphere =
            world_sphere_from_bounds(Mat4{ 1.0F }, Vec3{ -0.5F }, Vec3{ 0.5F });
        check(near_equal(sphere.center, Vec3{ 0.0F }), "a centered cube is not centered on zero");
        check(near_equal(sphere.radius, std::sqrt(3.0F) * 0.5F),
              "a unit cube does not give its half diagonal");
    }

    /**
     * A box away from its own origin keeps its own center.
     *
     * A cooked mesh is rarely centered on the origin of its local space. Reading
     * the radius off `max` alone, or taking the center as zero, both pass the
     * test above and fail this one.
     */
    void an_offset_box_centers_on_itself() {
        const Vec3 min{ 2.0F, 4.0F, 6.0F };
        const Vec3 max{ 4.0F, 6.0F, 8.0F };
        const Sphere sphere = world_sphere_from_bounds(Mat4{ 1.0F }, min, max);
        check(near_equal(sphere.center, Vec3{ 3.0F, 5.0F, 7.0F }),
              "an offset box does not center on itself");
        check(near_equal(sphere.radius, std::sqrt(3.0F)), "an offset box has the wrong radius");
        check(sphere_holds_every_corner(Mat4{ 1.0F }, min, max),
              "an offset box has a corner outside its sphere");
    }

    /**
     * Translation moves the center and leaves the radius alone.
     *
     * The translation is the column the linear part must not read. A radius that
     * grew with the translation would keep every mesh in a scene whose origin is
     * far away.
     */
    void translation_moves_only_the_center() {
        const Mat4 moved = glm::translate(Mat4{ 1.0F }, Vec3{ 100.0F, -50.0F, 25.0F });
        const Sphere sphere = world_sphere_from_bounds(moved, Vec3{ -0.5F }, Vec3{ 0.5F });
        check(near_equal(sphere.center, Vec3{ 100.0F, -50.0F, 25.0F }),
              "translation did not move the center");
        check(near_equal(sphere.radius, std::sqrt(3.0F) * 0.5F),
              "translation changed the radius");
    }

    /**
     * Rotating a cube changes nothing, and rotating a slab changes nothing either.
     *
     * A sphere is what makes this true, and it is the reason the test is a sphere
     * rather than an axis-aligned box in world space. A rotated slab grows an
     * axis-aligned box and does not grow this.
     */
    void rotation_keeps_the_radius() {
        const Mat4 turned = glm::rotate(Mat4{ 1.0F }, 0.7F, glm::normalize(Vec3{ 1.0F, 2.0F, 3.0F }));
        const Vec3 min{ -2.0F, -0.1F, -2.0F };
        const Vec3 max{ 2.0F, 0.1F, 2.0F };
        const Sphere flat = world_sphere_from_bounds(Mat4{ 1.0F }, min, max);
        const Sphere spun = world_sphere_from_bounds(turned, min, max);
        check(near_equal(flat.radius, spun.radius), "rotation changed the radius of a slab");
        check(sphere_holds_every_corner(turned, min, max),
              "a rotated slab has a corner outside its sphere");
    }

    /**
     * Scale grows the radius, and a non-uniform scale grows it by the axis that
     * reaches furthest.
     *
     * The case a single scale factor cannot express. Taking the smallest scale,
     * or an average of the three, underestimates and drops a mesh that is in
     * view.
     */
    void non_uniform_scale_follows_the_longest_axis() {
        const Mat4 stretched = glm::scale(Mat4{ 1.0F }, Vec3{ 1.0F, 1.0F, 10.0F });
        const Sphere sphere =
            world_sphere_from_bounds(stretched, Vec3{ -0.5F }, Vec3{ 0.5F });
        // Half extents become (0.5, 0.5, 5), so the corner distance is the length
        // of that.
        check(near_equal(sphere.radius, glm::length(Vec3{ 0.5F, 0.5F, 5.0F })),
              "a non-uniform scale gives the wrong radius");
        check(sphere_holds_every_corner(stretched, Vec3{ -0.5F }, Vec3{ 0.5F }),
              "a stretched cube has a corner outside its sphere");
    }

    /**
     * A shear still holds every corner.
     *
     * A shear is what a scene graph produces from a scaled parent and a rotated
     * child, so it is not a contrived matrix. Nothing about it is axis aligned in
     * either space.
     */
    void a_shear_still_holds_every_corner() {
        Mat4 shear{ 1.0F };
        shear[1][0] = 2.0F; // Local +Y leans two meters along world +X.
        check(sphere_holds_every_corner(shear, Vec3{ -0.5F }, Vec3{ 0.5F }),
              "a sheared cube has a corner outside its sphere");
    }

    /**
     * The cheap radius can come out too small, and this is a matrix where it does.
     *
     * Scaling the local half diagonal by the longest column of the matrix looks
     * like a safe upper bound and is not. When the three columns lean the same
     * way their signed sum is longer than any one of them, which is the same
     * thing as the largest singular value exceeding every column length.
     *
     * Without this check nothing in the suite says why the exact form is worth
     * the four lengths it costs.
     */
    void the_column_bound_can_come_out_too_small() {
        // Three columns that all lean along world +X. The matrix is not
        // singular: it flattens the box towards a line without reaching one.
        Mat4 skew{ 1.0F };
        skew[1][0] = 1.0F;
        skew[1][1] = 0.1F;
        skew[2][0] = 1.0F;
        skew[2][2] = 0.1F;

        const Vec3 min{ -0.5F };
        const Vec3 max{ 0.5F };
        check(sphere_holds_every_corner(skew, min, max),
              "a skewed cube has a corner outside its sphere");

        const float longest_column =
            std::max({ glm::length(Vec3{ skew[0] }), glm::length(Vec3{ skew[1] }),
                       glm::length(Vec3{ skew[2] }) });
        const float cheap = longest_column * glm::length((max - min) * 0.5F);
        const Sphere sphere = world_sphere_from_bounds(skew, min, max);
        check(sphere.radius > cheap, "the column bound is not too small on a matrix that skews");
    }

    /**
     * A degenerate box gives a point, and a point survives the frustum test.
     *
     * A cooked mesh with one vertex, or a mesh the cooker gave zero bounds,
     * must not become a radius that is not a number. That would compare false
     * against every plane and cull the whole scene.
     */
    void a_degenerate_box_gives_a_point() {
        const Sphere sphere =
            world_sphere_from_bounds(Mat4{ 1.0F }, Vec3{ 3.0F }, Vec3{ 3.0F });
        check(near_equal(sphere.center, Vec3{ 3.0F }), "a degenerate box lost its position");
        check(near_equal(sphere.radius, 0.0F), "a degenerate box has a radius");
    }

    /**
     * The sphere and the frustum test agree on a box in front of the camera.
     *
     * The two halves are tested apart, so one test puts them together. It is the
     * arrangement the mesh pass makes, and it catches a sphere in the wrong space.
     */
    void the_sphere_works_with_the_frustum() {
        const Mat4 projection = engine::perspective_reverse_z(1.0471975512F, 16.0F / 9.0F, 0.1F);
        const Mat4 view =
            glm::lookAt(Vec3{ 0.0F, 0.0F, 0.0F }, engine::world_forward, engine::world_up);
        const engine::Frustum frustum = engine::frustum_from_view_projection(projection * view);

        const Vec3 min{ -0.5F };
        const Vec3 max{ 0.5F };
        const Mat4 ahead = glm::translate(Mat4{ 1.0F }, Vec3{ 0.0F, 0.0F, -10.0F });
        const Mat4 behind = glm::translate(Mat4{ 1.0F }, Vec3{ 0.0F, 0.0F, 10.0F });

        const Sphere in_view = world_sphere_from_bounds(ahead, min, max);
        const Sphere out_of_view = world_sphere_from_bounds(behind, min, max);
        check(engine::frustum_contains_sphere(frustum, in_view.center, in_view.radius),
              "a box ten meters ahead is culled");
        check(!engine::frustum_contains_sphere(frustum, out_of_view.center, out_of_view.radius),
              "a box ten meters behind is kept");
    }

} // namespace

int main() {
    a_unit_cube_gives_its_half_diagonal();
    an_offset_box_centers_on_itself();
    translation_moves_only_the_center();
    rotation_keeps_the_radius();
    non_uniform_scale_follows_the_longest_axis();
    a_shear_still_holds_every_corner();
    the_column_bound_can_come_out_too_small();
    a_degenerate_box_gives_a_point();
    the_sphere_works_with_the_frustum();
    return test::report();
}
