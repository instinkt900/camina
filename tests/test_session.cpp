// M10.7a tests for engine::play::Session, and for what a pause does to it.
//
// tests/test_script.cpp drives the `game` table against a fake clock, so it
// proves the binding and nothing under it. This drives the real session, so
// between them the whole path from a Lua call to a step that does not run is
// covered.
//
// Nothing here opens a device. A session names no window and no Vulkan type,
// and the UI surface is faked for the reason tests/test_script.cpp gives: what
// matters is which presses reach a script, not what moth_ui draws.

#include "check.h"
#include "core/guid.h"
#include "core/jobs.h"
#include "math/conventions.h"
#include "physics/components.h"
#include "platform/input.h"
#include "play/session.h"
#include "scene/components.h"
#include "scene/world.h"
#include "script/components.h"
#include "script/ui_surface.h"

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

    using test::check;
    using test::section;
    namespace sc = engine::scene;
    namespace sp = engine::script;

    /// How long one step is, which is the rate a session runs at by default.
    constexpr float kStepSeconds = 1.0F / 60.0F;

    /// One identity that is not the null GUID. The value means nothing.
    constexpr engine::Guid kScript{ 7, 7 };

    [[nodiscard]] std::span<const std::byte> bytes_of(std::string_view text) {
        return { reinterpret_cast<const std::byte*>(text.data()), // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
                 text.size() };
    }

    /**
     * A UI surface that reports whatever presses a test hands it.
     *
     * Only the press half is driven here. A session decides when a press is
     * delivered, and this is what lets the test see the delivery with no
     * moth_ui and no window under it.
     */
    class FakeUi final : public sp::UiSurface {
    public:
        /// Records a press, the way a frame does between two steps.
        void press(std::string layout, std::string node) {
            presses_.push_back(sp::UiPress{ std::move(layout), std::move(node) });
        }

        [[nodiscard]] std::span<const sp::UiPress> presses() const override {
            return presses_;
        }
        void clear_presses() override { presses_.clear(); }

        /// Records a rebuild, the way a hot reload does.
        void reloaded(std::string layout) { reloads_.push_back(std::move(layout)); }

        [[nodiscard]] std::span<const std::string> reloads() const override {
            return reloads_;
        }
        void clear_reloads() override { reloads_.clear(); }

        bool show(std::string_view layout) override {
            (void)layout;
            return true;
        }
        bool hide(std::string_view layout) override {
            (void)layout;
            return true;
        }
        [[nodiscard]] bool visible(std::string_view layout) const override {
            (void)layout;
            return true;
        }
        [[nodiscard]] bool has_node(std::string_view layout,
                                    std::string_view node) const override {
            (void)layout;
            (void)node;
            return false;
        }
        [[nodiscard]] std::string text(std::string_view layout,
                                       std::string_view node) const override {
            (void)layout;
            (void)node;
            return {};
        }
        bool set_text(std::string_view layout, std::string_view node,
                      std::string_view text) override {
            (void)layout;
            (void)node;
            (void)text;
            return false;
        }
        [[nodiscard]] bool node_visible(std::string_view layout,
                                        std::string_view node) const override {
            (void)layout;
            (void)node;
            return false;
        }
        bool set_node_visible(std::string_view layout, std::string_view node,
                              bool visible) override {
            (void)layout;
            (void)node;
            (void)visible;
            return false;
        }
        bool set_image(std::string_view layout, std::string_view node,
                       std::string_view image) override {
            (void)layout;
            (void)node;
            (void)image;
            return false;
        }

    private:
        std::vector<sp::UiPress> presses_;
        std::vector<std::string> reloads_;
    };

    /// A component the script writes, so a check reads state and not a log.
    struct Counter {
        int updates = 0; ///< How many times on_update ran.
        int presses = 0; ///< How many presses reached the script.
        int fired = 0;   ///< How many press edges the fire action raised.
        int reloads = 0; ///< How many layout reloads reached the script.
        int held = 0;    ///< How many press edges on_paused_update saw.
        int paused = 0;  ///< How many times on_paused_update ran.
    };

} // namespace

/// @brief Describes the test component, the way a game describes its own.
///
/// It sits outside the engine exactly as `sandbox::Spin` does. Nothing in
/// `src/` names Counter, and the binding still carries it.
template <>
struct engine::reflect::Describe<Counter> {
    static constexpr const char* name = "Counter"; ///< The name the script uses.
    /// @brief The six counts.
    /// @return A tuple of field descriptors.
    static constexpr auto fields() {
        return std::make_tuple(ENGINE_FIELD(Counter, updates),
                               ENGINE_FIELD(Counter, presses),
                               ENGINE_FIELD(Counter, fired),
                               ENGINE_FIELD(Counter, reloads), ENGINE_FIELD(Counter, held),
                               ENGINE_FIELD(Counter, paused));
    }
};

namespace {

    /// The registry a session reads components through.
    void register_everything() {
        sc::register_builtin_components();
        engine::physics::register_components();
        sp::register_components();
        sc::components().add<Counter>();
    }

    /**
     * A world holding one scripted entity, and the session that runs it.
     *
     * The script counts its own calls into a component, because a component
     * survives what a script table does not and the check then reads the world
     * rather than the interpreter.
     */
    struct Fixture {
        sc::World world;
        engine::play::Session session;
        FakeUi ui;
        entt::entity entity = entt::null;

        void open() {
            const std::string_view source = R"(
                function on_update(seconds)
                    local counter = entity:get("Counter")
                    local fired = counter.fired
                    if input.pressed("fire") then
                        fired = fired + 1
                    end
                    entity:set("Counter", { updates = counter.updates + 1,
                                            presses = counter.presses,
                                            fired = fired,
                                            reloads = counter.reloads,
                                            held = counter.held,
                                            paused = counter.paused })
                end

                function on_ui_press(layout, node)
                    local counter = entity:get("Counter")
                    entity:set("Counter", { updates = counter.updates,
                                            presses = counter.presses + 1,
                                            fired = counter.fired,
                                            reloads = counter.reloads,
                                            held = counter.held,
                                            paused = counter.paused })
                end

                function on_ui_reload(layout)
                    local counter = entity:get("Counter")
                    entity:set("Counter", { updates = counter.updates,
                                            presses = counter.presses,
                                            fired = counter.fired,
                                            reloads = counter.reloads + 1,
                                            held = counter.held,
                                            paused = counter.paused })
                end

                function on_paused_update()
                    local counter = entity:get("Counter")
                    local seen = counter.held
                    if input.pressed("fire") then
                        seen = seen + 1
                    end
                    entity:set("Counter", { updates = counter.updates,
                                            presses = counter.presses,
                                            fired = counter.fired,
                                            reloads = counter.reloads,
                                            held = seen,
                                            paused = counter.paused + 1 })
                end
            )";
            check(session.scripts().load(kScript, "counter.lua", bytes_of(source)),
                  "the script compiles");

            // World::create gives the entity a Transform, so only the two this
            // test adds are emplaced here.
            entity = world.create();
            world.registry().emplace<Counter>(entity);
            world.registry().emplace<sp::ScriptComponent>(entity,
                                                          sp::ScriptComponent{ kScript });

            session.input().bind("fire", engine::platform::Key::F);
            session.set_ui(&ui);
            session.build(world);
        }

        /// Feeds one device frame, the way the runtime does each frame.
        void feed(bool fire_down) {
            engine::platform::InputFrame frame;
            frame.focused = true;
            frame.keys.at(static_cast<std::size_t>(engine::platform::Key::F)) = fire_down;
            session.feed_input(frame);
        }

        [[nodiscard]] const Counter& counter() const {
            return world.registry().get<Counter>(entity);
        }

        /// Runs @p frames frames of one step each.
        void run(int frames) {
            for (int i = 0; i < frames; ++i) {
                session.advance(world, engine::play::View{}, kStepSeconds);
            }
        }
    };

    void a_paused_session_runs_no_step() {
        section("a paused session runs no step");

        Fixture fixture;
        fixture.open();

        fixture.run(3);
        const int ran = fixture.counter().updates;
        check(ran > 0, "a running session updates the script");

        fixture.session.set_paused(true);
        check(fixture.session.paused(), "and it says it is paused");

        fixture.run(10);
        check(fixture.counter().updates == ran, "a paused one runs no update at all");

        fixture.session.set_paused(false);
        fixture.run(1);
        check(fixture.counter().updates > ran, "and resuming starts it again");
    }

    void a_pause_owes_the_step_after_it_nothing() {
        section("a pause owes the step after it nothing");

        Fixture fixture;
        fixture.open();
        fixture.run(1);

        const int ran = fixture.counter().updates;

        // Ten seconds of frames while paused. A clock that accumulated them
        // would pay them all back at once, and the ceiling in FixedTimestep
        // would then drop the rest and report a run that fell behind.
        fixture.session.set_paused(true);
        for (int i = 0; i < 10; ++i) {
            fixture.session.advance(fixture.world, engine::play::View{}, 1.0F);
        }
        fixture.session.set_paused(false);

        fixture.run(1);
        check(fixture.counter().updates == ran + 1,
              "the step after a long pause runs one step and not ten seconds of them");
        check(fixture.session.clock().dropped_seconds() == 0.0,
              "and the clock dropped no time, so nothing fell behind");
    }

    void a_paused_session_still_delivers_a_press() {
        section("a paused session still delivers a press");

        Fixture fixture;
        fixture.open();
        fixture.run(1);

        // The whole reason the rule exists. A press is what resumes a game, so
        // a session that delivered none while paused could never be resumed by
        // the menu it put on the screen.
        fixture.session.set_paused(true);
        const int before = fixture.counter().presses;

        fixture.ui.press("ui/pause.mothui", "resume");
        fixture.run(1);

        check(fixture.counter().presses == before + 1,
              "the press reached the script while the game was paused");
        check(fixture.ui.presses().empty(), "and the delivery drained it");
    }

    void a_paused_session_holds_the_bodies_still() {
        section("a paused session holds the bodies still");

        register_everything();

        sc::World world;
        engine::play::Session session;

        // A dynamic body with nothing under it, so gravity is the only thing
        // that can move it. A script could hold an entity still by accident.
        const entt::entity crate = world.create();
        world.registry().get<engine::Transform>(crate).position =
            engine::Vec3{ 0.0F, 10.0F, 0.0F };
        world.registry().emplace<engine::physics::RigidBody>(
            crate, engine::physics::RigidBody{ .type = engine::physics::BodyType::Dynamic });
        world.registry().emplace<engine::physics::BoxCollider>(crate);
        world.update();
        session.build(world);

        for (int i = 0; i < 10; ++i) {
            session.advance(world, engine::play::View{}, kStepSeconds);
        }
        const float fell_to = world.registry().get<engine::Transform>(crate).position.y;
        check(fell_to < 10.0F, "a running session lets the crate fall");

        session.set_paused(true);
        for (int i = 0; i < 60; ++i) {
            session.advance(world, engine::play::View{}, kStepSeconds);
        }

        check(world.registry().get<engine::Transform>(crate).position.y == fell_to,
              "and a paused one leaves it exactly where it was");
    }

    void a_pause_does_not_hand_the_game_the_keys_pressed_during_it() {
        section("a pause does not hand the game the keys pressed during it");

        Fixture fixture;
        fixture.open();
        fixture.feed(false);
        fixture.run(1);
        check(fixture.counter().fired == 0, "nothing was pressed yet");

        fixture.session.set_paused(true);

        // A key pressed and let go while the menu is up. The fold is an OR
        // across every frame since the last step, and a pause runs none, so
        // without a reset every one of these would still be down on the first
        // step after the resume.
        fixture.feed(true);
        fixture.session.advance(fixture.world, engine::play::View{}, kStepSeconds);
        fixture.feed(false);
        fixture.session.advance(fixture.world, engine::play::View{}, kStepSeconds);

        fixture.session.set_paused(false);
        fixture.feed(false);
        fixture.run(1);

        check(fixture.counter().fired == 0,
              "and the game never saw the key that was pressed while it was paused");

        // The action still works, so the check above is a reset rather than an
        // input path that stopped answering.
        fixture.feed(true);
        fixture.run(1);
        check(fixture.counter().fired == 1, "and a key pressed after the resume still reads");
    }

    void a_paused_session_still_delivers_a_reload() {
        section("a paused session still delivers a reload");

        Fixture fixture;
        fixture.open();
        fixture.run(1);

        // A menu is edited while the game is paused, which is the whole reason
        // this has to reach a script on the frame clock. A reload delivered only
        // on a step would leave every label wrong for as long as the menu is up,
        // which is exactly when a person is looking at it.
        fixture.session.set_paused(true);
        const int before = fixture.counter().reloads;

        fixture.ui.reloaded("ui/pause.mothui");
        fixture.run(1);

        check(fixture.counter().reloads == before + 1,
              "the reload reached the script while the game was paused");
        check(fixture.ui.reloads().empty(), "and the delivery drained it");
    }

    void a_paused_game_reads_the_key_that_resumes_it() {
        section("a paused game reads the key that resumes it");

        Fixture fixture;
        fixture.open();
        fixture.feed(false);
        fixture.run(1);
        check(fixture.counter().paused == 0, "a running session runs no paused update");

        fixture.session.set_paused(true);
        fixture.session.advance(fixture.world, engine::play::View{}, kStepSeconds);
        check(fixture.counter().paused == 1, "a paused one runs it once for each frame");
        check(fixture.counter().held == 0, "and nothing was pressed yet");

        // The whole point. A script reads an action inside on_update, and a
        // paused session runs none, so the key that would resume the game could
        // never be seen.
        fixture.feed(true);
        fixture.session.advance(fixture.world, engine::play::View{}, kStepSeconds);
        check(fixture.counter().held == 1, "the key reached the paused game");

        // An edge and not the key being down, the same as on a step. Holding it
        // would otherwise resume and pause again on every frame.
        fixture.session.advance(fixture.world, engine::play::View{}, kStepSeconds);
        check(fixture.counter().held == 1, "and holding it raises no second edge");

        const int ran = fixture.counter().updates;
        check(ran > 0, "the running steps before the pause updated the script");
        fixture.session.advance(fixture.world, engine::play::View{}, kStepSeconds);
        check(fixture.counter().updates == ran,
              "and on_update still never runs while the game is paused");
    }

    void a_script_edited_while_paused_restarts_there_and_then() {
        section("a script edited while paused restarts there and then");

        Fixture fixture;
        fixture.open();
        fixture.run(1);

        fixture.session.set_paused(true);
        fixture.session.advance(fixture.world, engine::play::View{}, kStepSeconds);
        const std::size_t before = fixture.session.scripts().restart_count();

        // Editing a menu while it is on the screen is exactly when this
        // matters, and the game is paused for the whole of it. A restart that
        // waited for a resume would leave the old text running under a layout
        // somebody is looking at.
        const std::string_view edited = R"(
            function on_paused_update() end
        )";
        check(fixture.session.scripts().reload(kScript, "counter.lua", bytes_of(edited)),
              "the edited script compiles");

        fixture.session.advance(fixture.world, engine::play::View{}, kStepSeconds);
        check(fixture.session.scripts().restart_count() == before + 1,
              "and the instance restarted while the game was still paused");
    }

    void a_pose_written_while_paused_is_the_pose_the_next_frame_draws() {
        section("a pose written while paused is the pose the next frame draws");

        register_everything();

        sc::World world;
        engine::play::Session session;

        const entt::entity crate = world.create();
        world.registry().get<engine::Transform>(crate).position =
            engine::Vec3{ 0.0F, 10.0F, 0.0F };
        world.registry().emplace<engine::physics::RigidBody>(
            crate, engine::physics::RigidBody{ .type = engine::physics::BodyType::Dynamic });
        world.registry().emplace<engine::physics::BoxCollider>(crate);
        world.update();
        session.build(world);

        for (int i = 0; i < 10; ++i) {
            session.advance(world, engine::play::View{}, kStepSeconds);
        }
        const float fell_to = world.registry().get<engine::Transform>(crate).position.y;
        check(fell_to < 10.0F, "the crate fell");

        session.set_paused(true);
        session.advance(world, engine::play::View{}, kStepSeconds);

        // What a Main menu button does: it puts the room back while the game is
        // held. Before this the pose stayed drawn where the crate fell, and the
        // reset was invisible until something resumed.
        const engine::Vec3 home{ 0.0F, 4.0F, 0.0F };
        check(session.simulation().teleport(world, crate, home, engine::Quat{ 1.0F, 0.0F, 0.0F, 0.0F }),
              "the crate teleports home");

        session.advance(world, engine::play::View{}, kStepSeconds);
        check(world.registry().get<engine::Transform>(crate).position.y == home.y,
              "and the next paused frame draws it there");

        // And it stays. A paused frame draws every frame, so a pose that was not
        // authoritative would be blended towards on the first one and held there.
        session.advance(world, engine::play::View{}, kStepSeconds);
        check(world.registry().get<engine::Transform>(crate).position.y == home.y,
              "and it stays there while the pause lasts");
    }

    void a_script_asks_the_session_to_quit() {
        section("a script asks the session to quit");

        Fixture fixture;
        fixture.open();
        fixture.run(1);
        check(!fixture.session.quit_requested(), "nothing has asked to quit");

        // The Quit button of a game's own main menu. The session records it and
        // stops nothing: whoever drives it decides what stopping means, so a
        // game played in an editor viewport cannot close the editor.
        const std::string_view quitter = R"(
            function on_update()
                game.quit()
            end
        )";
        check(fixture.session.scripts().reload(kScript, "counter.lua", bytes_of(quitter)),
              "the script that quits compiles");

        fixture.run(2);
        check(fixture.session.quit_requested(), "the session was asked to quit");

        // Sticky, because whoever reads it may be several frames later.
        fixture.run(1);
        check(fixture.session.quit_requested(), "and the request stands");
    }

} // namespace

int main() {
    // The solver runs on the scheduler, so it has to be up before a session
    // steps one. tests/test_physics.cpp does the same.
    engine::jobs::init();
    register_everything();

    a_paused_session_runs_no_step();
    a_pause_owes_the_step_after_it_nothing();
    a_paused_session_still_delivers_a_press();
    a_paused_session_holds_the_bodies_still();
    a_pause_does_not_hand_the_game_the_keys_pressed_during_it();
    a_paused_session_still_delivers_a_reload();
    a_paused_game_reads_the_key_that_resumes_it();
    a_script_edited_while_paused_restarts_there_and_then();
    a_pose_written_while_paused_is_the_pose_the_next_frame_draws();
    a_script_asks_the_session_to_quit();

    engine::jobs::shutdown();
    return test::report();
}
