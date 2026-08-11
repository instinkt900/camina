// M7.1 tests for the Box3D conventions.
//
// DESIGN.md section 3 carried an open item from the day the conventions were
// settled: confirm that Box3D agrees about which way is up before trusting it.
// This is that check, written so it runs again on every build rather than once
// in a chat.
//
// A physics library that disagrees with the renderer about up produces
// mirrored or inverted motion, and the symptom points at the gameplay code
// rather than at the library. That is why this is a test and not a comment.

#include "check.h"
#include "physics/conventions.h"

#include <cmath>

namespace {

    using engine::Vec3;
    using engine::physics::default_gravity;
    using test::check;
    using test::section;

    /// Box3D uses -10 rather than -9.81. See physics/conventions.h.
    constexpr float kExpectedMagnitude = 10.0F;

    /// Half a percent of a meter per second squared. The value is exact today,
    /// and an update that trims it towards 9.81 is not what this test guards.
    constexpr float kTolerance = 0.05F;

    void gravity_points_down_y() {
        section("Box3D agrees with DESIGN.md section 3");

        const Vec3 gravity = default_gravity();

        // The engine is +Y up, so the default gravity has to be -Y. The two
        // checks either side of it are what tell a wrong axis from a wrong
        // sign: a library that used +Z up would pass neither.
        check(gravity.y < 0.0F, "default gravity points down -Y");
        check(gravity.x == 0.0F, "default gravity has no X");
        check(gravity.z == 0.0F, "default gravity has no Z");

        check(std::fabs(std::fabs(gravity.y) - kExpectedMagnitude) < kTolerance,
              "default gravity is about 10 meters per second squared");
    }

} // namespace

int main() {
    gravity_points_down_y();
    return test::report();
}
