// This program is expected to die. tests/CMakeLists.txt passes it only when the
// engine assertion reported the failure first.
//
// EnTT guards its own preconditions with ENTT_ASSERT. Nothing defined that
// macro, so EnTT used assert(), and NDEBUG removed assert() from a
// RelWithDebInfo build. src/core/entt.h points ENTT_ASSERT at ENGINE_ASSERT.
//
// Without that wiring this program still dies. It dies reading off the end of
// an empty pool, with no message and no line number, and that is the failure
// this test exists to keep away.

#include "scene/world.h"

#include <cstdio>

namespace {

    /// A component nothing ever adds to an entity.
    struct NotThere {
        int value = 0;
    };

} // namespace

int main() {
    engine::scene::World world;
    const entt::entity entity = world.create();

    std::printf("Reading a component the entity does not carry.\n");
    std::fflush(stdout);

    const NotThere& missing = world.registry().get<NotThere>(entity);

    // Unreachable. Reading the value keeps the compiler from removing the get.
    std::printf("The process is still running, and the value is %d.\n", missing.value);
    return 0;
}
