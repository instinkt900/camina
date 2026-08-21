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
#include "core/jobs.h"
#include "physics/components.h"
#include "physics/simulation.h"
#include "platform/input.h"
#include "scene/components.h"
#include "scene/prefab.h"
#include "scene/step_motion.h"
#include "scene/world.h"
#include "script/components.h"
#include "script/host.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

    using test::check;
    using test::section;
    namespace sc = engine::scene;
    namespace sp = engine::script;
    namespace pf = engine::platform;
    using engine::Vec3;

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

    /**
     * A clock a test can hold and read back.
     *
     * `play::Session` is the real one and it lives above `src/script/`, so this
     * is what the binding is driven against. What matters here is that the
     * `game` table reaches the interface, not what a session does when it is
     * paused. tests/test_session.cpp covers that half.
     */
    class FakeClock final : public sp::GameClock {
    public:
        void set_paused(bool paused) override {
            paused_ = paused;
            ++writes;
        }
        [[nodiscard]] bool paused() const override { return paused_; }

        /// How many times a script asked for a change, so a test can tell a
        /// call that did nothing from one that was never made.
        int writes = 0;

    private:
        bool paused_ = false;
    };

    /**
     * A quit request a test can read back.
     *
     * `play::Session` is the real one. What matters here is that `game.quit`
     * reaches the interface, not what an application does when it is asked.
     */
    class FakeExit final : public sp::GameExit {
    public:
        void request_quit() override { ++asked; }
        [[nodiscard]] bool quit_requested() const override { return asked > 0; }

        /// How many times a script asked, so a test can tell one call from none.
        int asked = 0;
    };

    /**
     * A UI surface a test drives, with no moth_ui under it.
     *
     * M10.6 puts `script::UiSurface` between the binding and the game UI, so
     * `engine_core` never names a moth_ui type. That seam is what lets this file
     * check the whole `ui` table with no window, no device and no layout file.
     * The real one is `engine::ui::ScriptSurface`.
     */
    class FakeUi final : public sp::UiSurface {
    public:
        /// One node of one layout.
        struct Node {
            std::string text;
            std::string image;
            bool visible = true;
        };

        /// Adds a layout with the named nodes, hidden to start with.
        void add(std::string layout, const std::vector<std::string>& nodes) {
            Layout& made = layouts_[layout];
            made.shown = false;
            for (const std::string& node : nodes) {
                made.nodes[node] = Node{};
            }
        }

        /// Records a press, the way a frame does between two steps.
        void press(std::string layout, std::string node) {
            presses_.push_back(sp::UiPress{ std::move(layout), std::move(node) });
        }

        /// Records a rebuild, the way a hot reload does.
        void reloaded(std::string layout) { reloads_.push_back(std::move(layout)); }

        /// Reads one node back, for a check that Lua wrote what it meant to.
        [[nodiscard]] const Node* node_of(const std::string& layout,
                                          const std::string& node) const {
            const auto found = layouts_.find(layout);
            if (found == layouts_.end()) {
                return nullptr;
            }
            const auto child = found->second.nodes.find(node);
            return child == found->second.nodes.end() ? nullptr : &child->second;
        }

        bool show(std::string_view layout) override {
            return set_shown(layout, true);
        }
        bool hide(std::string_view layout) override {
            return set_shown(layout, false);
        }
        [[nodiscard]] bool visible(std::string_view layout) const override {
            const Layout* found = find_layout(layout);
            return found != nullptr && found->shown;
        }
        [[nodiscard]] bool has_node(std::string_view layout,
                                    std::string_view node) const override {
            return find_node(layout, node) != nullptr;
        }
        [[nodiscard]] std::string text(std::string_view layout,
                                       std::string_view node) const override {
            const Node* found = find_node(layout, node);
            return found == nullptr ? std::string{} : found->text;
        }
        bool set_text(std::string_view layout, std::string_view node,
                      std::string_view text) override {
            Node* found = find_node(layout, node);
            if (found == nullptr) {
                return false;
            }
            found->text = std::string{ text };
            return true;
        }
        [[nodiscard]] bool node_visible(std::string_view layout,
                                        std::string_view node) const override {
            const Node* found = find_node(layout, node);
            return found != nullptr && found->visible;
        }
        bool set_node_visible(std::string_view layout, std::string_view node,
                              bool visible) override {
            Node* found = find_node(layout, node);
            if (found == nullptr) {
                return false;
            }
            found->visible = visible;
            return true;
        }
        bool set_image(std::string_view layout, std::string_view node,
                       std::string_view image) override {
            Node* found = find_node(layout, node);
            if (found == nullptr) {
                return false;
            }
            found->image = std::string{ image };
            return true;
        }
        [[nodiscard]] std::span<const sp::UiPress> presses() const override {
            return presses_;
        }
        void clear_presses() override { presses_.clear(); }

        [[nodiscard]] std::span<const std::string> reloads() const override {
            return reloads_;
        }
        void clear_reloads() override { reloads_.clear(); }

    private:
        struct Layout {
            bool shown = false;
            std::map<std::string, Node> nodes;
        };

        [[nodiscard]] const Layout* find_layout(std::string_view layout) const {
            const auto found = layouts_.find(std::string{ layout });
            return found == layouts_.end() ? nullptr : &found->second;
        }

        [[nodiscard]] const Node* find_node(std::string_view layout,
                                            std::string_view node) const {
            const Layout* found = find_layout(layout);
            if (found == nullptr) {
                return nullptr;
            }
            const auto child = found->nodes.find(std::string{ node });
            return child == found->nodes.end() ? nullptr : &child->second;
        }

        [[nodiscard]] Node* find_node(std::string_view layout, std::string_view node) {
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
            return const_cast<Node*>(std::as_const(*this).find_node(layout, node));
        }

        bool set_shown(std::string_view layout, bool shown) {
            const auto found = layouts_.find(std::string{ layout });
            if (found == layouts_.end()) {
                return false;
            }
            found->second.shown = shown;
            return true;
        }

        std::map<std::string, Layout> layouts_;
        std::vector<sp::UiPress> presses_;
        std::vector<std::string> reloads_;
    };

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

    /// Loads @p text again under @p guid, and reports whether it compiled.
    [[nodiscard]] bool reload(sp::Host& host, engine::Guid guid, std::string_view text) {
        return host.reload(guid, "test.lua", bytes_of(text));
    }

    /**
     * Runs the two steps a restart takes, and says what the instance did.
     *
     * A reload marks the instance rather than tearing it down there and then,
     * because `on_destroy` and `on_start` need the world and the services of a
     * step. So the sync drops it on the next update() and builds it again on the
     * one after, the same as an entity that changed which script it names.
     */
    void settle_reload(sp::Host& host, sc::World& world) {
        host.update(world, 1.0);
        host.update(world, 2.0);
    }

    void test_a_reload_restarts_the_instance() {
        section("A reload runs on_destroy, throws the table away, and runs on_start");

        sp::Host host;
        check(load(host, kFirst, R"(
            function on_start() end
            function on_update(s) end
            function on_destroy() end
        )"),
              "the first text compiles");

        sc::World world;
        (void)with_script(world, kFirst);

        host.update(world, 0.0);
        check(host.call_count(sp::Callback::Start) == 1, "the entity started once");
        check(host.call_count(sp::Callback::Destroy) == 0, "and has not been torn down");

        check(reload(host, kFirst, R"(
            function on_start() end
            function on_update(s) end
            function on_destroy() end
        )"),
              "the new text compiles");

        settle_reload(host, world);

        // Both halves. Only on_destroy would be a leak, and only on_start would
        // be an instance built over one nobody told it was going.
        check(host.call_count(sp::Callback::Destroy) == 1, "the old instance was torn down");
        check(host.call_count(sp::Callback::Start) == 2, "and the new one was started");
        check(host.instance_count() == 1, "and the entity is running again");
        check(host.restart_count() == 1, "which counts as one restart");
        check(host.stopped_count() == 0, "with nothing stopped");
    }

    void test_a_reload_throws_the_script_table_away() {
        section("A value a script kept in its own table does not survive a reload");

        // The rule, and it is deliberate. Carrying a table across two versions
        // of a chunk has no answer for a value whose shape changed, and the
        // wrong answer there reads as a game bug. State that has to survive goes
        // on a component, which the test below checks.
        sp::Host host;
        const std::string_view text = R"(
            counter = 0
            function on_update(s)
              counter = counter + 1
              if counter > 2 then error("the table survived the reload") end
            end
        )";
        check(load(host, kFirst, text), "the script compiles");

        sc::World world;
        (void)with_script(world, kFirst);

        // Two steps, so the counter is at its ceiling. A third with the same
        // table would raise, and that is what says the table went away.
        host.update(world, 0.0);
        host.update(world, 1.0);
        check(host.stopped_count() == 0, "two steps are within what the script allows");

        check(reload(host, kFirst, text), "the same text loads again");

        // settle_reload runs on_update once on the fresh instance, so the
        // counter is at 1 after it. A table that survived would be at 3 and the
        // script would have raised inside settle_reload itself.
        settle_reload(host, world);
        host.update(world, 3.0);
        check(host.stopped_count() == 0, "the counter started from zero again");
    }

    void test_component_state_survives_a_reload() {
        section("State a script put on a component is still there after a reload");

        // The other half of the rule above. A reload rebuilds the Lua instance
        // and no entity, so nothing it wrote to the world moves.
        sc::ComponentRegistry components;
        components.add<engine::Transform>();
        sp::Host host{ components };

        check(load(host, kFirst, R"(
            function on_start()
              entity:set("Transform", { position = vec3(0, 7, 0) })
            end
            function on_update(s) end
        )"),
              "the writing script compiles");

        sc::World world;
        const entt::entity entity = world.create();
        world.set_local(entity, engine::Transform{});
        world.registry().emplace<sp::ScriptComponent>(entity, sp::ScriptComponent{ kFirst });

        host.update(world, 0.0);
        check(world.local(entity).position.y == 7.0F, "the script wrote the component");

        // The new text reads the component rather than writing it, and raises
        // when the value is not the one the old version left. So the check is
        // the reload seeing state the previous version put there.
        check(reload(host, kFirst, R"(
            function on_start()
              local t = entity:get("Transform")
              if t == nil then error("no transform after the reload") end
              if math.abs(t.position.y - 7) > 1e-4 then error("the component was reset") end
            end
            function on_update(s) end
        )"),
              "the reading script compiles");

        settle_reload(host, world);
        check(host.call_count(sp::Callback::Start) == 2, "the new version started");
        check(host.stopped_count() == 0, "and it found the value the old one wrote");
        check(world.local(entity).position.y == 7.0F, "which is still on the entity");
    }

    void test_a_reload_that_will_not_compile_changes_nothing() {
        section("A script that will not compile leaves the old one running");

        // A person saves in the middle of an edit, and the file is briefly not
        // Lua. Taking the game down for that would make the whole feature worse
        // than restarting by hand.
        sp::Host host;
        check(load(host, kFirst, R"(
            function on_update(s) end
        )"),
              "the good text compiles");

        sc::World world;
        (void)with_script(world, kFirst);
        host.update(world, 0.0);
        const std::size_t before = host.call_count(sp::Callback::Update);

        check(!reload(host, kFirst, "function on_update(s) this is not lua end"),
              "the broken text is refused");

        settle_reload(host, world);
        check(host.instance_count() == 1, "the instance was never torn down");
        check(host.call_count(sp::Callback::Destroy) == 0, "so on_destroy did not run");
        check(host.restart_count() == 0, "and nothing was restarted");
        check(host.call_count(sp::Callback::Update) > before, "the old text is still running");
        check(host.stopped_count() == 0, "and nothing stopped");
    }

    void test_a_reload_reaches_every_entity_sharing_the_script() {
        section("Reloading one script restarts every entity running it");

        sp::Host host;
        // Both declare on_destroy, because the count below is what says the
        // entity on the other script was never torn down. A script with no
        // on_destroy is skipped and counts nothing, so the check would pass
        // whether the instance was dropped or not.
        const std::string_view shared = "function on_update(s) end\nfunction on_destroy() end\n";
        check(load(host, kFirst, shared), "the shared script compiles");
        check(load(host, kSecond, shared), "and the other one");

        sc::World world;
        (void)with_script(world, kFirst);
        (void)with_script(world, kFirst);
        (void)with_script(world, kFirst);
        (void)with_script(world, kSecond);

        host.update(world, 0.0);
        check(host.instance_count() == 4, "four entities are running");

        check(reload(host, kFirst, shared), "the shared script reloads");
        settle_reload(host, world);

        // Three and not one. A reload that reached whichever instance the map
        // happened to hand back first would pass a count of instances and fail
        // this.
        check(host.restart_count() == 3, "all three entities sharing it restarted");
        check(host.instance_count() == 4, "and every entity is running again");
        check(host.call_count(sp::Callback::Destroy) == 3, "the entity on the other script "
                                                           "was left alone");
    }

    void test_a_reload_revives_a_stopped_instance() {
        section("Fixing a script and saving it starts the entity again");

        // An error stops that instance and it is never called again, which is
        // what keeps a broken script off the frame. Without this, the only way
        // back is a scene reload, and the person who just fixed the file has no
        // reason to expect that.
        sp::Host host;
        check(load(host, kFirst, "function on_update(s) error('boom') end\n"),
              "the throwing script compiles");

        sc::World world;
        (void)with_script(world, kFirst);

        host.update(world, 0.0);
        check(host.stopped_count() == 1, "the instance stopped");

        check(reload(host, kFirst, "function on_update(s) end\n"), "the fixed text loads");
        settle_reload(host, world);

        check(host.stopped_count() == 0, "the stopped instance is gone");
        check(host.instance_count() == 1, "and the entity is running the new text");

        const std::size_t before = host.call_count(sp::Callback::Update);
        host.update(world, 5.0);
        check(host.call_count(sp::Callback::Update) == before + 1, "and it updates again");
    }

    void test_a_plain_load_does_not_restart_anything() {
        section("Loading a script the host already holds restarts nothing");

        // load() is what startup uses, and it walks every cooked script. A load
        // that looked like a reload would tear every instance down and build it
        // again each time somebody called it.
        sp::Host host;
        const std::string_view text =
            "function on_start() end\nfunction on_update(s) end\nfunction on_destroy() end\n";
        check(load(host, kFirst, text), "the script compiles");

        sc::World world;
        (void)with_script(world, kFirst);
        host.update(world, 0.0);

        check(load(host, kFirst, text), "and it loads again");
        settle_reload(host, world);

        check(host.restart_count() == 0, "nothing restarted");
        check(host.call_count(sp::Callback::Destroy) == 0, "and on_destroy never ran");
        check(host.call_count(sp::Callback::Start) == 1, "and on_start ran once, at the start");
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

    void test_the_handle_follows_the_world_of_the_step() {
        section("An instance reads the world it is being stepped with");

        sc::ComponentRegistry components;
        components.add<engine::Transform>();
        components.add<Turret>();
        sp::Host host{ components };

        // Writes its own range from the Turret it finds. Run against a second
        // world holding a different value, it has to see the second one.
        check(load(host, kFirst, R"(
            function on_update(s)
              local t = entity:get("Turret")
              if t == nil then error("no world") end
              seen = t.range
              entity:set("Turret", { label = "visited" })
            end
        )"),
              "the script compiles");

        sc::World first;
        const entt::entity a = first.create();
        first.registry().emplace<Turret>(a, Turret{ .range = 1.0F });
        first.registry().emplace<sp::ScriptComponent>(a, sp::ScriptComponent{ kFirst });
        host.update(first, 1.0 / 60.0);
        check(first.registry().get<Turret>(a).label == "visited", "the first world was written");

        // A different world, the way a scene reload would give one. The
        // instance was made against the first, and it must not read that one.
        sc::World second;
        const entt::entity b = second.create();
        second.registry().emplace<Turret>(b, Turret{ .range = 2.0F });
        second.registry().emplace<sp::ScriptComponent>(b, sp::ScriptComponent{ kFirst });

        host.update(second, 2.0 / 60.0);
        check(host.stopped_count() == 0, "the script ran against the second world");
        check(second.registry().get<Turret>(b).label == "visited",
              "and it wrote the entity of that world");
    }

    void test_on_destroy_can_still_read_its_entity() {
        section("A teardown reads the world stop() was given");

        sc::ComponentRegistry components;
        components.add<Turret>();
        sp::Host host{ components };

        // on_destroy is where a script gives something back, so it has to be
        // able to read the entity. stop() sets the world for that reason, and
        // an entity kept in a table has to reach the same world rather than
        // whichever one made the instance.
        check(load(host, kFirst, R"(
            kept = nil
            function on_update(s) kept = entity end
            function on_destroy()
              if kept == nil then error("nothing kept") end
              local t = kept:get("Turret")
              if t == nil then error("on_destroy could not read its entity") end
              if t.range ~= 12.0 then error("read the wrong world") end
            end
        )"),
              "the script compiles");

        sc::World world;
        const entt::entity entity = world.create();
        world.registry().emplace<Turret>(entity, Turret{});
        world.registry().emplace<sp::ScriptComponent>(entity, sp::ScriptComponent{ kFirst });

        host.update(world, 1.0 / 60.0);
        check(host.instance_count() == 1, "it ran and kept the entity");

        host.stop(world);
        check(host.call_count(sp::Callback::Destroy) == 1, "on_destroy ran");
        check(host.stopped_count() == 0, "and it read the world stop() was given");
    }

    /// Runs one script against one entity and reports whether it raised.
    [[nodiscard]] bool run_ok(std::string_view text) {
        sc::ComponentRegistry components;
        components.add<engine::Transform>();
        components.add<Turret>();
        sp::Host host{ components };
        if (!load(host, kFirst, text)) {
            return false;
        }

        sc::World world;
        const entt::entity entity = world.create();
        world.registry().emplace<Turret>(entity, Turret{});
        world.registry().emplace<sp::ScriptComponent>(entity, sp::ScriptComponent{ kFirst });

        host.update(world, 1.0 / 60.0);
        return host.stopped_count() == 0 && host.call_count(sp::Callback::Update) == 1;
    }

    void test_vec3_has_the_operators_an_author_expects() {
        section("vec3 does arithmetic rather than making a script call functions");

        check(run_ok(R"(
            function on_update(s)
              local a = vec3(1, 2, 3)
              local b = vec3(10, 20, 30)
              local sum = a + b
              if sum.x ~= 11 or sum.z ~= 33 then error("addition") end
              if (b - a).y ~= 18 then error("subtraction") end
              if (a * 2).z ~= 6 then error("vector times scalar") end
              if (2 * a).z ~= 6 then error("scalar times vector") end
              if (b / 10).x ~= 1 then error("division") end
              if (-a).x ~= -1 then error("negation") end
              if a ~= vec3(1, 2, 3) then error("equality") end
              if a:dot(vec3(1, 0, 0)) ~= 1 then error("dot") end
              if vec3(1,0,0):cross(vec3(0,1,0)).z ~= 1 then error("cross") end
              if vec3(3, 4, 0):length() ~= 5 then error("length") end
              if vec3(0, 5, 0):normalized().y ~= 1 then error("normalized") end
            end
        )"),
              "every operator and method behaves");
    }

    void test_normalizing_a_zero_vector_gives_no_nan() {
        section("A vector too short to normalize gives zero, not NaN");

        // glm::normalize divides by the length, so this would otherwise fill
        // every component with NaN and spread it through whatever came next.
        check(run_ok(R"(
            function on_update(s)
              local n = vec3(0, 0, 0):normalized()
              if n.x ~= 0 or n.y ~= 0 or n.z ~= 0 then error("not zero") end
              if n.x ~= n.x then error("NaN") end
            end
        )"),
              "it comes back as zero");
    }

    void test_an_axis_of_no_direction_gives_identity() {
        section("A rotation about a zero axis is identity, not a zero quaternion");

        // glm::angleAxis wants a unit axis and does not check. With a zero axis
        // it gives quat(cos(angle / 2), 0, 0, 0), so half a turn is
        // quat(0, 0, 0, 0). That is not a rotation, and nothing reports it:
        // multiplying a vector by it and casting it to a matrix both read as
        // identity, because every term is zero either way.
        check(run_ok(R"(
            function on_update(s)
              local q = quat_from_axis_angle(vec3(0, 0, 0), math.pi)
              if q.w ~= 1 then error("w is " .. tostring(q.w) .. ", not identity") end
              if q.x ~= 0 or q.y ~= 0 or q.z ~= 0 then error("not identity") end

              -- The part a zero quaternion would fail: it has to be a unit
              -- quaternion, so writing it to a rotation field reads back whole.
              entity:set("Transform", { rotation = q })
              local back = entity:get("Transform").rotation
              if back.w ~= 1 then error("the rotation did not round trip") end
            end
        )"),
              "it comes back as identity and round trips through a field");
    }

    void test_quat_turns_a_vector() {
        section("A quaternion composes and turns a vector");

        check(run_ok(R"(
            function on_update(s)
              local half_pi = math.pi / 2
              local q = quat_from_axis_angle(vec3(0, 1, 0), half_pi)
              -- Turning +X a quarter turn about +Y gives -Z, right-handed.
              local turned = q * vec3(1, 0, 0)
              if math.abs(turned.z + 1) > 1e-5 then error("wrong axis: " .. turned.z) end
              if math.abs(turned.x) > 1e-5 then error("x did not clear") end

              local twice = q * q
              local back = twice * vec3(1, 0, 0)
              if math.abs(back.x + 1) > 1e-5 then error("composing two turns") end

              local identity = quat(1, 0, 0, 0)
              if (identity * vec3(2, 3, 4)).y ~= 3 then error("identity moved it") end
            end
        )"),
              "it turns and composes the way the conventions say");
    }

    void test_a_component_vector_is_the_engine_type() {
        section("A vector read from a component is a vec3, not a table");

        sc::ComponentRegistry components;
        components.add<engine::Transform>();
        components.add<Turret>();
        sp::Host host{ components };

        // Read, do arithmetic on it, and write it straight back. That round
        // trip is the reason the binding hands back the engine type.
        check(load(host, kFirst, R"(
            function on_update(s)
              local t = entity:get("Turret")
              entity:set("Turret", { aim = t.aim + vec3(1, 1, 1) })
            end
        )"),
              "the script compiles");

        sc::World world;
        const entt::entity entity = world.create();
        world.registry().emplace<Turret>(entity, Turret{});
        world.registry().emplace<sp::ScriptComponent>(entity, sp::ScriptComponent{ kFirst });

        host.update(world, 1.0 / 60.0);
        check(host.stopped_count() == 0, "it ran");

        const Turret& turret = world.registry().get<Turret>(entity);
        check(turret.aim.x == 1.0F && turret.aim.y == 1.0F && turret.aim.z == 0.0F,
              "the vector came back through arithmetic and landed on the field");
    }

    void test_a_partial_table_still_writes() {
        section("A table naming one component still works beside the vec3 form");

        sc::ComponentRegistry components;
        components.add<Turret>();
        sp::Host host{ components };
        check(load(host, kFirst, "function on_update(s) entity:set(\"Turret\", "
                                 "{ aim = { y = 9.0 } }) end"),
              "the script compiles");

        sc::World world;
        const entt::entity entity = world.create();
        world.registry().emplace<Turret>(entity, Turret{});
        world.registry().emplace<sp::ScriptComponent>(entity, sp::ScriptComponent{ kFirst });

        host.update(world, 1.0 / 60.0);
        const Turret& turret = world.registry().get<Turret>(entity);
        check(turret.aim.y == 9.0F, "the named component was written");
        check(turret.aim.z == -1.0F, "and the rest was kept");
    }

    void test_the_random_source_is_reproducible() {
        section("Two hosts in one process draw the same numbers");

        // DESIGN.md section 9 rests a reproducible run on the fixed step, and
        // Lua seeds math.random from the clock unless something says otherwise.
        // Two hosts built in one process stand in for two runs of one command.
        const auto draw = [](sp::Host& host, sc::World& world) {
            host.update(world, 1.0 / 60.0);
        };

        sc::ComponentRegistry components;
        components.add<Turret>();

        constexpr std::string_view kScript = R"(
            function on_update(s)
              entity:set("Turret", { range = math.random() })
            end
        )";

        float first = 0.0F;
        float second = 0.0F;
        for (float* out : { &first, &second }) {
            sp::Host host{ components };
            check(load(host, kFirst, kScript), "the script compiles");
            sc::World world;
            const entt::entity entity = world.create();
            world.registry().emplace<Turret>(entity, Turret{});
            world.registry().emplace<sp::ScriptComponent>(entity,
                                                          sp::ScriptComponent{ kFirst });
            draw(host, world);
            *out = world.registry().get<Turret>(entity).range;
        }

        check(first != 0.0F, "the script drew a number");
        check(first == second, "and two hosts drew the same one");
    }

    void test_reseeding_from_the_clock_is_not_available() {
        section("math.randomseed refuses the form that reads the clock");

        // The no-argument form seeds from the clock, which would undo the seed
        // the host set without a script meaning to. Making it unavailable is
        // what turns the guarantee from a note into something structural.
        //
        // The rejected call has to sit between a known seed and the draw, with
        // nothing reseeding after it. An earlier version of this test called
        // math.randomseed(1) again before drawing, which reset the generator
        // and made the check pass whether the clock form was refused or not.
        check(run_ok(R"(
            function on_update(s)
              math.randomseed(7)
              local expected = math.random()

              math.randomseed(7)
              math.randomseed()
              local actual = math.random()
              if actual ~= expected then
                error("math.randomseed() reseeded, so the run is not reproducible")
              end

              -- And a named seed still works, or the guard would be doing its
              -- job by breaking the whole function.
              math.randomseed(8)
              local other = math.random()
              if other == expected then error("a different seed drew the same number") end
            end
        )"),
              "the clock form changes nothing and a named seed still works");
    }

    void test_the_world_finds_and_walks() {
        section("A script finds an entity by name and walks its children");

        sc::ComponentRegistry components;
        components.add<engine::Transform>();
        components.add<engine::scene::Name>();
        sp::Host host{ components };

        // The children come back as a sequence, so ipairs walks them in order.
        // pairs over a string-keyed table would not, and that is the difference
        // between a reproducible run and one that fails once in ten.
        check(load(host, kFirst, R"(
            function on_update(s)
              local stack = world.find("stack")
              if stack == nil then error("did not find the stack") end
              if stack:name() ~= "stack" then error("wrong name back") end

              local kids = stack:children()
              if #kids ~= 3 then error("wrong child count: " .. #kids) end

              local names = {}
              for i, child in ipairs(kids) do names[i] = child:name() end
              if names[1] ~= "a" or names[2] ~= "b" or names[3] ~= "c" then
                error("children came back out of order: " .. table.concat(names, ","))
              end

              if kids[1]:parent():name() ~= "stack" then error("wrong parent") end
              if stack:parent() ~= nil then error("a root has a parent") end
              if world.find("nothing_called_this") ~= nil then error("found a ghost") end
            end
        )"),
              "the script compiles");

        sc::World world;
        const entt::entity stack = world.create();
        world.registry().emplace<engine::scene::Name>(stack, engine::scene::Name{ "stack" });
        for (const char* name : { "a", "b", "c" }) {
            const entt::entity child = world.create();
            world.registry().emplace<engine::scene::Name>(child, engine::scene::Name{ name });
            check(world.set_parent(child, stack), "the child parents");
        }
        world.registry().emplace<sp::ScriptComponent>(stack, sp::ScriptComponent{ kFirst });

        host.update(world, 1.0 / 60.0);
        check(host.stopped_count() == 0, "every lookup answered as the script expected");
    }

    void test_a_script_creates_and_destroys() {
        section("A script creates an entity and destroys one");

        sc::ComponentRegistry components;
        components.add<engine::Transform>();
        sp::Host host{ components };
        check(load(host, kFirst, R"(
            made = nil
            function on_update(s)
              if made == nil then
                made = world.create()
                if made == nil then error("create gave nothing") end
                if not made:valid() then error("a new entity is not valid") end
              else
                if not world.destroy(made) then error("destroy refused") end
                if made:valid() then error("it survived being destroyed") end
                if world.destroy(made) then error("destroying it twice worked") end
              end
            end
        )"),
              "the script compiles");

        sc::World world;
        const entt::entity entity = world.create();
        world.registry().emplace<sp::ScriptComponent>(entity, sp::ScriptComponent{ kFirst });

        const std::size_t before = world.size();
        host.update(world, 1.0 / 60.0);
        check(world.size() == before + 1, "the first step made one entity");

        host.update(world, 2.0 / 60.0);
        check(world.size() == before, "and the second step destroyed it");
        check(host.stopped_count() == 0, "with no error either way");
    }

    void test_input_reaches_a_script_by_action_name() {
        section("A script reads an action by name and never an SDL constant");

        sc::ComponentRegistry components;
        components.add<engine::Transform>();
        components.add<Turret>();
        sp::Host host{ components };
        check(load(host, kFirst, R"(
            function on_update(s)
              if input.held("fire") then entity:set("Turret", { armed = true }) end
              if input.pressed("fire") then entity:set("Turret", { range = 99.0 }) end
              if input.held("nothing_bound") then error("an unbound action read true") end
            end
        )"),
              "the script compiles");

        sc::World world;
        const entt::entity entity = world.create();
        world.registry().emplace<Turret>(entity, Turret{});
        world.registry().emplace<sp::ScriptComponent>(entity, sp::ScriptComponent{ kFirst });

        engine::platform::Input input;
        input.bind("fire", engine::platform::Key::F);

        // No input in the services at all, which is what an offscreen run gives.
        host.update(world, 1.0 / 60.0);
        check(!world.registry().get<Turret>(entity).armed,
              "a step with no input module reads every action as false");

        pf::InputFrame frame;
        frame.focused = true;
        frame.keys.at(static_cast<std::size_t>(engine::platform::Key::F)) = true;
        input.update(frame);

        host.update(world, 2.0 / 60.0, sp::Services{ .input = &input });
        const Turret& turret = world.registry().get<Turret>(entity);
        check(turret.armed, "a held action reaches the script");
        check(turret.range == 99.0F, "and so does the press edge");
        check(host.stopped_count() == 0, "and an unbound action reads false");
    }

    void test_the_services_follow_the_step() {
        section("A service passed one step is gone the next");

        // The same rule the world follows, for the same reason. An instance
        // lives across steps, so anything captured when it was made would
        // outlive the call that supplied it. See issue #273.
        sc::ComponentRegistry components;
        components.add<engine::Transform>();
        components.add<Turret>();
        sp::Host host{ components };
        check(load(host, kFirst, R"(
            function on_update(s)
              entity:set("Turret", { armed = input.held("fire") })
            end
        )"),
              "the script compiles");

        sc::World world;
        const entt::entity entity = world.create();
        world.registry().emplace<Turret>(entity, Turret{});
        world.registry().emplace<sp::ScriptComponent>(entity, sp::ScriptComponent{ kFirst });

        engine::platform::Input input;
        input.bind("fire", engine::platform::Key::F);
        pf::InputFrame frame;
        frame.focused = true;
        frame.keys.at(static_cast<std::size_t>(engine::platform::Key::F)) = true;
        input.update(frame);

        host.update(world, 1.0 / 60.0, sp::Services{ .input = &input });
        check(world.registry().get<Turret>(entity).armed, "the action read true with input");

        host.update(world, 2.0 / 60.0);
        check(!world.registry().get<Turret>(entity).armed,
              "and false on a step that passed none");
    }

    void test_a_script_instances_a_prefab() {
        section("A script instances a prefab by name, which is what the throw does");

        sc::ComponentRegistry components;
        components.add<engine::Transform>();
        components.add<engine::scene::Name>();
        sp::Host host{ components };

        sc::PrefabLibrary library;
        check(library.add("crate.prefab", nlohmann::json::parse(R"({
            "__version": 1,
            "entities": [{ "parent": -1, "components": {
                "Name": { "__version": 1, "value": "crate" },
                "Transform": { "position": [0.0, 5.0, 0.0] }
            }}]
        })")),
              "the prefab parses");

        check(load(host, kFirst, R"(
            function on_update(s)
              local made = world.instance("crate.prefab")
              if made == nil then error("instance gave nothing") end
              if made:name() ~= "crate" then error("wrong prefab came back") end

              -- A throw is this and a velocity, so the pose has to be settable
              -- on the entity that just came back.
              made:set("Transform", { position = vec3(1, 2, 3) })
              local t = made:get("Transform")
              if t.position.y ~= 2 then error("the new entity did not move") end

              if world.instance("no_such.prefab") ~= nil then
                error("instanced a prefab nobody loaded")
              end
            end
        )"),
              "the script compiles");

        sc::World world;
        const entt::entity entity = world.create();
        world.registry().emplace<sp::ScriptComponent>(entity, sp::ScriptComponent{ kFirst });

        const std::size_t before = world.size();
        host.update(world, 1.0 / 60.0, sp::Services{ .prefabs = &library });
        check(host.stopped_count() == 0, "the script ran");
        check(world.size() == before + 1, "and the world gained the instanced entity");

        // Without a library in the services there is nothing to instance from,
        // and the script has to be told rather than left guessing.
        host.update(world, 2.0 / 60.0);
        check(host.stopped_count() == 1, "a step with no library refuses and the script sees it");
    }

    void test_the_physics_verbs_reach_a_script() {
        section("A script reads a velocity, pushes a body, and asks if it sleeps");

        sc::ComponentRegistry components;
        components.add<engine::Transform>();
        engine::physics::register_components(components);
        sp::Host host{ components };

        check(load(host, kFirst, R"(
            function on_update(s)
              if entity:velocity() == nil then error("no velocity to read") end
              if not entity:is_awake() then error("a new body is asleep") end
              if not entity:set_velocity(vec3(0, 0, 5)) then error("set_velocity refused") end
              if math.abs(entity:velocity().z - 5) > 1e-4 then error("the velocity did not take") end
              if not entity:impulse(vec3(0, 1, 0)) then error("impulse refused") end
              if entity:velocity().y <= 0 then error("the impulse did nothing") end
            end
        )"),
              "the script compiles");

        sc::World world;
        const entt::entity entity = world.create();
        world.set_local(entity, engine::Transform{ .position = { 0.0F, 5.0F, 0.0F } });
        world.registry().emplace<engine::physics::RigidBody>(
            entity, engine::physics::RigidBody{ .type = engine::physics::BodyType::Dynamic });
        world.registry().emplace<engine::physics::BoxCollider>(
            entity, engine::physics::BoxCollider{});
        world.registry().emplace<sp::ScriptComponent>(entity, sp::ScriptComponent{ kFirst });

        engine::physics::Simulation simulation;
        simulation.build(world);

        host.update(world, 1.0 / 60.0, sp::Services{ .physics = &simulation });
        check(host.stopped_count() == 0, "every physics verb answered");

        // A step with no simulation leaves them all nil or false, rather than
        // reaching a simulation the caller is no longer stepping.
        check(load(host, kSecond, R"(
            function on_update(s)
              if entity:velocity() ~= nil then error("read a velocity with no simulation") end
              if entity:set_velocity(vec3(1, 0, 0)) then error("wrote one") end
              if entity:is_awake() then error("claimed a body was awake") end
            end
        )"),
              "the second script compiles");
        // Naming a different script drops the old instance on the step that
        // sees the change, and the next step starts the new one.
        world.registry().replace<sp::ScriptComponent>(entity, sp::ScriptComponent{ kSecond });
        host.update(world, 2.0 / 60.0);
        check(host.instance_count() == 0, "changing the script drops the old instance");

        host.update(world, 3.0 / 60.0);
        check(host.instance_count() == 1, "and the next step starts the new one");
        check(host.stopped_count() == 0, "and a step with no simulation answers nothing");
    }

    /**
     * Issue #284. A Transform written to a dynamic body must not freeze it.
     *
     * The write used to register the entity with `scene::StepMotion`, whose
     * `begin_step` then put that pose back at the top of every step. The body
     * kept integrating and its velocity read back correctly, so everything
     * looked right except that the position never moved.
     *
     * The falling body is what makes this testable. A body standing still
     * cannot tell a freeze apart from working correctly.
     */
    void test_a_transform_written_to_a_dynamic_body_does_not_freeze_it() {
        section("A script writing a Transform to a dynamic body leaves it falling");

        sc::ComponentRegistry components;
        components.add<engine::Transform>();
        engine::physics::register_components(components);
        sp::Host host{ components };

        // The write every step is the shape that used to freeze it, and it is
        // what a person reaches for before learning about teleport().
        check(load(host, kFirst, R"(
            function on_update(s)
              entity:set("Transform", { position = vec3(0, 5, 0) })
            end
        )"),
              "the script compiles");

        sc::World world;
        const entt::entity entity = world.create();
        world.set_local(entity, engine::Transform{ .position = { 0.0F, 5.0F, 0.0F } });
        world.registry().emplace<engine::physics::RigidBody>(
            entity, engine::physics::RigidBody{ .type = engine::physics::BodyType::Dynamic });
        world.registry().emplace<engine::physics::BoxCollider>(entity,
                                                               engine::physics::BoxCollider{});
        world.registry().emplace<sp::ScriptComponent>(entity, sp::ScriptComponent{ kFirst });

        engine::physics::Simulation simulation;
        simulation.build(world);

        sc::StepMotion motion;
        constexpr double kStepSeconds = 1.0 / 60.0;
        for (std::uint32_t i = 0; i < 60; ++i) {
            motion.begin_step(world);
            host.update(world, static_cast<double>(i + 1) * kStepSeconds,
                        sp::Services{ .physics = &simulation, .motion = &motion });
            simulation.step(world, static_cast<float>(kStepSeconds));
        }

        // The order the runtime uses, and the order is the whole bug. Both
        // blend the same pair of steps, and StepMotion writes last, so a pose
        // recorded there wins over the one the solver produced.
        simulation.interpolate(world, 1.0F);
        motion.interpolate(world, 1.0F);
        world.update();

        check(host.stopped_count() == 0, "the script ran every step");

        // A second of gravity from rest is about 5 metres, and the body starts
        // at 5. Reaching the floor is not the point. Moving at all is.
        const float height = world.world_matrix(entity)[3][1];
        check(height < 4.0F, "the body kept falling while the script wrote its Transform");

        // The mechanism, not only the symptom. Recording it is what put the
        // pose back at the top of every step.
        check(motion.tracked() == 0, "and the write never reached StepMotion");
    }

    /**
     * The same write, on a body the entity owns rather than the solver.
     *
     * A kinematic body reads its pose off the entity, so writing the Transform
     * is the right way to move one. The guard must not take the interpolation
     * away from those, or every script-moved lift would judder between steps.
     */
    void test_a_transform_written_to_a_kinematic_body_still_interpolates() {
        section("A kinematic body keeps its step interpolation");

        sc::ComponentRegistry components;
        components.add<engine::Transform>();
        engine::physics::register_components(components);
        sp::Host host{ components };

        check(load(host, kFirst, R"(
            function on_update(s)
              entity:set("Transform", { position = vec3(0, s, 0) })
            end
        )"),
              "the script compiles");

        sc::World world;
        const entt::entity entity = world.create();
        world.set_local(entity, engine::Transform{ .position = { 0.0F, 0.0F, 0.0F } });
        world.registry().emplace<engine::physics::RigidBody>(
            entity, engine::physics::RigidBody{ .type = engine::physics::BodyType::Kinematic });
        world.registry().emplace<engine::physics::BoxCollider>(entity,
                                                               engine::physics::BoxCollider{});
        world.registry().emplace<sp::ScriptComponent>(entity, sp::ScriptComponent{ kFirst });

        engine::physics::Simulation simulation;
        simulation.build(world);

        sc::StepMotion motion;
        host.update(world, 1.0 / 60.0,
                    sp::Services{ .physics = &simulation, .motion = &motion });

        check(host.stopped_count() == 0, "the script ran");
        check(motion.tracked() == 1, "the entity that owns its own pose is still tracked");
    }

    void test_stop_does_not_reach_the_services_of_the_last_step() {
        section("A teardown reaches no service the caller did not pass to stop()");

        // stop() is called as the world goes away, and the simulation usually
        // goes with it. An on_destroy that kept an entity and pushed a body
        // through the services of the last update() would reach freed memory,
        // and nothing would report it. So stop() takes its own services and the
        // default is nothing.
        sc::ComponentRegistry components;
        components.add<engine::Transform>();
        engine::physics::register_components(components);
        sp::Host host{ components };

        check(load(host, kFirst, R"(
            kept = nil
            function on_update(s) kept = entity end
            function on_destroy()
              -- The world is still there, so a component still reads.
              if kept:get("Transform") == nil then error("lost the world too") end
              -- The services are not. This write has to be refused, and the
              -- velocity the caller set has to survive it.
              kept:set_velocity(vec3(0, 0, 99))
            end
        )"),
              "the script compiles");

        sc::World world;
        const entt::entity entity = world.create();
        world.set_local(entity, engine::Transform{ .position = { 0.0F, 5.0F, 0.0F } });
        world.registry().emplace<engine::physics::RigidBody>(
            entity, engine::physics::RigidBody{ .type = engine::physics::BodyType::Dynamic });
        world.registry().emplace<engine::physics::BoxCollider>(
            entity, engine::physics::BoxCollider{});
        world.registry().emplace<sp::ScriptComponent>(entity, sp::ScriptComponent{ kFirst });

        // The simulation stays alive on purpose. Letting it go out of scope
        // first would make the bug this guards against undefined rather than
        // wrong, and a test cannot tell the two apart. Here the pointer would
        // still be good, so a stop() that kept it would read a real velocity
        // and the script would say so.
        engine::physics::Simulation simulation;
        simulation.build(world);
        host.update(world, 1.0 / 60.0, sp::Services{ .physics = &simulation });
        check(host.stopped_count() == 0, "the step ran with a simulation");

        // A value only the caller set, so a write from the teardown is visible.
        // stop() resets the stopped counter, so that cannot be the signal here.
        check(simulation.set_linear_velocity(entity, Vec3{ 0.0F, 0.0F, 7.0F }),
              "the caller sets a velocity");

        host.stop(world);
        check(host.call_count(sp::Callback::Destroy) == 1, "on_destroy ran");

        Vec3 after{ 0.0F, 0.0F, 0.0F };
        check(simulation.linear_velocity(entity, after), "the velocity reads back");
        check(after.z == 7.0F, "and the teardown could not write through a service");
    }

    /// A named entity with a body and a box collider, at a height.
    entt::entity add_body(sc::World& world, std::string_view name, engine::physics::BodyType type,
                          const Vec3& position, const Vec3& half_extents, bool is_trigger) {
        const entt::entity entity = world.create();
        world.set_local(entity, engine::Transform{ .position = position });
        world.registry().emplace<sc::Name>(entity, sc::Name{ std::string{ name } });
        world.registry().emplace<engine::physics::RigidBody>(
            entity, engine::physics::RigidBody{ .type = type });
        world.registry().emplace<engine::physics::BoxCollider>(
            entity, engine::physics::BoxCollider{ .half_extents = half_extents,
                                                  .is_trigger = is_trigger });
        return entity;
    }

    /// The registry a physics scene needs, which is the transform and the bodies.
    [[nodiscard]] sc::ComponentRegistry physics_registry() {
        sc::ComponentRegistry components;
        components.add<engine::Transform>();
        engine::physics::register_components(components);
        return components;
    }

    /**
     * Runs @p steps whole steps, and hands the events of each one over.
     *
     * The delivery goes inside the loop rather than after it, which is the whole
     * point: the simulation keeps the events of one step, so a caller that read
     * them once for several steps would report the last and lose the rest.
     */
    void run_steps(sp::Host& host, sc::World& world, engine::physics::Simulation& simulation,
                   std::uint32_t steps) {
        constexpr float kStep = 1.0F / 60.0F;
        for (std::uint32_t i = 0; i < steps; ++i) {
            host.update(world, static_cast<double>(i + 1) / 60.0,
                        sp::Services{ .physics = &simulation });
            simulation.step(world, kStep);
            host.deliver_physics_events(world, simulation,
                                        sp::Services{ .physics = &simulation });
        }
    }

    void test_a_trigger_reaches_the_volume_and_not_the_visitor() {
        section("A trigger event runs on the volume, and names what crossed it");

        // The script verifies itself and calls error() on anything it did not
        // expect, so stopped_count() reports a wrong pairing as well as a wrong
        // order. The count of calls is what says it ran at all.
        sc::ComponentRegistry components = physics_registry();
        sp::Host host{ components };

        check(load(host, kFirst, R"(
            calls = 0
            function on_trigger(other, began)
              calls = calls + 1
              if entity:name() ~= "goal" then error("this ran on the wrong entity") end
              if other:name() ~= "crate" then error("the visitor is not the crate") end
              if calls == 1 and not began then error("the first event was not the entry") end
              if calls == 2 and began then error("the second event was not the exit") end
              if calls > 2 then error("more events than one crossing can make") end
            end
        )"),
              "the goal script compiles");

        // The visitor is deaf on purpose. A trigger has a direction, and a
        // script on the thing that crossed must not hear the volume's event.
        check(load(host, kSecond, R"(
            function on_trigger(other, began) error("the visitor heard a trigger event") end
        )"),
              "the crate script compiles");

        sc::World world;
        const entt::entity goal = add_body(world, "goal", engine::physics::BodyType::Static,
                                           Vec3{ 0.0F, 2.0F, 0.0F }, Vec3{ 2.0F, 0.5F, 2.0F },
                                           true);
        const entt::entity crate = add_body(world, "crate", engine::physics::BodyType::Dynamic,
                                            Vec3{ 0.0F, 6.0F, 0.0F }, Vec3{ 0.5F, 0.5F, 0.5F },
                                            false);
        world.registry().emplace<sp::ScriptComponent>(goal, sp::ScriptComponent{ kFirst });
        world.registry().emplace<sp::ScriptComponent>(crate, sp::ScriptComponent{ kSecond });

        engine::physics::Simulation simulation;
        simulation.build(world);
        run_steps(host, world, simulation, 240);

        check(host.call_count(sp::Callback::Trigger) == 2,
              "the volume heard the crate enter and heard it leave");
        check(host.stopped_count() == 0, "and every check inside the script held");
    }

    void test_a_contact_reaches_both_bodies() {
        section("A contact runs on each of the two bodies, with the other as other");

        // Box3D fixes no order for a contact, so a one-sided call would land on
        // whichever body the solver listed first and which script heard a
        // collision would depend on solver internals.
        sc::ComponentRegistry components = physics_registry();
        sp::Host host{ components };

        check(load(host, kFirst, R"(
            function on_contact(other, began)
              if entity:name() ~= "floor" then error("this ran on the wrong entity") end
              if other:name() ~= "crate" then error("the floor was not touched by the crate") end
            end
        )"),
              "the floor script compiles");

        check(load(host, kSecond, R"(
            function on_contact(other, began)
              if entity:name() ~= "crate" then error("this ran on the wrong entity") end
              if other:name() ~= "floor" then error("the crate did not touch the floor") end
            end
        )"),
              "the crate script compiles");

        sc::World world;
        const entt::entity floor = add_body(world, "floor", engine::physics::BodyType::Static,
                                            Vec3{ 0.0F, -1.0F, 0.0F }, Vec3{ 50.0F, 1.0F, 50.0F },
                                            false);
        const entt::entity crate = add_body(world, "crate", engine::physics::BodyType::Dynamic,
                                            Vec3{ 0.0F, 3.0F, 0.0F }, Vec3{ 0.5F, 0.5F, 0.5F },
                                            false);
        world.registry().emplace<sp::ScriptComponent>(floor, sp::ScriptComponent{ kFirst });
        world.registry().emplace<sp::ScriptComponent>(crate, sp::ScriptComponent{ kSecond });

        engine::physics::Simulation simulation;
        simulation.build(world);

        // One step at a time, so the first contact can be measured on its own.
        // A total taken at the end would say nothing about the sides: one script
        // called twice and two scripts called once come to the same number.
        std::size_t before = 0;
        std::size_t gained = 0;
        for (std::uint32_t i = 0; i < 240 && gained == 0; ++i) {
            before = host.call_count(sp::Callback::Contact);
            run_steps(host, world, simulation, 1);
            gained = host.call_count(sp::Callback::Contact) - before;
        }

        check(gained == 2, "one contact between two scripted bodies makes two calls");
        check(host.stopped_count() == 0, "and each side was handed the other one");
    }

    void test_one_step_reports_every_event() {
        section("A step that makes several events reports all of them");

        // Collapsing the events of a step to one would lose the ones a puzzle
        // cares about, and it would make the result depend on the frame rate.
        // Two crates dropped side by side from one height cross on the same
        // step, so a delivery that reported one of them would be caught here.
        sc::ComponentRegistry components = physics_registry();
        sp::Host host{ components };

        check(load(host, kFirst, R"(
            function on_trigger(other, began) end
        )"),
              "the goal script compiles");

        sc::World world;
        const entt::entity goal = add_body(world, "goal", engine::physics::BodyType::Static,
                                           Vec3{ 0.0F, 2.0F, 0.0F }, Vec3{ 4.0F, 0.5F, 4.0F },
                                           true);
        world.registry().emplace<sp::ScriptComponent>(goal, sp::ScriptComponent{ kFirst });

        // Far enough apart never to touch each other, and at one height, so the
        // two crossings fall on the same step.
        (void)add_body(world, "left", engine::physics::BodyType::Dynamic,
                       Vec3{ -2.0F, 6.0F, 0.0F }, Vec3{ 0.5F, 0.5F, 0.5F }, false);
        (void)add_body(world, "right", engine::physics::BodyType::Dynamic,
                       Vec3{ 2.0F, 6.0F, 0.0F }, Vec3{ 0.5F, 0.5F, 0.5F }, false);

        engine::physics::Simulation simulation;
        simulation.build(world);

        std::size_t most_in_one_step = 0;
        for (std::uint32_t i = 0; i < 240; ++i) {
            const std::size_t before = host.call_count(sp::Callback::Trigger);
            run_steps(host, world, simulation, 1);
            most_in_one_step =
                std::max(most_in_one_step, host.call_count(sp::Callback::Trigger) - before);
        }

        check(host.call_count(sp::Callback::Trigger) == 4,
              "two crates entering and leaving make four calls");
        check(most_in_one_step == 2, "and one step handed over both of them at once");
        check(host.stopped_count() == 0, "with no script stopped");
    }

    void test_an_event_for_an_unscripted_entity_is_dropped() {
        section("An event that names no script costs nothing and reports nothing");

        // The normal case. Most things that touch carry no script, and a crate
        // landing on a floor is two entities and usually no callback at all.
        sc::ComponentRegistry components = physics_registry();
        sp::Host host{ components };

        sc::World world;
        (void)add_body(world, "floor", engine::physics::BodyType::Static,
                       Vec3{ 0.0F, -1.0F, 0.0F }, Vec3{ 50.0F, 1.0F, 50.0F }, false);
        (void)add_body(world, "crate", engine::physics::BodyType::Dynamic,
                       Vec3{ 0.0F, 3.0F, 0.0F }, Vec3{ 0.5F, 0.5F, 0.5F }, false);

        engine::physics::Simulation simulation;
        simulation.build(world);
        run_steps(host, world, simulation, 120);

        check(host.call_count(sp::Callback::Contact) == 0, "nothing was called");
        check(host.instance_count() == 0, "because nothing carried a script");
        check(host.stopped_count() == 0, "and the delivery raised nothing");
    }

    void test_a_callback_reaches_the_services_of_its_step() {
        section("A physics callback can push a body through the services");

        // The delivery takes its own services, the same way update() does. A
        // callback that could read a component but not push a body would be a
        // half-bound call, and a puzzle that answers an overlap by moving
        // something is the first thing anybody writes.
        sc::ComponentRegistry components = physics_registry();
        sp::Host host{ components };

        check(load(host, kFirst, R"(
            function on_trigger(other, began)
              if not began then return end
              if not other:set_velocity(vec3(0, 20, 0)) then error("the push was refused") end
            end
        )"),
              "the goal script compiles");

        sc::World world;
        const entt::entity goal = add_body(world, "goal", engine::physics::BodyType::Static,
                                           Vec3{ 0.0F, 2.0F, 0.0F }, Vec3{ 2.0F, 0.5F, 2.0F },
                                           true);
        const entt::entity crate = add_body(world, "crate", engine::physics::BodyType::Dynamic,
                                            Vec3{ 0.0F, 6.0F, 0.0F }, Vec3{ 0.5F, 0.5F, 0.5F },
                                            false);
        world.registry().emplace<sp::ScriptComponent>(goal, sp::ScriptComponent{ kFirst });

        engine::physics::Simulation simulation;
        simulation.build(world);

        // Stop on the step the push happens, so the velocity is read before
        // gravity has taken it back.
        for (std::uint32_t i = 0; i < 240; ++i) {
            run_steps(host, world, simulation, 1);
            if (host.call_count(sp::Callback::Trigger) > 0) {
                break;
            }
        }

        check(host.call_count(sp::Callback::Trigger) == 1, "the crate entered the volume");
        check(host.stopped_count() == 0, "and the script pushed it without error");

        Vec3 velocity{ 0.0F, 0.0F, 0.0F };
        check(simulation.linear_velocity(crate, velocity), "the crate velocity reads back");
        check(velocity.y > 0.0F, "and the callback sent a falling crate upward");
    }

    /// An entity carrying a script and a Turret to write the answers into.
    [[nodiscard]] entt::entity with_script_and_turret(Fixture& fixture, engine::Guid guid) {
        const entt::entity entity = fixture.world.create();
        fixture.world.registry().emplace<Turret>(entity, Turret{});
        fixture.world.registry().emplace<sp::ScriptComponent>(entity,
                                                              sp::ScriptComponent{ guid });
        return entity;
    }

    void test_a_script_shows_and_hides_a_layout() {
        section("A script shows a layout, hides it, and asks which is open");

        FakeUi ui;
        ui.add("ui/pause.mothui", { "resume" });

        Fixture fixture;
        sp::Host host{ fixture.components };
        check(load(host, kFirst, R"(
            local shown = false
            function on_update()
                if not shown then
                    shown = true
                    ui.show("ui/pause.mothui")
                    entity:set("Turret", { armed = ui.visible("ui/pause.mothui") })
                else
                    ui.hide("ui/pause.mothui")
                end
            end
        )"),
              "the script compiles");
        const entt::entity entity = with_script_and_turret(fixture, kFirst);

        sp::Services services;
        services.ui = &ui;

        host.update(fixture.world, 0.0, services);
        check(ui.visible("ui/pause.mothui"), "the layout is showing");
        check(fixture.world.registry().get<Turret>(entity).armed,
              "and the script can ask whether it is");

        host.update(fixture.world, 1.0 / 60.0, services);
        check(!ui.visible("ui/pause.mothui"), "hiding it takes it away");
        check(host.stopped_count() == 0, "and no call raised an error");
    }

    void test_a_layout_or_node_that_is_not_there_answers_rather_than_fails() {
        section("A layout or a node that is not there answers rather than fails");

        FakeUi ui;
        ui.add("ui/pause.mothui", { "resume" });

        Fixture fixture;
        sp::Host host{ fixture.components };
        check(load(host, kFirst, R"(
            function on_update()
                local answers = ""
                answers = answers .. tostring(ui.show("ui/pause.mothui"))
                answers = answers .. "," .. tostring(ui.show("ui/none.mothui"))
                answers = answers .. "," .. tostring(ui.find("ui/pause.mothui", "resume") ~= nil)
                answers = answers .. "," .. tostring(ui.find("ui/pause.mothui", "resme") ~= nil)
                entity:set("Turret", { label = answers })
            end
        )"),
              "the script compiles");
        const entt::entity entity = with_script_and_turret(fixture, kFirst);

        sp::Services services;
        services.ui = &ui;
        host.update(fixture.world, 0.0, services);

        check(host.stopped_count() == 0, "no call raised an error");
        check(fixture.world.registry().get<Turret>(entity).label == "true,false,true,false",
              "a real layout and node answer true, and a missing one answers false");
    }

    void test_a_handle_reads_and_writes_what_a_node_shows() {
        section("A script finds a node by name and changes what it shows");

        FakeUi ui;
        ui.add("ui/hud.mothui", { "score", "portrait" });
        check(ui.set_text("ui/hud.mothui", "score", "0"), "the fake starts at zero");

        Fixture fixture;
        sp::Host host{ fixture.components };
        check(load(host, kFirst, R"(
            function on_update()
                local score = ui.find("ui/hud.mothui", "score")
                entity:set("Turret", { label = score:text() .. "|" .. score.layout .. "|" .. score.node })
                score:set_text("1200")
                score:set_visible(false)
                ui.find("ui/hud.mothui", "portrait"):set_image("ui/hero.png")
            end
        )"),
              "the script compiles");
        const entt::entity entity = with_script_and_turret(fixture, kFirst);

        sp::Services services;
        services.ui = &ui;
        host.update(fixture.world, 0.0, services);

        check(host.stopped_count() == 0, "no call raised an error");
        check(fixture.world.registry().get<Turret>(entity).label == "0|ui/hud.mothui|score",
              "the handle reads the text and names what it stands for");

        const FakeUi::Node* score = ui.node_of("ui/hud.mothui", "score");
        check(score != nullptr && score->text == "1200", "and it writes the new text");
        check(score != nullptr && !score->visible, "and hides the node");

        const FakeUi::Node* portrait = ui.node_of("ui/hud.mothui", "portrait");
        check(portrait != nullptr && portrait->image == "ui/hero.png",
              "and another handle sets an image");
    }

    void test_a_handle_survives_a_step_and_resolves_again() {
        section("A handle kept across steps names the node rather than holding it");

        // The reload trap, in the shape a script can reach. A handle holds two
        // strings and looks the node up again on every call, so a surface that
        // rebuilt everything between two steps leaves the handle correct.
        FakeUi first;
        first.add("ui/hud.mothui", { "score" });
        FakeUi second;
        second.add("ui/hud.mothui", { "score" });

        Fixture fixture;
        sp::Host host{ fixture.components };
        check(load(host, kFirst, R"(
            local score = nil
            function on_update()
                score = score or ui.find("ui/hud.mothui", "score")
                score:set_text("kept")
            end
        )"),
              "the script compiles");
        (void)with_script_and_turret(fixture, kFirst);

        host.update(fixture.world, 0.0, sp::Services{ .ui = &first });
        const FakeUi::Node* wrote_first = first.node_of("ui/hud.mothui", "score");
        check(wrote_first != nullptr && wrote_first->text == "kept", "the first step wrote");

        // A different surface entirely, which is what a reload amounts to.
        host.update(fixture.world, 1.0 / 60.0, sp::Services{ .ui = &second });
        const FakeUi::Node* wrote_second = second.node_of("ui/hud.mothui", "score");
        check(wrote_second != nullptr && wrote_second->text == "kept",
              "and the handle wrote into the surface of the second step");
        check(host.stopped_count() == 0, "with no error on either");
    }

    void test_a_press_reaches_the_script() {
        section("A press on a node calls into the script");

        FakeUi ui;
        ui.add("ui/pause.mothui", { "resume", "quit" });

        Fixture fixture;
        sp::Host host{ fixture.components };
        check(load(host, kFirst, R"(
            local count = 0
            function on_update() end
            function on_ui_press(layout, node)
                count = count + 1
                entity:set("Turret", { label = layout .. "/" .. node, range = count })
            end
        )"),
              "the script compiles");
        const entt::entity entity = with_script_and_turret(fixture, kFirst);

        sp::Services services;
        services.ui = &ui;
        host.update(fixture.world, 0.0, services);

        ui.press("ui/pause.mothui", "resume");
        host.deliver_ui_events(fixture.world, services);

        check(fixture.world.registry().get<Turret>(entity).label == "ui/pause.mothui/resume",
              "the callback names the layout and the node");
        check(fixture.world.registry().get<Turret>(entity).range == 1.0F, "and it ran once");

        // The surface gathers presses on the frame clock, so one left behind
        // would be delivered again on every later step.
        host.deliver_ui_events(fixture.world, services);
        check(fixture.world.registry().get<Turret>(entity).range == 1.0F,
              "and a press is delivered once");
        check(ui.presses().empty(), "because the surface is drained");
    }

    void test_one_step_delivers_every_press() {
        section("One step delivers every press the frames gathered");

        // A frame often runs no step at all, so several presses can land
        // between two steps. Reporting only the last one is the shape of bug
        // issue #263 describes for physics events.
        FakeUi ui;
        ui.add("ui/pause.mothui", { "resume", "quit" });

        Fixture fixture;
        sp::Host host{ fixture.components };
        check(load(host, kFirst, R"(
            local seen = ""
            function on_update() end
            function on_ui_press(layout, node)
                seen = seen == "" and node or (seen .. "," .. node)
                entity:set("Turret", { label = seen })
            end
        )"),
              "the script compiles");
        const entt::entity entity = with_script_and_turret(fixture, kFirst);

        sp::Services services;
        services.ui = &ui;
        host.update(fixture.world, 0.0, services);

        ui.press("ui/pause.mothui", "resume");
        ui.press("ui/pause.mothui", "quit");
        host.deliver_ui_events(fixture.world, services);

        check(fixture.world.registry().get<Turret>(entity).label == "resume,quit",
              "both presses arrive, in the order the frames reported them");
    }

    void test_a_press_reaches_every_listener() {
        section("A press reaches every instance that declares the callback");

        FakeUi ui;
        ui.add("ui/pause.mothui", { "resume" });

        Fixture fixture;
        sp::Host host{ fixture.components };
        check(load(host, kFirst, R"(
            function on_update() end
            function on_ui_press(layout, node)
                entity:set("Turret", { armed = true })
            end
        )"),
              "the listening script compiles");
        check(load(host, kSecond, "function on_update() end"),
              "and so does one that does not listen");

        const entt::entity first = with_script_and_turret(fixture, kFirst);
        const entt::entity second = with_script_and_turret(fixture, kFirst);
        const entt::entity quiet = with_script_and_turret(fixture, kSecond);

        sp::Services services;
        services.ui = &ui;
        host.update(fixture.world, 0.0, services);

        ui.press("ui/pause.mothui", "resume");
        host.deliver_ui_events(fixture.world, services);

        check(host.call_count(sp::Callback::Press) == 2, "both listeners were called");
        check(fixture.world.registry().get<Turret>(first).armed, "the first one ran");
        check(fixture.world.registry().get<Turret>(second).armed, "and so did the second");
        check(!fixture.world.registry().get<Turret>(quiet).armed,
              "and the script that declared none was left alone");
    }

    void test_a_press_with_no_listener_is_dropped() {
        section("A press nobody listens for is dropped rather than kept");

        FakeUi ui;
        ui.add("ui/pause.mothui", { "resume" });

        Fixture fixture;
        sp::Host host{ fixture.components };
        check(load(host, kFirst, "function on_update() end"), "the script compiles");
        (void)with_script_and_turret(fixture, kFirst);

        sp::Services services;
        services.ui = &ui;
        host.update(fixture.world, 0.0, services);

        ui.press("ui/pause.mothui", "resume");
        host.deliver_ui_events(fixture.world, services);

        check(host.call_count(sp::Callback::Press) == 0, "nothing was called");
        check(ui.presses().empty(), "and the press was still drained");
    }

    void test_a_script_pauses_and_resumes_the_game() {
        section("A script pauses and resumes the game");

        FakeClock clock;

        Fixture fixture;
        sp::Host host{ fixture.components };
        check(load(host, kFirst, R"(
            function on_update()
                if game.paused() then
                    game.resume()
                else
                    game.pause()
                end
            end
        )"),
              "the script compiles");
        (void)with_script_and_turret(fixture, kFirst);

        sp::Services services;
        services.clock = &clock;

        host.update(fixture.world, 0.0, services);
        check(clock.paused(), "the first step paused the game");

        host.update(fixture.world, 0.0, services);
        check(!clock.paused(), "and the next one resumed it");
        check(clock.writes == 2, "and each call reached the clock");
    }

    void test_a_script_asks_the_game_to_quit() {
        section("A script asks the game to quit");

        FakeExit exit;

        Fixture fixture;
        sp::Host host{ fixture.components };
        check(load(host, kFirst, R"(
            function on_update()
                entity:set("Turret", { armed = game.quit() })
            end
        )"),
              "the script compiles");
        const entt::entity entity = with_script_and_turret(fixture, kFirst);

        sp::Services services;
        services.exit = &exit;
        host.update(fixture.world, 0.0, services);

        check(exit.asked == 1, "the request reached the interface");
        check(exit.quit_requested(), "and it reads as asked for");
        check(fixture.world.registry().get<Turret>(entity).armed,
              "and the call answered true");
    }

    void test_a_script_with_no_clock_answers_false() {
        section("A script with no clock answers false rather than failing");

        Fixture fixture;
        sp::Host host{ fixture.components };
        check(load(host, kFirst, R"(
            function on_update()
                entity:set("Turret", { armed = game.pause() or game.resume()
                                               or game.paused() or game.quit() })
            end
        )"),
              "the script compiles");
        const entt::entity entity = with_script_and_turret(fixture, kFirst);

        // No clock in the services, which is what a build that binds none gives
        // a script. Every call answers false and none of them raises an error.
        sp::Services services;
        host.update(fixture.world, 0.0, services);

        check(!fixture.world.registry().get<Turret>(entity).armed,
              "every call in the game table answered false, quit included");
        check(host.stopped_count() == 0, "and none of them raised an error");
    }

    void test_a_reload_reaches_the_script_that_wrote_the_layout() {
        section("A layout reload reaches every script that declares the callback");

        FakeUi ui;
        ui.add("ui/hud.mothui", { "score" });

        Fixture fixture;
        sp::Host host{ fixture.components };
        check(load(host, kFirst, R"(
            function on_update() end
            function on_ui_reload(layout)
                ui.find(layout, "score"):set_text("written again")
            end
        )"),
              "the listening script compiles");
        check(load(host, kSecond, "function on_update() end"),
              "and so does one that does not listen");

        (void)with_script_and_turret(fixture, kFirst);
        (void)with_script_and_turret(fixture, kSecond);

        sp::Services services;
        services.ui = &ui;
        host.update(fixture.world, 0.0, services);

        // What a rebuild does: the node goes back to what the file carries.
        const FakeUi::Node* score = ui.node_of("ui/hud.mothui", "score");
        check(score != nullptr && score->text.empty(), "the node starts with no text");

        ui.reloaded("ui/hud.mothui");
        host.deliver_ui_events(fixture.world, services);

        check(host.call_count(sp::Callback::Reload) == 1, "the one listener was called");
        score = ui.node_of("ui/hud.mothui", "score");
        check(score != nullptr && score->text == "written again",
              "and it wrote the layout again");
        check(ui.reloads().empty(), "and the delivery drained it");
    }

    void test_a_reload_is_delivered_before_a_press() {
        section("A reload is delivered before a press of the same batch");

        FakeUi ui;
        ui.add("ui/hud.mothui", { "score" });

        Fixture fixture;
        sp::Host host{ fixture.components };

        // The script records the order it was called in. A press that ran first
        // would act on a menu still reading whatever its file says, which is the
        // whole reason the order is fixed rather than incidental.
        check(load(host, kFirst, R"(
            order = ""
            function on_update() end
            function on_ui_reload(layout)
                order = order .. "r"
            end
            function on_ui_press(layout, node)
                order = order .. "p"
                entity:set("Turret", { label = order })
            end
        )"),
              "the script compiles");
        const entt::entity entity = with_script_and_turret(fixture, kFirst);

        sp::Services services;
        services.ui = &ui;
        host.update(fixture.world, 0.0, services);

        // The press is recorded first, so a delivery that walked them in the
        // order they arrived would give the other answer.
        ui.press("ui/hud.mothui", "score");
        ui.reloaded("ui/hud.mothui");
        host.deliver_ui_events(fixture.world, services);

        check(fixture.world.registry().get<Turret>(entity).label == "rp",
              "the reload ran before the press");
    }

    void test_a_script_with_no_reload_callback_is_left_alone() {
        section("A reload nobody listens for is dropped rather than kept");

        FakeUi ui;
        ui.add("ui/hud.mothui", { "score" });

        Fixture fixture;
        sp::Host host{ fixture.components };
        check(load(host, kFirst, "function on_update() end"), "the script compiles");
        (void)with_script_and_turret(fixture, kFirst);

        sp::Services services;
        services.ui = &ui;
        host.update(fixture.world, 0.0, services);

        ui.reloaded("ui/hud.mothui");
        host.deliver_ui_events(fixture.world, services);

        check(host.call_count(sp::Callback::Reload) == 0, "nothing was called");
        check(ui.reloads().empty(), "and the reload was drained rather than kept");
        check(host.stopped_count() == 0, "and no instance was stopped");
    }

    void test_a_script_with_no_ui_service_answers_false() {
        section("A build with no game UI answers every call rather than failing");

        Fixture fixture;
        sp::Host host{ fixture.components };
        check(load(host, kFirst, R"(
            function on_update()
                local answers = tostring(ui.show("ui/pause.mothui"))
                answers = answers .. "," .. tostring(ui.visible("ui/pause.mothui"))
                answers = answers .. "," .. tostring(ui.find("ui/pause.mothui", "resume") ~= nil)
                entity:set("Turret", { label = answers })
            end
        )"),
              "the script compiles");
        const entt::entity entity = with_script_and_turret(fixture, kFirst);

        // No surface at all, which is what `with_ui=False` gives. Every call has
        // to answer, the same way an action reads false when nobody bound an
        // input module.
        host.update(fixture.world, 0.0, sp::Services{});

        check(host.stopped_count() == 0, "no call raised an error");
        check(fixture.world.registry().get<Turret>(entity).label == "false,false,false",
              "and every one answers false");
    }

} // namespace

int main() {
    // The physics test needs the scheduler, because Box3D runs its solver on it.
    engine::jobs::init();

    test_the_lifecycle_runs_in_order();
    test_the_step_count_is_what_drives_it();
    test_seconds_reach_the_script();
    test_two_entities_keep_separate_state();
    test_an_error_stops_that_instance_alone();
    test_a_script_that_will_not_compile_is_refused();
    test_a_dead_entity_drops_its_instance();
    test_a_reload_restarts_the_instance();
    test_a_reload_throws_the_script_table_away();
    test_component_state_survives_a_reload();
    test_a_reload_that_will_not_compile_changes_nothing();
    test_a_reload_reaches_every_entity_sharing_the_script();
    test_a_reload_revives_a_stopped_instance();
    test_a_plain_load_does_not_restart_anything();
    test_stop_runs_destroy_on_everything();
    test_a_script_needs_no_callbacks();
    test_a_script_reads_a_component();
    test_a_script_writes_a_component();
    test_a_game_type_needs_no_engine_code();
    test_a_transform_written_from_lua_marks_the_entity_dirty();
    test_the_binding_refuses_what_it_cannot_do();
    test_a_component_the_entity_lacks_reads_nil();
    test_each_instance_sees_its_own_entity();
    test_the_handle_follows_the_world_of_the_step();
    test_on_destroy_can_still_read_its_entity();
    test_vec3_has_the_operators_an_author_expects();
    test_normalizing_a_zero_vector_gives_no_nan();
    test_an_axis_of_no_direction_gives_identity();
    test_quat_turns_a_vector();
    test_a_component_vector_is_the_engine_type();
    test_a_partial_table_still_writes();
    test_the_random_source_is_reproducible();
    test_reseeding_from_the_clock_is_not_available();
    test_the_world_finds_and_walks();
    test_a_script_creates_and_destroys();
    test_input_reaches_a_script_by_action_name();
    test_the_services_follow_the_step();
    test_a_script_instances_a_prefab();
    test_the_physics_verbs_reach_a_script();
    test_a_transform_written_to_a_dynamic_body_does_not_freeze_it();
    test_a_transform_written_to_a_kinematic_body_still_interpolates();
    test_stop_does_not_reach_the_services_of_the_last_step();
    test_a_trigger_reaches_the_volume_and_not_the_visitor();
    test_a_contact_reaches_both_bodies();
    test_one_step_reports_every_event();
    test_an_event_for_an_unscripted_entity_is_dropped();
    test_a_callback_reaches_the_services_of_its_step();

    test_a_script_shows_and_hides_a_layout();
    test_a_layout_or_node_that_is_not_there_answers_rather_than_fails();
    test_a_handle_reads_and_writes_what_a_node_shows();
    test_a_handle_survives_a_step_and_resolves_again();
    test_a_press_reaches_the_script();
    test_one_step_delivers_every_press();
    test_a_press_reaches_every_listener();
    test_a_press_with_no_listener_is_dropped();
    test_a_script_with_no_ui_service_answers_false();
    test_a_reload_reaches_the_script_that_wrote_the_layout();
    test_a_reload_is_delivered_before_a_press();
    test_a_script_with_no_reload_callback_is_left_alone();

    test_a_script_pauses_and_resumes_the_game();
    test_a_script_asks_the_game_to_quit();
    test_a_script_with_no_clock_answers_false();

    // The pool has to stop before main returns. test_physics.cpp does the same,
    // and leaving the workers running at exit can hang the process or read as a
    // leak under a sanitizer.
    engine::jobs::shutdown();

    if (test::g_failures != 0) {
        std::printf("\n%d check(s) failed.\n", test::g_failures);
        return 1;
    }
    std::printf("\nAll checks passed.\n");
    return 0;
}
