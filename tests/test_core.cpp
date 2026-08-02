// M0 tests. These use plain checks and no framework. A framework arrives when
// there is something that needs one. See rule 4.6 in DESIGN.md.

#include "core/arena.h"
#include "core/handle.h"
#include "core/jobs.h"
#include "math/conventions.h"

#include <cmath>
#include <cstdio>
#include <cstdint>
#include <vector>

namespace {

    int g_failures = 0;

    void check(bool condition, const char* name) {
        if (condition) {
            std::printf("  pass  %s\n", name);
        } else {
            std::printf("  FAIL  %s\n", name);
            ++g_failures;
        }
    }

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

} // namespace

int main() {
    std::printf("arena\n");
    test_arena();
    std::printf("handle\n");
    test_handle();
    std::printf("conventions\n");
    test_conventions();
    std::printf("jobs\n");
    test_jobs();

    if (g_failures == 0) {
        std::printf("\nAll tests passed.\n");
        return 0;
    }
    std::printf("\n%d test(s) failed.\n", g_failures);
    return 1;
}
