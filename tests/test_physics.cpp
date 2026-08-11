// M7.1 tests for the Box3D conventions.
//
// DESIGN.md section 3 carried an open item from the day the conventions were
// settled. It asked somebody to confirm that Box3D agrees about which way is
// up. This is that check, written so it runs on every build rather than once.
//
// A physics library that disagrees with the renderer about up produces mirrored
// or inverted motion. The symptom points at the gameplay code rather than at
// the library. That is why this is a test and not a comment.

#include "check.h"
#include "core/jobs.h"
#include "physics/conventions.h"
#include "physics/world.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <vector>

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
        // sign. A library that used +Z up would pass neither.
        check(gravity.y < 0.0F, "default gravity points down -Y");
        check(gravity.x == 0.0F, "default gravity has no X");
        check(gravity.z == 0.0F, "default gravity has no Z");

        check(std::fabs(std::fabs(gravity.y) - kExpectedMagnitude) < kTolerance,
              "default gravity is about 10 meters per second squared");
    }

    constexpr float kBoxHalfSize = 0.5F;
    constexpr float kStep = 1.0F / 60.0F;
    constexpr std::uint32_t kStepCount = 120;

    /// A stack big enough that the solver has real work to split.
    constexpr std::uint32_t kLargeWidth = 8;
    constexpr std::uint32_t kLargeHeight = 12;

    /// A stack the size the sandbox will actually hold. The issue behind this
    /// work asks whether the job system pays for itself at that size, and only a
    /// measurement at that size can answer it.
    constexpr std::uint32_t kSmallWidth = 2;
    constexpr std::uint32_t kSmallHeight = 3;

    /// Builds the same scene every time, so two runs differ only in the worker
    /// count they were given.
    std::vector<engine::physics::BodyId> build_stack(engine::physics::World& world,
                                                     std::uint32_t width, std::uint32_t height) {
        world.add_static_box(Vec3{ 0.0F, -1.0F, 0.0F }, Vec3{ 50.0F, 1.0F, 50.0F });

        std::vector<engine::physics::BodyId> boxes;
        for (std::uint32_t y = 0; y < height; ++y) {
            for (std::uint32_t x = 0; x < width; ++x) {
                for (std::uint32_t z = 0; z < width; ++z) {
                    const Vec3 center{ static_cast<float>(x) * 1.2F,
                                       (static_cast<float>(y) * 1.2F) + kBoxHalfSize,
                                       static_cast<float>(z) * 1.2F };
                    boxes.push_back(world.add_dynamic_box(
                        center, Vec3{ kBoxHalfSize, kBoxHalfSize, kBoxHalfSize }));
                }
            }
        }
        return boxes;
    }

    /// Steps the scene and returns how long the steps took, in milliseconds.
    double run_steps(engine::physics::World& world) {
        const auto start = std::chrono::steady_clock::now();
        for (std::uint32_t i = 0; i < kStepCount; ++i) {
            world.step(kStep);
        }
        const auto end = std::chrono::steady_clock::now();
        return std::chrono::duration<double, std::milli>(end - start).count();
    }

    void a_box_falls_and_lands() {
        section("A body falls, and the floor stops it");

        engine::physics::World world;
        const engine::physics::BodyId box =
            world.add_dynamic_box(Vec3{ 0.0F, 8.0F, 0.0F },
                                  Vec3{ kBoxHalfSize, kBoxHalfSize, kBoxHalfSize });
        world.add_static_box(Vec3{ 0.0F, -1.0F, 0.0F }, Vec3{ 50.0F, 1.0F, 50.0F });

        const float start_height = world.body_position(box).y;
        for (std::uint32_t i = 0; i < kStepCount; ++i) {
            world.step(kStep);
        }
        const Vec3 landed = world.body_position(box);

        // Falling proves the step ran at all. Landing proves the solver ran, and
        // that is the part the task callbacks carry.
        check(landed.y < start_height, "the box fell");
        check(landed.y > 0.0F, "the floor stopped it above the origin");
        check(std::fabs(landed.x) < 0.1F && std::fabs(landed.z) < 0.1F,
              "it fell straight down");
    }

    void the_worker_count_matches_the_job_system() {
        section("Box3D is told what the job system actually has");

        const engine::physics::World shared;
        check(shared.worker_count() == engine::jobs::worker_count(),
              "a default world takes the job system worker count");

        const engine::physics::World single(1);
        check(single.worker_count() == 1, "a world can be asked for one worker");
    }

    void the_solver_runs_on_the_job_system() {
        section("The solver runs on the job system and not on threads of its own");

        // Box3D falls back to its own scheduler and its own threads whenever a
        // world definition leaves either callback null. A step that ran that way
        // would land the boxes in the same place and take about the same time, so
        // the count is the only thing that tells the two apart.
        const std::uint64_t before = engine::physics::tasks_enqueued();

        engine::physics::World world;
        build_stack(world, kSmallWidth, kSmallHeight);
        world.step(kStep);

        check(engine::physics::tasks_enqueued() > before,
              "one step handed work to the job system");
    }

    /// Reports the cost of a step on the pool against the cost on one thread.
    ///
    /// This asserts nothing about the time. A test machine is shared, a CI runner
    /// is shared harder, and a threshold on wall time fails for reasons that have
    /// nothing to do with this code. The number is here to be read, and the pull
    /// request records what it said on the reference machine.
    ///
    /// It does assert that the two runs agree about where the stack ended up. A
    /// scheduler that drops a task, or returns from finish before the task has
    /// finished, changes the answer rather than the timing.
    void report_step_cost(const char* label, std::uint32_t width, std::uint32_t height) {
        engine::physics::World threaded;
        const std::vector<engine::physics::BodyId> threaded_boxes =
            build_stack(threaded, width, height);
        const double threaded_ms = run_steps(threaded);

        engine::physics::World single(1);
        const std::vector<engine::physics::BodyId> single_boxes =
            build_stack(single, width, height);
        const double single_ms = run_steps(single);

        std::printf("  ----  %s: %u bodies, %.3f ms per step on %u workers, %.3f ms on 1\n", label,
                    threaded.body_count(), threaded_ms / kStepCount, threaded.worker_count(),
                    single_ms / kStepCount);
        std::fflush(stdout);

        std::uint32_t moved = 0;
        for (std::size_t i = 0; i < threaded_boxes.size(); ++i) {
            const Vec3 a = threaded.body_position(threaded_boxes[i]);
            const Vec3 b = single.body_position(single_boxes[i]);
            if (std::fabs(a.y - b.y) > 0.05F) {
                ++moved;
            }
        }

        check(threaded_boxes.size() == single_boxes.size(), "both runs built the same scene");
        check(moved == 0, "the threaded run and the single-threaded run agree");
    }

} // namespace

int main() {
    // The physics world needs the scheduler, because Box3D runs its solver on it.
    engine::jobs::init();

    gravity_points_down_y();
    a_box_falls_and_lands();
    the_worker_count_matches_the_job_system();
    the_solver_runs_on_the_job_system();

    // Two sizes, either side of the crossover DESIGN.md section 5.1 records. The
    // small one is what the sandbox will hold, and the pool loses there. The
    // large one is where it wins. Both are printed, because a single number
    // would hide which side of the crossover the reader is on.
    test::section("What the job system is worth, by scene size");
    report_step_cost("sandbox size", kSmallWidth, kSmallHeight);
    report_step_cost("large stack ", kLargeWidth, kLargeHeight);

    engine::jobs::shutdown();
    return test::report();
}
