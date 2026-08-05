// M5.3a tests for the barrier derivation.
//
// Every one of these runs with no device. That is the reason a pass declares
// what it touches as data rather than by calling into a builder: a builder
// needs a graph, a graph needs a device, and then the one part of a renderer
// that most needs a test would have none. See DESIGN.md section 9 and issue #62.

#include "check.h"
#include "render/render_graph.h"

#include <array>
#include <cstddef>
#include <cstdlib>
#include <span>
#include <vector>

namespace {

    using test::check;
    using engine::gfx::ResourceState;
    using engine::render::derive_barriers;
    using engine::render::GraphSchedule;
    using engine::render::PassDesc;
    using engine::render::ResourceId;
    using engine::render::ResourceRead;
    using engine::render::ResourceWrite;

    constexpr ResourceId kColor{ 0 };
    constexpr ResourceId kDepth{ 1 };

    /// How many barriers one pass ended up with.
    [[nodiscard]] std::size_t count(const GraphSchedule& schedule, std::size_t pass) {
        return schedule.passes[pass].before.size();
    }

    /**
     * A read that follows a write needs a barrier.
     *
     * This is the case the whole file exists for. The first pass writes the
     * target and the second samples it, and nothing may start sampling before
     * the write has finished.
     */
    void test_a_read_after_a_write_gets_a_barrier() {
        const std::array writes{ ResourceWrite{ kColor, ResourceState::ColorTarget } };
        const std::array reads{ ResourceRead{ kColor, ResourceState::ShaderRead } };
        const std::array passes{
            PassDesc{ .name = "offscreen", .reads = {}, .writes = writes },
            PassDesc{ .name = "tonemap", .reads = reads, .writes = {} },
        };
        const std::array initial{ ResourceState::Undefined, ResourceState::Undefined };

        GraphSchedule schedule;
        check(derive_barriers(passes, initial, schedule), "the declarations are valid");
        check(schedule.passes.size() == 2, "there is one entry for each pass");

        check(count(schedule, 1) == 1, "the reader waits for the writer");
        const auto& barrier = schedule.passes[1].before[0];
        check(barrier.resource == kColor, "and the barrier names the resource that moved");
        check(barrier.before == ResourceState::ColorTarget, "from what the writer left it in");
        check(barrier.after == ResourceState::ShaderRead, "to what the reader needs");
    }

    /**
     * A read that follows a read in the same state needs nothing.
     *
     * The one case that must produce no barrier. It is also the common one, so
     * a barrier here would cost real time on every frame for nothing.
     */
    void test_a_read_after_a_read_gets_none() {
        const std::array reads{ ResourceRead{ kColor, ResourceState::ShaderRead } };
        const std::array passes{
            PassDesc{ .name = "first", .reads = reads, .writes = {} },
            PassDesc{ .name = "second", .reads = reads, .writes = {} },
            PassDesc{ .name = "third", .reads = reads, .writes = {} },
        };
        // Already in the state all three want, so even the first needs nothing.
        const std::array initial{ ResourceState::ShaderRead, ResourceState::Undefined };

        GraphSchedule schedule;
        check(derive_barriers(passes, initial, schedule), "the declarations are valid");
        check(count(schedule, 0) == 0, "the first reader needs no barrier");
        check(count(schedule, 1) == 0, "and neither does the second");
        check(count(schedule, 2) == 0, "and neither does the third");
    }

    /**
     * A read that follows a read in another state still gets a barrier.
     *
     * Two reads need no ordering against each other, and that is not the same
     * as needing nothing: a resource read in one state and then in another has
     * to move between them. A depth buffer tested by one pass and sampled by a
     * later one is the case.
     *
     * Without this the "read after a read" test above says the whole story is
     * "two reads need nothing", and a derivation that compared no states at all
     * would pass every other test in this file.
     */
    void test_a_read_in_another_state_gets_a_barrier() {
        const std::array tested{ ResourceRead{ kDepth, ResourceState::DepthRead } };
        const std::array sampled{ ResourceRead{ kDepth, ResourceState::ShaderRead } };
        const std::array passes{
            PassDesc{ .name = "tests depth", .reads = tested, .writes = {} },
            PassDesc{ .name = "samples depth", .reads = sampled, .writes = {} },
        };
        const std::array initial{ ResourceState::Undefined, ResourceState::DepthRead };

        GraphSchedule schedule;
        check(derive_barriers(passes, initial, schedule), "the declarations are valid");
        check(count(schedule, 0) == 0, "the first reader wants the state it is already in");
        check(count(schedule, 1) == 1, "the second reader needs the state changed");
        const auto& barrier = schedule.passes[1].before[0];
        check(barrier.before == ResourceState::DepthRead && barrier.after == ResourceState::ShaderRead,
              "and the barrier carries both states");
    }

    /**
     * Two writes to one resource are ordered, even in the same state.
     *
     * Nothing changes state here, so a check that only compared states would
     * let these two overlap. The barrier carries the ordering and nothing else,
     * which is why its two states are allowed to match.
     */
    void test_two_writes_are_ordered() {
        const std::array writes{ ResourceWrite{ kColor, ResourceState::ColorTarget } };
        const std::array passes{
            PassDesc{ .name = "opaque", .reads = {}, .writes = writes },
            PassDesc{ .name = "blended", .reads = {}, .writes = writes },
        };
        const std::array initial{ ResourceState::ColorTarget, ResourceState::Undefined };

        GraphSchedule schedule;
        check(derive_barriers(passes, initial, schedule), "the declarations are valid");
        check(count(schedule, 1) == 1, "the second writer is ordered against the first");
        const auto& barrier = schedule.passes[1].before[0];
        check(barrier.before == barrier.after,
              "and it changes no state, because it carries only the ordering");
    }

    /**
     * A write that follows a read is ordered as well.
     *
     * The hazard runs the other way here: the write must not start before the
     * read it follows has finished.
     */
    void test_a_write_after_a_read_is_ordered() {
        const std::array reads{ ResourceRead{ kDepth, ResourceState::DepthRead } };
        const std::array writes{ ResourceWrite{ kDepth, ResourceState::DepthRead } };
        const std::array passes{
            PassDesc{ .name = "reader", .reads = reads, .writes = {} },
            PassDesc{ .name = "writer", .reads = {}, .writes = writes },
        };
        const std::array initial{ ResourceState::Undefined, ResourceState::DepthRead };

        GraphSchedule schedule;
        check(derive_barriers(passes, initial, schedule), "the declarations are valid");
        check(count(schedule, 0) == 0, "the reader was already in the state it wanted");
        check(count(schedule, 1) == 1, "the writer is ordered against the reader");
    }

    /**
     * The first access moves a resource out of the state it started in.
     */
    void test_the_first_access_leaves_the_initial_state() {
        const std::array writes{ ResourceWrite{ kColor, ResourceState::ColorTarget } };
        const std::array passes{ PassDesc{ .name = "mesh", .reads = {}, .writes = writes } };
        const std::array initial{ ResourceState::Undefined, ResourceState::Undefined };

        GraphSchedule schedule;
        check(derive_barriers(passes, initial, schedule), "the declarations are valid");
        check(count(schedule, 0) == 1, "the first write needs a barrier");
        check(schedule.passes[0].before[0].before == ResourceState::Undefined,
              "and it starts from the state the caller declared");
        check(schedule.final_states[0] == ResourceState::ColorTarget,
              "the frame ends with the resource where the last pass left it");
    }

    /**
     * A pass may read and write one attachment, and it gets one barrier.
     *
     * Depth is the case: the test reads the buffer and the write updates it.
     * Declaring both is honest, and two barriers for one transition would be
     * wrong.
     */
    void test_one_resource_read_and_written_by_one_pass() {
        const std::array reads{ ResourceRead{ kDepth, ResourceState::DepthTarget } };
        const std::array writes{ ResourceWrite{ kDepth, ResourceState::DepthTarget } };
        const std::array passes{ PassDesc{ .name = "mesh", .reads = reads, .writes = writes } };
        const std::array initial{ ResourceState::Undefined, ResourceState::Undefined };

        GraphSchedule schedule;
        check(derive_barriers(passes, initial, schedule), "the declarations are valid");
        check(count(schedule, 0) == 1, "one barrier covers both");

        // The pair folds into a write, so the next pass is ordered against it.
        const std::array again{ PassDesc{ .name = "mesh", .reads = reads, .writes = writes },
                                PassDesc{ .name = "later", .reads = reads, .writes = {} } };
        GraphSchedule second;
        check(derive_barriers(again, initial, second), "the declarations are valid");
        check(count(second, 1) == 1, "and the reader after it still waits for the write");
    }

    /**
     * A pass that wants one resource in two states at once is refused.
     *
     * There is no answer to the question, and picking one would hide the
     * mistake behind a picture that is only wrong sometimes.
     */
    void test_two_states_at_once_are_refused() {
        const std::array reads{ ResourceRead{ kDepth, ResourceState::DepthRead } };
        const std::array writes{ ResourceWrite{ kDepth, ResourceState::DepthTarget } };
        const std::array passes{ PassDesc{ .name = "confused", .reads = reads, .writes = writes } };
        const std::array initial{ ResourceState::Undefined, ResourceState::Undefined };

        GraphSchedule schedule;
        check(!derive_barriers(passes, initial, schedule), "the pass is refused");
        check(schedule.passes.empty(), "and nothing is left behind to act on");
    }

    /// A resource nobody declared is refused rather than ignored.
    void test_an_unknown_resource_is_refused() {
        const std::array writes{ ResourceWrite{ ResourceId{ 7 }, ResourceState::ColorTarget } };
        const std::array passes{ PassDesc{ .name = "stray", .reads = {}, .writes = writes } };
        const std::array initial{ ResourceState::Undefined, ResourceState::Undefined };

        GraphSchedule schedule;
        check(!derive_barriers(passes, initial, schedule), "the pass is refused");
    }

    /**
     * A lifetime spans the first and the last pass that touch a resource.
     *
     * The aliasing in issue #122 reads this, and it is derived here because the
     * walk already knows it. A resource nothing names is reported as unused
     * rather than as live over pass zero.
     */
    void test_lifetimes_span_first_to_last_use() {
        const std::array color{ ResourceWrite{ kColor, ResourceState::ColorTarget } };
        const std::array depth{ ResourceWrite{ kDepth, ResourceState::DepthTarget } };
        const std::array passes{
            PassDesc{ .name = "shadow", .reads = {}, .writes = depth },
            PassDesc{ .name = "mesh", .reads = {}, .writes = color },
            PassDesc{ .name = "tonemap", .reads = {}, .writes = color },
        };
        const std::array initial{ ResourceState::Undefined, ResourceState::Undefined,
                                  ResourceState::Undefined };

        GraphSchedule schedule;
        check(derive_barriers(passes, initial, schedule), "the declarations are valid");
        check(schedule.lifetimes[0].used, "the color target is used");
        check(schedule.lifetimes[0].first_pass == 1 && schedule.lifetimes[0].last_pass == 2,
              "and it is live from the mesh pass to the tonemap");
        check(schedule.lifetimes[1].used, "the depth target is used");
        check(schedule.lifetimes[1].first_pass == 0 && schedule.lifetimes[1].last_pass == 0,
              "and it is live over the shadow pass alone");
        check(!schedule.lifetimes[2].used, "the third resource is reported as never named");
    }

    /// No passes at all is a valid frame that needs nothing.
    void test_an_empty_frame_is_valid() {
        const std::array initial{ ResourceState::Present };
        GraphSchedule schedule;
        check(derive_barriers({}, initial, schedule), "an empty frame is valid");
        check(schedule.passes.empty(), "and it holds no passes");
        check(schedule.final_states[0] == ResourceState::Present,
              "and it leaves every resource where it found it");
    }

} // namespace

int main() {
    test_a_read_after_a_write_gets_a_barrier();
    test_a_read_after_a_read_gets_none();
    test_a_read_in_another_state_gets_a_barrier();
    test_two_writes_are_ordered();
    test_a_write_after_a_read_is_ordered();
    test_the_first_access_leaves_the_initial_state();
    test_one_resource_read_and_written_by_one_pass();
    test_two_states_at_once_are_refused();
    test_an_unknown_resource_is_refused();
    test_lifetimes_span_first_to_last_use();
    test_an_empty_frame_is_valid();
    return test::report();
}
