// M9.4 tests for play-in-editor.
//
// These drive the shipped sandbox scene, because the restore has to survive
// what a real game does to a world: bodies that fall, scripts that create
// entities, and scripts that destroy them. A world built here would move in
// whatever way the test asked for and prove nothing about the shipped one.
//
// Nothing here opens a device. PlayMode names no Vulkan type and no ImGui type,
// so a session runs with no window at all.

#include "assets/content.h"
#include "check.h"
#include "core/jobs.h"
#include "editor/play_mode.h"
#include "physics/components.h"
#include "sandbox/game.h"
#include "scene/component_registry.h"
#include "scene/prefab.h"
#include "scene/scene_file.h"
#include "scene/world.h"

#if defined(ENGINE_WITH_LUA)
#include "script/components.h"
#endif

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>

namespace {

    using test::check;
    using test::section;

    /// How long one step is, which is the rate the sandbox runs at.
    constexpr float kStepSeconds = 1.0F / 60.0F;

    /**
     * The process-wide registry, in the order an application builds it.
     *
     * PlayMode snapshots through `scene::save_scene`, which reads the
     * process-wide registry and the process-wide prefab library by default.
     * That is what both applications hand it, so the test uses the same ones
     * rather than a registry of its own.
     */
    void register_everything() {
        engine::scene::register_builtin_components();
        engine::physics::register_components();
#if defined(ENGINE_WITH_LUA)
        engine::script::register_components();
#endif
        sandbox::register_components();
    }

    /// Opens the cooked tree and reads the shipped scene into @p world.
    [[nodiscard]] bool load_shipped(engine::scene::World& world,
                                    engine::assets::Content& content) {
        if (!content.open(sandbox::default_content_directory())) {
            return false;
        }
        return sandbox::load(sandbox::default_content_directory(), &content, world);
    }

    /// Runs @p steps whole steps through the session.
    void run(engine::editor::PlayMode& play, engine::scene::World& world, std::uint32_t steps) {
        for (std::uint32_t i = 0; i < steps; ++i) {
            play.advance(world, engine::play::View{}, kStepSeconds);
        }
    }

    /**
     * A stopped session leaves the world exactly as it was authored.
     *
     * This is the whole of M9.4. The session is given enough steps to matter:
     * the crates fall, the movers turn, and the throw makes an entity the
     * authored scene never had. Then stop puts the document back and the world
     * has to write out the same bytes it did before the first step.
     *
     * The check that the world changed at all is not decoration. Without it a
     * session that did nothing would pass this test, and a session that does
     * nothing is exactly what a broken one looks like.
     */
    void test_stop_restores_the_authored_world() {
        section("a stopped session restores the world");

        engine::assets::Content content;
        engine::scene::World world;
        check(load_shipped(world, content), "the shipped content loads");

        const nlohmann::json authored = engine::scene::save_scene(world);
        const std::size_t authored_entities = world.size();

        engine::editor::PlayMode play;
        const engine::editor::PlayDesc desc{ .content = &content,
                                             .bind_actions = &sandbox::bind_actions };
        check(play.play(world, desc), "the session starts");
        check(play.state() == engine::editor::PlayState::Playing, "the state is Playing");
        check(play.snapshot() == authored, "the snapshot is the authored world");

        // Enough for the stack to settle and for the spinners to turn well past
        // where they started.
        run(play, world, 120);

        // One frame with the throw key down, then more steps for the crate it
        // makes to fly and land. The key reaches the game through the same fold
        // the runtime uses, so this drives the binding rather than going round
        // it.
        engine::platform::InputFrame pressed;
        pressed.keys.at(static_cast<std::size_t>(sandbox::kThrowKey)) = true;
        play.feed_input(pressed);
        run(play, world, 1);
        play.feed_input(engine::platform::InputFrame{});
        run(play, world, 120);

        const nlohmann::json played = engine::scene::save_scene(world);
        check(played != authored, "the session moved the world");
#if defined(ENGINE_WITH_LUA)
        check(world.size() > authored_entities, "the throw added an entity");
#endif

        play.stop(world);
        check(play.state() == engine::editor::PlayState::Edit, "the state is Edit again");
        check(play.session() == nullptr, "the session is gone");
        check(world.size() == authored_entities, "the entity count is the authored one");
        check(engine::scene::save_scene(world) == authored, "the world writes out as authored");
    }

    /**
     * A second session starts from the authored world, not from the first one.
     *
     * A session that carried anything over would show up here rather than in
     * the test above, because the second play snapshots whatever the first one
     * left behind.
     */
    void test_a_second_session_starts_from_the_same_place() {
        section("a second session starts where the first one did");

        engine::assets::Content content;
        engine::scene::World world;
        check(load_shipped(world, content), "the shipped content loads");

        const nlohmann::json authored = engine::scene::save_scene(world);
        const engine::editor::PlayDesc desc{ .content = &content,
                                             .bind_actions = &sandbox::bind_actions };

        engine::editor::PlayMode play;
        check(play.play(world, desc), "the first session starts");
        run(play, world, 60);
        play.stop(world);

        check(play.play(world, desc), "the second session starts");
        check(play.snapshot() == authored, "the second snapshot is the authored world");
        run(play, world, 60);
        play.stop(world);

        check(engine::scene::save_scene(world) == authored, "the world is authored again");
    }

    /**
     * Pause holds the steps and keeps the session.
     *
     * A pause that dropped the session would read as working, because stop
     * would still restore the world. So the check is that nothing moves while
     * it is held and that the session is still there to resume.
     */
    void test_pause_holds_the_step() {
        section("pause holds the fixed step");

        engine::assets::Content content;
        engine::scene::World world;
        check(load_shipped(world, content), "the shipped content loads");

        engine::editor::PlayMode play;
        const engine::editor::PlayDesc desc{ .content = &content,
                                             .bind_actions = &sandbox::bind_actions };
        check(play.play(world, desc), "the session starts");
        run(play, world, 60);

        play.pause();
        check(play.state() == engine::editor::PlayState::Paused, "the state is Paused");
        check(play.session() != nullptr, "the session is still there");

        const nlohmann::json held = engine::scene::save_scene(world);
        const std::uint64_t steps = play.session()->clock().steps_taken();
        run(play, world, 60);
        check(engine::scene::save_scene(world) == held, "nothing moved while it was paused");
        check(play.session()->clock().steps_taken() == steps, "no step ran while it was paused");

        play.resume();
        check(play.state() == engine::editor::PlayState::Playing, "the state is Playing again");
        run(play, world, 60);
        check(play.session()->clock().steps_taken() > steps, "the steps run again after resume");

        play.stop(world);
    }

    /// Play does nothing while a session runs, and stop does nothing in Edit.
    void test_the_states_refuse_what_they_cannot_do() {
        section("a request that does not apply changes nothing");

        engine::assets::Content content;
        engine::scene::World world;
        check(load_shipped(world, content), "the shipped content loads");

        engine::editor::PlayMode play;
        const engine::editor::PlayDesc desc{ .content = &content,
                                             .bind_actions = &sandbox::bind_actions };

        // A stop with no session must not clear the world. Nothing has been
        // snapshotted, so a restore here would leave an empty one.
        const std::size_t authored_entities = world.size();
        play.stop(world);
        check(world.size() == authored_entities, "a stop in Edit state leaves the world alone");

        check(play.play(world, desc), "the session starts");
        check(!play.play(world, desc), "a second play is refused while one runs");

        // A resume that never paused, and a pause that already paused.
        play.resume();
        check(play.state() == engine::editor::PlayState::Playing, "resume while playing is a nop");
        play.pause();
        play.pause();
        check(play.state() == engine::editor::PlayState::Paused, "pause is not a toggle");

        play.stop(world);
    }

} // namespace

int main() {
    register_everything();

    // The physics world runs its solver on the scheduler, so a session needs
    // the pool up before it builds one.
    engine::jobs::init();

    test_stop_restores_the_authored_world();
    test_a_second_session_starts_from_the_same_place();
    test_pause_holds_the_step();
    test_the_states_refuse_what_they_cannot_do();

    engine::jobs::shutdown();
    return test::report();
}
