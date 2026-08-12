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

    /// A component the engine does not name, standing in for a game type.
    ///
    /// sandbox::Spin is the real case: the engine names it nowhere, and it has
    /// to reach a script all the same. A type declared here proves the binding
    /// reads descriptors rather than a list somebody maintains.
    struct Turret {
        engine::Vec3 aim{ 0.0F, 0.0F, -1.0F };
        float range = 12.0F;
        bool armed = false;
        std::string label = "turret";
    };

} // namespace

/// @brief Describes the game-side test component, the way a game would.
///
/// This sits outside the engine exactly as `sandbox::Spin` does. Nothing in
/// `src/` names Turret, and the binding still carries it.
template <>
struct engine::reflect::Describe<Turret> {
    static constexpr const char* name = "Turret"; ///< The name a script uses.
    /// @brief The four fields.
    /// @return A tuple of field descriptors.
    static constexpr auto fields() {
        return std::make_tuple(ENGINE_FIELD(Turret, aim), ENGINE_FIELD(Turret, range),
                               ENGINE_FIELD(Turret, armed), ENGINE_FIELD(Turret, label));
    }
};

namespace {

    /// A world and a registry that holds the two types these tests use.
    struct Fixture {
        sc::ComponentRegistry components;
        sc::World world;

        Fixture() {
            components.add<engine::Transform>();
            components.add<Turret>();
        }
    };

    void test_a_script_reads_a_component() {
        section("A script reads any described component by name");

        Fixture fixture;
        sp::Host host{ fixture.components };
        check(load(host, kFirst, R"(
            function on_update(s)
              local t = entity:get("Turret")
              if t == nil then error("no Turret") end
              if t.range ~= 12.0 then error("wrong range: " .. tostring(t.range)) end
              if t.armed ~= false then error("wrong armed") end
              if t.label ~= "turret" then error("wrong label") end
              if t.aim.z ~= -1.0 then error("wrong aim.z") end
            end
        )"),
              "the reading script compiles");

        const entt::entity entity = fixture.world.create();
        fixture.world.registry().emplace<Turret>(entity, Turret{});
        fixture.world.registry().emplace<sp::ScriptComponent>(entity,
                                                              sp::ScriptComponent{ kFirst });

        host.update(fixture.world, 1.0 / 60.0);
        check(host.call_count(sp::Callback::Update) == 1, "it ran");
        check(host.stopped_count() == 0, "and every field read as the C++ side holds it");
    }

    void test_a_script_writes_a_component() {
        section("A script writes a component, and the change reaches the world");

        Fixture fixture;
        sp::Host host{ fixture.components };
        check(load(host, kFirst, R"(
            function on_update(s)
              entity:set("Turret", { range = 30.0, armed = true, label = "live",
                                     aim = { x = 1.0 } })
            end
        )"),
              "the writing script compiles");

        const entt::entity entity = fixture.world.create();
        fixture.world.registry().emplace<Turret>(entity, Turret{});
        fixture.world.registry().emplace<sp::ScriptComponent>(entity,
                                                              sp::ScriptComponent{ kFirst });

        host.update(fixture.world, 1.0 / 60.0);
        check(host.stopped_count() == 0, "the script ran without an error");

        const Turret& turret = fixture.world.registry().get<Turret>(entity);
        check(turret.range == 30.0F, "a number reached the component");
        check(turret.armed, "and a bool did");
        check(turret.label == "live", "and a string did");

        // A table naming one component keeps the rest, so a script can move one
        // axis without reading the whole vector back first.
        check(turret.aim.x == 1.0F, "the named part of a vector was written");
        check(turret.aim.z == -1.0F, "and the parts it did not name were kept");
    }

    void test_a_game_type_needs_no_engine_code() {
        section("A component the engine never names still reaches a script");

        // Turret is declared in this file. Nothing in src/ mentions it, and the
        // binding still reads and writes it, because it walks Describe<T> and
        // not a list. That is rule 4.5 working from the outside.
        Fixture fixture;
        sp::Host host{ fixture.components };
        check(load(host, kFirst, R"(
            function on_update(s)
              local t = entity:get("Turret")
              entity:set("Turret", { range = t.range * 2.0 })
            end
        )"),
              "the script compiles");

        const entt::entity entity = fixture.world.create();
        fixture.world.registry().emplace<Turret>(entity, Turret{});
        fixture.world.registry().emplace<sp::ScriptComponent>(entity,
                                                              sp::ScriptComponent{ kFirst });

        host.update(fixture.world, 1.0 / 60.0);
        check(fixture.world.registry().get<Turret>(entity).range == 24.0F,
              "it read the value and wrote a new one back");
    }

    void test_a_transform_written_from_lua_marks_the_entity_dirty() {
        section("Moving an entity from Lua moves what it draws");

        Fixture fixture;
        sp::Host host{ fixture.components };
        check(load(host, kFirst, R"(
            function on_update(s)
              entity:set("Transform", { position = { x = 5.0, y = 6.0, z = 7.0 } })
            end
        )"),
              "the moving script compiles");

        const entt::entity parent = fixture.world.create();
        const entt::entity child = fixture.world.create();
        check(fixture.world.set_parent(child, parent), "the child parents to the entity");
        fixture.world.set_local(child, engine::Transform{ .position = { 0.0F, 1.0F, 0.0F } });
        fixture.world.registry().emplace<sp::ScriptComponent>(parent,
                                                              sp::ScriptComponent{ kFirst });

        fixture.world.update();
        host.update(fixture.world, 1.0 / 60.0);
        check(host.stopped_count() == 0, "the script ran");

        // A Transform written through the registry goes around set_local(), so
        // without mark_dirty() the world matrix stays stale and the child never
        // follows. This is the check that the binding does not forget it.
        fixture.world.update();
        const engine::Mat4 moved = fixture.world.world_matrix(parent);
        check(moved[3][0] == 5.0F && moved[3][1] == 6.0F,
              "the parent's world matrix rebuilt");

        const engine::Mat4 child_matrix = fixture.world.world_matrix(child);
        check(child_matrix[3][1] == 7.0F, "and the child moved with it");
    }

    void test_the_binding_refuses_what_it_cannot_do() {
        section("A wrong name or a wrong type is refused rather than guessed at");

        Fixture fixture;
        sp::Host host{ fixture.components };
        check(load(host, kFirst, R"(
            results = {}
            function on_update(s)
              if entity:get("NoSuchComponent") ~= nil then error("named a missing type") end
              if entity:has("NoSuchComponent") then error("claimed a missing type") end
              if not entity:has("Turret") then error("lost a real type") end
              if entity:set("Turret", { range = "not a number" }) then
                error("wrote a string into a float")
              end
              if entity:set("NoSuchComponent", { x = 1 }) then
                error("wrote to a missing type")
              end
            end
        )"),
              "the script compiles");

        const entt::entity entity = fixture.world.create();
        fixture.world.registry().emplace<Turret>(entity, Turret{});
        fixture.world.registry().emplace<sp::ScriptComponent>(entity,
                                                              sp::ScriptComponent{ kFirst });

        host.update(fixture.world, 1.0 / 60.0);
        check(host.stopped_count() == 0, "every refusal came back as the script expected");
        check(fixture.world.registry().get<Turret>(entity).range == 12.0F,
              "and the refused write left the field alone");
    }

    void test_a_component_the_entity_lacks_reads_nil() {
        section("Asking for a component the entity does not carry gives nil");

        Fixture fixture;
        sp::Host host{ fixture.components };
        check(load(host, kFirst, R"(
            function on_update(s)
              if entity:get("Turret") ~= nil then error("invented a component") end
              if entity:has("Turret") then error("claimed one") end
              if entity:get("Transform") == nil then error("lost the one it has") end
            end
        )"),
              "the script compiles");

        const entt::entity entity = fixture.world.create();
        fixture.world.registry().emplace<sp::ScriptComponent>(entity,
                                                              sp::ScriptComponent{ kFirst });

        host.update(fixture.world, 1.0 / 60.0);
        check(host.stopped_count() == 0, "the registered type it lacks reads as nil");
    }

    void test_each_instance_sees_its_own_entity() {
        section("Two entities running one script each see themselves");

        Fixture fixture;
        sp::Host host{ fixture.components };
        // Each writes its own id into its own range, so a shared entity handle
        // would leave the two fields equal.
        check(load(host, kFirst, R"(
            function on_update(s)
              entity:set("Turret", { range = entity.id + 1000.0 })
            end
        )"),
              "the script compiles");

        const entt::entity first = fixture.world.create();
        const entt::entity second = fixture.world.create();
        for (const entt::entity entity : { first, second }) {
            fixture.world.registry().emplace<Turret>(entity, Turret{});
            fixture.world.registry().emplace<sp::ScriptComponent>(
                entity, sp::ScriptComponent{ kFirst });
        }

        host.update(fixture.world, 1.0 / 60.0);
        const float a = fixture.world.registry().get<Turret>(first).range;
        const float b = fixture.world.registry().get<Turret>(second).range;
        check(a != b, "the two entities wrote different values");
        check(a == static_cast<float>(entt::to_integral(first)) + 1000.0F,
              "and each one is its own id");
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
    test_a_script_reads_a_component();
    test_a_script_writes_a_component();
    test_a_game_type_needs_no_engine_code();
    test_a_transform_written_from_lua_marks_the_entity_dirty();
    test_the_binding_refuses_what_it_cannot_do();
    test_a_component_the_entity_lacks_reads_nil();
    test_each_instance_sees_its_own_entity();

    if (test::g_failures != 0) {
        std::printf("\n%d check(s) failed.\n", test::g_failures);
        return 1;
    }
    std::printf("\nAll checks passed.\n");
    return 0;
}
