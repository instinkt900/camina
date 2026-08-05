// M0 tests. These use plain checks and no framework. A framework arrives when
// there is something that needs one. See rule 4.6 in DESIGN.md.

#include "check.h"

#include "core/arena.h"
#include "core/frame_stats.h"
#include "core/handle.h"
#include "core/jobs.h"
#include "math/conventions.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

    using test::check;

    void test_arena() {
        engine::Arena arena(1024);

        check(arena.capacity() == 1024, "arena reports its capacity");
        check(arena.used() == 0, "a new arena has used zero bytes");

        auto* a = arena.allocate_n<std::uint32_t>(4);
        check(a != nullptr, "arena returns a block");
        check(arena.used() == 16, "arena advances by the request size");

        // Ask for a type with a larger alignment and confirm the result is aligned.
        auto* b = arena.allocate_n<double>(1);
        check(b != nullptr, "arena returns an aligned block");
        check(reinterpret_cast<std::uintptr_t>(b) % alignof(double) == 0,
              "arena honors the alignment");

        const std::size_t high = arena.high_water();
        arena.reset();
        check(arena.used() == 0, "reset releases every allocation");
        check(arena.high_water() == high, "reset keeps the high water mark");

        check(arena.allocate(4096, 8) == nullptr, "arena returns null when it is full");
    }

    void test_handle() {
        struct TextureTag {};
        using TextureHandle = engine::Handle<TextureTag>;

        TextureHandle none;
        check(!none.valid(), "a default handle is invalid");

        const TextureHandle handle = TextureHandle::make(7, 3);
        check(handle.valid(), "a made handle is valid");
        check(handle.index() == 7, "handle keeps the index");
        check(handle.generation() == 3, "handle keeps the generation");
        check(handle != TextureHandle::make(7, 4), "a stale generation compares unequal");
        check(sizeof(TextureHandle) == 8, "handle stays 8 bytes for the future C ABI");
    }

    void test_conventions() {
        // Reverse-Z sends the near plane to depth 1 and infinity to depth 0.
        const engine::Mat4 projection =
            engine::perspective_reverse_z(glm::radians(60.0F), 16.0F / 9.0F, 0.1F);

        const engine::Vec4 near_point = projection * engine::Vec4{ 0.0F, 0.0F, -0.1F, 1.0F };
        const float near_depth = near_point.z / near_point.w;
        check(std::abs(near_depth - 1.0F) < 1e-4F, "the near plane maps to depth 1");

        const engine::Vec4 far_point = projection * engine::Vec4{ 0.0F, 0.0F, -1.0e6F, 1.0F };
        const float far_depth = far_point.z / far_point.w;
        check(std::abs(far_depth) < 1e-4F, "a distant point maps to depth near 0");

        check(engine::world_forward == engine::Vec3(0.0F, 0.0F, -1.0F), "forward is -Z");
        check(engine::world_up == engine::Vec3(0.0F, 1.0F, 0.0F), "up is +Y");
        // Right-handed means X cross Y equals Z, where Z points backward. Stated with
        // the named vectors, forward cross up gives right.
        check(glm::cross(engine::world_forward, engine::world_up) == engine::world_right,
              "forward cross up gives right");
        check(glm::cross(engine::world_right, engine::world_up) == -engine::world_forward,
              "right cross up gives backward, so the basis is right-handed");
    }

    void test_jobs() {
        engine::jobs::init();
        check(engine::jobs::worker_count() > 0, "the job system starts workers");

        constexpr std::uint32_t count = 100000;
        std::vector<std::uint32_t> data(count, 0);

        engine::jobs::parallel_for(
            count, 256,
            [&data](std::uint32_t begin, std::uint32_t end, std::uint32_t /*thread_index*/) {
                for (std::uint32_t i = begin; i < end; ++i) {
                    data[i] = i;
                }
            });

        bool all_written = true;
        for (std::uint32_t i = 0; i < count; ++i) {
            if (data[i] != i) {
                all_written = false;
                break;
            }
        }
        check(all_written, "parallel_for covers every item exactly once");

        // An empty range must not deadlock or crash.
        engine::jobs::parallel_for(0, 1, [](std::uint32_t, std::uint32_t, std::uint32_t) {});
        check(true, "parallel_for accepts an empty range");

        engine::jobs::shutdown();
        check(engine::jobs::worker_count() == 0, "shutdown stops the workers");
    }

    /**
     * Pins the orientation that the rasterizer sees.
     *
     * A face wound counter-clockwise seen from outside, and facing the camera,
     * must reach the framebuffer with a negative signed area. That is the value
     * VK_FRONT_FACE_COUNTER_CLOCKWISE accepts as front facing.
     *
     * Vulkan clip space puts +Y down, and perspective_reverse_z negates the Y
     * row. The two cancel, so no winding change is needed anywhere. Remove the
     * negation and this test fails, which is the signal to revisit frontFace in
     * gfx/vulkan/vk_pipeline.cpp. An inside-out cube is otherwise easy to miss.
     */
    void test_winding() {
        const engine::Mat4 projection =
            engine::perspective_reverse_z(glm::radians(60.0F), 16.0F / 9.0F, 0.1F);

        // The +Z face of a unit cube, wound counter-clockwise from outside, seen
        // by a camera three meters along +Z.
        const std::array<engine::Vec3, 3> face{ engine::Vec3{ -0.5F, -0.5F, 0.5F },
                                                engine::Vec3{ 0.5F, -0.5F, 0.5F },
                                                engine::Vec3{ 0.5F, 0.5F, 0.5F } };

        std::array<engine::Vec2, 3> ndc{};
        for (std::size_t i = 0; i < face.size(); ++i) {
            const engine::Vec3 eye{ face[i].x, face[i].y, face[i].z - 3.0F };
            const engine::Vec4 clip = projection * engine::Vec4{ eye, 1.0F };
            ndc[i] = engine::Vec2{ clip.x / clip.w, clip.y / clip.w };
        }

        float area = 0.0F;
        for (std::size_t i = 0; i < ndc.size(); ++i) {
            const engine::Vec2& a = ndc[i];
            const engine::Vec2& b = ndc[(i + 1) % ndc.size()];
            area += (a.x * b.y) - (b.x * a.y);
        }
        area *= 0.5F;

        check(area < 0.0F, "a camera-facing outward face is front facing under CCW");
    }

    void test_frame_stats() {
        engine::FrameStats empty(0);
        check(empty.counted() == 0, "a new collector holds no samples");
        check(empty.summarize().count == 0, "an empty run summarizes to zeros");

        // The warm-up drops leading samples rather than weighting them. A run
        // whose first frames build pipelines would otherwise report the startup.
        engine::FrameStats warm(3);
        for (const double sample : { 100.0, 100.0, 100.0, 10.0, 20.0 }) {
            warm.add(sample);
        }
        check(warm.dropped() == 3, "the warm-up drops the count it was given");
        check(warm.counted() == 2, "the samples after the warm-up are kept");
        check(warm.summarize().high_ms == 20.0, "a dropped sample stays out of the summary");

        // Fewer samples than the warm-up leaves nothing, and that must report
        // zeros rather than read off the end of an empty array.
        engine::FrameStats short_run(10);
        short_run.add(1.0);
        check(short_run.counted() == 0, "a run shorter than the warm-up keeps nothing");
        check(short_run.summarize().count == 0, "a run shorter than the warm-up summarizes to zeros");

        // Ten samples, added out of order, so the summary has to sort them.
        engine::FrameStats run(0);
        for (const double sample : { 7.0, 3.0, 10.0, 1.0, 5.0, 9.0, 2.0, 8.0, 4.0, 6.0 }) {
            run.add(sample);
        }
        const engine::FrameSummary summary = run.summarize();
        check(summary.count == 10, "the summary counts every sample");
        check(summary.low_ms == 1.0, "the low is the fastest frame");
        check(summary.high_ms == 10.0, "the high is the slowest frame");
        check(std::abs(summary.mean_ms - 5.5) < 1e-9, "the mean averages every sample");
        // Nearest rank, so the median of an even count is the lower middle
        // sample. Averaging the two would report 5.5, which no frame took.
        check(summary.median_ms == 5.0, "the median is a sample rather than an average");
        check(summary.p95_ms == 10.0, "p95 of ten samples is the slowest");
        check(summary.p99_ms == 10.0, "p99 of ten samples is the slowest");

        // One sample must be reachable at every percentile. Rounding the rank
        // down instead would ask for index zero minus one.
        engine::FrameStats single(0);
        single.add(4.0);
        const engine::FrameSummary one = single.summarize();
        check(one.median_ms == 4.0 && one.p99_ms == 4.0 && one.high_ms == 4.0,
              "every percentile of one sample is that sample");

        // A hundred samples put a percentile away from the ends, where an
        // off-by-one in the rank shows up as a neighbouring value.
        engine::FrameStats hundred(0);
        for (int i = 1; i <= 100; ++i) {
            hundred.add(static_cast<double>(i));
        }
        const engine::FrameSummary many = hundred.summarize();
        check(many.median_ms == 50.0, "the median of a hundred samples is the fiftieth");
        check(many.p95_ms == 95.0, "p95 of a hundred samples is the ninety-fifth");
        check(many.p99_ms == 99.0, "p99 of a hundred samples is the ninety-ninth");

        // A clock that goes backwards would otherwise drag the mean down, and
        // nothing in the reported numbers would say why.
        engine::FrameStats negative(0);
        negative.add(5.0);
        negative.add(-1.0);
        check(negative.counted() == 1, "a period below zero is dropped");
    }

} // namespace

int main() {
    std::printf("arena\n");
    test_arena();
    std::printf("handle\n");
    test_handle();
    std::printf("conventions\n");
    test_conventions();
    std::printf("winding\n");
    test_winding();
    std::printf("jobs\n");
    test_jobs();
    std::printf("frame stats\n");
    test_frame_stats();

    return test::report();
}
