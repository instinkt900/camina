// M8.1 tests for the Lua host.
//
// Every script here is a string literal rather than a file. The host reads no
// asset on purpose: the caller resolves a GUID through assets::Content and
// hands over bytes, so a test needs no cooked tree and no content root.
//
// What these cannot cover is the cooked path, which is test_cooker.cpp, and the
// fixed step itself, which is test_timestep.cpp. This file checks the lifecycle,
// the per-entity state, and what an error does.

#include "check.h"
#include "core/guid.h"
#include "scene/world.h"
#include "script/components.h"
#include "script/host.h"

#include <cstddef>
#include <span>
#include <string_view>

namespace {

    using test::check;
    using test::section;
    namespace sc = engine::scene;
    namespace sp = engine::script;

    /// Two identities that are not the null GUID and are not each other. The
    /// values mean nothing. The host only ever compares them.
    constexpr engine::Guid kFirst{ 1, 1 };
    constexpr engine::Guid kSecond{ 2, 2 };

    [[nodiscard]] std::span<const std::byte> bytes_of(std::string_view text) {
        return { reinterpret_cast<const std::byte*>(text.data()), text.size() };
    }

    /// Loads @p text under @p guid, and reports whether it compiled.
    [[nodiscard]] bool load(sp::Host& host, engine::Guid guid, std::string_view text) {
        return host.load(guid, "test.lua", bytes_of(text));
    }

    /// An entity carrying a script, at the world origin.
    [[nodiscard]] entt::entity with_script(sc::World& world, engine::Guid guid) {
        const entt::entity entity = world.create();
        world.registry().emplace<sp::ScriptComponent>(entity, sp::ScriptComponent{ guid });
        return entity;
    }

    void test_the_lifecycle_runs_in_order() {
        section("A script gets on_start once, on_update each step, on_destroy once");

        sp::Host host;
        check(load(host, kFirst, R"(
            starts = 0
            updates = 0
            function on_start() starts = starts + 1 end
            function on_update(seconds) updates = updates + 1 end
            function on_destroy() end
        )"),
              "the script compiles");
        check(host.loaded(kFirst), "and the host holds it");

        sc::World world;
        const entt::entity entity = with_script(world, kFirst);

        host.update(world, 1.0 / 60.0);
        check(host.instance_count() == 1, "the first step gives the entity an instance");
        check(host.call_count(sp::Callback::Start) == 1, "and on_start ran once");
        check(host.call_count(sp::Callback::Update) == 1, "and on_update ran once");

        host.update(world, 2.0 / 60.0);
        host.update(world, 3.0 / 60.0);
        check(host.call_count(sp::Callback::Start) == 1, "on_start does not run again");
        check(host.call_count(sp::Callback::Update) == 3, "and on_update ran once for each step");

        // Removing the component ends the instance. The entity is still there,
        // so on_destroy has something to run against.
        world.registry().erase<sp::ScriptComponent>(entity);
        host.update(world, 4.0 / 60.0);
        check(host.instance_count() == 0, "losing the component drops the instance");
        check(host.call_count(sp::Callback::Destroy) == 1, "and on_destroy ran once");
        check(host.call_count(sp::Callback::Update) == 3, "and it did not update after that");
    }

    void test_the_step_count_is_what_drives_it() {
        section("on_update runs once for each call, whatever the seconds are");

        sp::Host host;
        check(load(host, kFirst, "n = 0\nfunction on_update(s) n = n + 1 end\n"),
              "the counter script compiles");

        sc::World world;
        (void)with_script(world, kFirst);

        // The host counts calls, not time. A fixed step is what decides how
        // many calls there are, and DESIGN.md section 9 is why that matters.
        constexpr std::size_t kSteps = 120;
        for (std::size_t i = 0; i < kSteps; ++i) {
            host.update(world, static_cast<double>(i + 1) / 60.0);
        }
        check(host.call_count(sp::Callback::Update) == kSteps,
              "120 steps give exactly 120 updates");
    }

    void test_seconds_reach_the_script() {
        section("The seconds the host is given are the seconds the script sees");

        sp::Host host;
        check(load(host, kFirst, "last = -1\nfunction on_update(s) last = s end\n"),
              "the script compiles");

        sc::World world;
        (void)with_script(world, kFirst);

        host.update(world, 0.25);
        check(host.call_count(sp::Callback::Update) == 1, "it updated");
        check(host.stopped_count() == 0, "and nothing failed reading the argument");
    }

    void test_two_entities_keep_separate_state() {
        section("One script on two entities gives each its own table");

        sp::Host host;
        // The counter is deliberately not initialized at the top of the chunk,
        // because that would run once for each instance and hide the very thing
        // this checks.
        //
        // Two entities over two steps. With a table each, both counters reach
        // 2. With one shared table the single counter reaches 4, and the third
        // increment raises the error. So the check below tells the two apart
        // rather than passing either way.
        check(load(host, kFirst, R"(
            function on_update(s)
              mine = (mine or 0) + 1
              if mine > 2 then error('state leaked between entities') end
            end
        )"),
              "the script compiles");

        sc::World world;
        (void)with_script(world, kFirst);
        (void)with_script(world, kFirst);

        host.update(world, 1.0 / 60.0);
        check(host.instance_count() == 2, "both entities got an instance");
        check(host.call_count(sp::Callback::Update) == 2, "and both updated");

        host.update(world, 2.0 / 60.0);
        check(host.call_count(sp::Callback::Update) == 4, "and both updated again");
        check(host.stopped_count() == 0, "and neither counter saw the other one");
    }

    void test_an_error_stops_that_instance_alone() {
        section("An error stops one instance, reports once, and leaves the rest running");

        sp::Host host;
        check(load(host, kFirst, "function on_update(s) error('boom') end\n"),
              "the throwing script compiles");
        check(load(host, kSecond, "function on_update(s) end\n"), "and so does the quiet one");

        sc::World world;
        (void)with_script(world, kFirst);
        (void)with_script(world, kSecond);

        host.update(world, 1.0 / 60.0);
        check(host.stopped_count() == 1, "the throwing instance stopped");
        check(host.instance_count() == 2, "and both instances are still held");

        // This is the whole reason for stopping. A script that threw on every
        // step would fill the log and cost the frame each time.
        const std::size_t after_first = host.call_count(sp::Callback::Update);
        host.update(world, 2.0 / 60.0);
        host.update(world, 3.0 / 60.0);
        check(host.call_count(sp::Callback::Update) == after_first + 2,
              "only the quiet one kept updating");
        check(host.stopped_count() == 1, "and nothing else stopped");
    }

    void test_a_script_that_will_not_compile_is_refused() {
        section("A syntax error is reported at load, not at the first step");

        sp::Host host;
        check(!load(host, kFirst, "function on_update( end\n"), "load refuses a syntax error");
        check(!host.loaded(kFirst), "and the host holds nothing for it");

        sc::World world;
        (void)with_script(world, kFirst);

        // The entity names a script that never loaded. It gets no instance and
        // one message, rather than a message on every step.
        host.update(world, 1.0 / 60.0);
        host.update(world, 2.0 / 60.0);
        check(host.call_count(sp::Callback::Update) == 0, "nothing ran");
        check(host.stopped_count() == 1, "and the entity is counted as stopped once");
    }

    void test_a_dead_entity_drops_its_instance() {
        section("An entity the world no longer holds loses its instance");

        sp::Host host;
        check(load(host, kFirst, "function on_update(s) end\n"), "the script compiles");

        sc::World world;
        const entt::entity entity = with_script(world, kFirst);
        host.update(world, 1.0 / 60.0);
        check(host.instance_count() == 1, "the entity has an instance");

        // A reload recycles entity numbers, so an instance that kept its number
        // would attach to whatever took it. This is the same hazard
        // scene::StepMotion had to answer.
        world.destroy(entity);
        host.update(world, 2.0 / 60.0);
        check(host.instance_count() == 0, "destroying the entity drops it");
        check(host.call_count(sp::Callback::Update) == 1, "and it did not update again");
    }

    void test_stop_runs_destroy_on_everything() {
        section("stop() tears every instance down");

        sp::Host host;
        check(load(host, kFirst, "function on_destroy() end\n"), "the script compiles");

        sc::World world;
        (void)with_script(world, kFirst);
        (void)with_script(world, kFirst);
        host.update(world, 1.0 / 60.0);
        check(host.instance_count() == 2, "two instances are running");

        host.stop(world);
        check(host.instance_count() == 0, "stop() drops them");
        check(host.call_count(sp::Callback::Destroy) == 2, "and each one saw on_destroy");
    }

    void test_a_script_needs_no_callbacks() {
        section("A script that declares nothing is not an error");

        sp::Host host;
        check(load(host, kFirst, "x = 1\n"), "a script with no callbacks compiles");

        sc::World world;
        (void)with_script(world, kFirst);

        host.update(world, 1.0 / 60.0);
        check(host.instance_count() == 1, "it still gets an instance");
        check(host.call_count(sp::Callback::Update) == 0, "and nothing is called");
        check(host.stopped_count() == 0, "and that is not a failure");
    }

} // namespace

int main() {
    test_the_lifecycle_runs_in_order();
    test_the_step_count_is_what_drives_it();
    test_seconds_reach_the_script();
    test_two_entities_keep_separate_state();
    test_an_error_stops_that_instance_alone();
    test_a_script_that_will_not_compile_is_refused();
    test_a_dead_entity_drops_its_instance();
    test_stop_runs_destroy_on_everything();
    test_a_script_needs_no_callbacks();

    if (test::g_failures != 0) {
        std::printf("\n%d check(s) failed.\n", test::g_failures);
        return 1;
    }
    std::printf("\nAll checks passed.\n");
    return 0;
}
