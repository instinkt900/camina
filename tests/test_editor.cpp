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
#include "editor/edits.h"
#include "editor/history.h"
#include "editor/interaction.h"
#if defined(ENGINE_WITH_AUDIO)
#include "assets/script.h"
#include "assets/sound.h"
#include "audio/mixer.h"
#include "audio/scene_audio.h"
#include "audio/script_audio.h"
#endif
#include "editor/play_mode.h"
#include "play/session.h"
#include "editor/panels.h"
#include "editor/picking.h"
#include "editor/placement.h"
#include "physics/components.h"
#include "sandbox/game.h"
#include "math/transform.h"
#include "scene/component_registry.h"
#include "scene/components.h"
#include "scene/prefab.h"
#include "scene/scene_file.h"
#include "scene/world.h"

#if defined(ENGINE_WITH_LUA)
#include "script/components.h"
#endif

#include <nlohmann/json.hpp>

#include <array>
#include <cmath>
#include <fstream>
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

    /// How close two matrices have to be, in meters and in the units of a basis.
    constexpr float kPlaceTolerance = 1.0e-4F;

    [[nodiscard]] bool near_enough(const engine::Vec3& a, const engine::Vec3& b) {
        return std::fabs(a.x - b.x) < kPlaceTolerance && std::fabs(a.y - b.y) < kPlaceTolerance &&
               std::fabs(a.z - b.z) < kPlaceTolerance;
    }

    [[nodiscard]] bool near_enough(const engine::Mat4& a, const engine::Mat4& b) {
        for (glm::length_t column = 0; column < 4; ++column) {
            for (glm::length_t row = 0; row < 4; ++row) {
                if (std::fabs(a[column][row] - b[column][row]) > kPlaceTolerance) {
                    return false;
                }
            }
        }
        return true;
    }

    /**
     * A transform survives being composed into a matrix and read back.
     *
     * `place_entity` goes through a matrix, so an entity dragged by a gizmo
     * comes back through this. A rotation that came back conjugated, or a scale
     * that came back on the wrong axis, would move an entity every time
     * somebody touched it.
     */
    void test_a_transform_round_trips_through_a_matrix() {
        section("a transform through a matrix and back");

        const engine::Quat turn =
            glm::angleAxis(glm::radians(35.0F), glm::normalize(engine::Vec3{ 0.3F, 1.0F, 0.2F }));
        const engine::Transform original{ .position = { 1.5F, -2.0F, 3.25F },
                                          .rotation = turn,
                                          .scale = { 2.0F, 0.5F, 1.25F } };

        const engine::Transform back = engine::from_matrix(engine::to_matrix(original));
        check(near_enough(engine::to_matrix(back), engine::to_matrix(original)),
              "the matrix it composes to is the one it came from");
        check(std::fabs(back.scale.x - 2.0F) < kPlaceTolerance &&
                  std::fabs(back.scale.y - 0.5F) < kPlaceTolerance &&
                  std::fabs(back.scale.z - 1.25F) < kPlaceTolerance,
              "a non-uniform scale comes back on the right axes");
    }

    /**
     * A gizmo drag puts an entity exactly where it was dragged.
     *
     * The parent is the whole of this test. A child stores a transform relative
     * to its parent, and a gizmo works in world space, so placing a child under
     * a moved and turned parent is where the arithmetic can go wrong. Issue
     * #302 named it: the handles have to land where the pointer is.
     */
    void test_placing_an_entity_under_a_parent() {
        section("placing an entity through the hierarchy");

        engine::scene::World world;

        const entt::entity parent = world.create();
        world.set_local(parent, { .position = { 10.0F, 1.0F, -4.0F },
                                  .rotation = glm::angleAxis(glm::radians(70.0F),
                                                             engine::Vec3{ 0.0F, 1.0F, 0.0F }) });

        const entt::entity middle = world.create();
        check(world.set_parent(middle, parent), "the middle entity attaches");
        world.set_local(middle, { .position = { 0.0F, 2.0F, 0.0F } });

        const entt::entity leaf = world.create();
        check(world.set_parent(leaf, middle), "the leaf attaches");
        world.set_local(leaf, { .position = { 1.0F, 0.0F, 0.0F } });
        world.update();

        const engine::Mat4 parent_before = world.world_matrix(parent);

        // Where a drag left it: somewhere the child's own transform would never
        // have put it.
        const engine::Transform dragged{ .position = { -3.0F, 5.0F, 8.0F },
                                         .rotation = glm::angleAxis(glm::radians(-25.0F),
                                                                    engine::Vec3{ 1.0F, 0.0F,
                                                                                  0.0F }),
                                         .scale = { 1.5F, 1.5F, 1.5F } };
        engine::editor::place_entity(world, middle, engine::to_matrix(dragged));
        world.update();

        check(near_enough(world.world_matrix(middle), engine::to_matrix(dragged)),
              "the dragged entity ends up exactly where it was dragged");
        check(near_enough(world.world_matrix(parent), parent_before),
              "the parent did not move");

        // The write went through set_local, so the subtree was marked and the
        // leaf was composed again. Writing the component directly is the
        // bug this catches: everything under the entity stays behind.
        const engine::Vec3 leaf_position{ world.world_matrix(leaf)[3] };
        const engine::Vec3 expected{ engine::to_matrix(dragged) *
                                     engine::Vec4{ 1.0F, 0.0F, 0.0F, 1.0F } };
        check(std::fabs(leaf_position.x - expected.x) < kPlaceTolerance &&
                  std::fabs(leaf_position.y - expected.y) < kPlaceTolerance &&
                  std::fabs(leaf_position.z - expected.z) < kPlaceTolerance,
              "the leaf followed");
    }

    /// A root entity has no parent to divide out, and lands just the same.
    void test_placing_a_root_entity() {
        section("placing an entity with no parent");

        engine::scene::World world;
        const entt::entity entity = world.create();
        world.update();

        const engine::Transform dragged{ .position = { 4.0F, 0.5F, -2.0F } };
        engine::editor::place_entity(world, entity, engine::to_matrix(dragged));
        world.update();

        check(near_enough(world.world_matrix(entity), engine::to_matrix(dragged)),
              "a root entity lands where it was dragged");
    }

    /**
     * A dragged entity reaches the scene file and comes back the same.
     *
     * The gizmo writes a local transform, the writer collapses a prefab
     * instance to its overrides, and the reader builds it again. A drag that
     * did not survive that round trip would look right until somebody reopened
     * the scene, which is the worst moment to find out.
     */
    void test_a_dragged_entity_survives_the_scene_file() {
        section("a dragged entity through the scene file");

        engine::assets::Content content;
        engine::scene::World world;
        check(load_shipped(world, content), "the shipped content loads");

        entt::entity dragged = entt::null;
        for (const auto [entity, named] :
             world.registry().view<const engine::scene::Name>().each()) {
            if (named.value == "big crate") {
                dragged = entity;
            }
        }
        check(dragged != entt::null, "the crate to drag is there");

        // Where a gizmo would have left it. The crate is a prefab instance, so
        // this has to come back as an override rather than as a new entity.
        const engine::Transform moved{ .position = { -2.5F, 3.75F, 1.25F },
                                       .rotation = glm::angleAxis(glm::radians(30.0F),
                                                                  engine::Vec3{ 0.0F, 1.0F,
                                                                                0.0F }),
                                       .scale = { 1.6F, 1.6F, 1.6F } };
        engine::editor::place_entity(world, dragged, engine::to_matrix(moved));
        world.update();

        const nlohmann::json saved = engine::scene::save_scene(world);

        engine::scene::World reopened;
        check(engine::scene::load_scene(saved, reopened), "the saved scene loads again");
        reopened.update();
        check(engine::scene::save_scene(reopened) == saved,
              "saving what was loaded gives the same document");

        entt::entity found = entt::null;
        for (const auto [entity, named] :
             reopened.registry().view<const engine::scene::Name>().each()) {
            if (named.value == "big crate") {
                found = entity;
            }
        }
        check(found != entt::null, "the crate came back");
        check(near_enough(reopened.world_matrix(found), engine::to_matrix(moved)),
              "and it is where it was dragged to");
    }

    /// A unit cube for every mesh, which is all a picking test needs.
    [[nodiscard]] engine::editor::BoundsLookup unit_cubes() {
        return [](engine::Guid /*mesh*/, engine::Vec3& min, engine::Vec3& max) {
            min = engine::Vec3{ -0.5F, -0.5F, -0.5F };
            max = engine::Vec3{ 0.5F, 0.5F, 0.5F };
            return true;
        };
    }

    /// Adds an entity that occupies space, at a position.
    entt::entity add_box(engine::scene::World& world, const engine::Vec3& at,
                         const engine::Vec3& scale = { 1.0F, 1.0F, 1.0F }) {
        const entt::entity entity = world.create();
        world.set_local(entity, { .position = at, .scale = scale });
        world.registry().emplace<engine::scene::MeshRenderer>(entity);
        world.update();
        return entity;
    }

    /**
     * A click picks the thing under it, and the nearest of two.
     *
     * The nearest check is the one that matters. A picker that returns the first
     * hit rather than the closest looks correct until two things line up, and
     * then it selects the one behind, which reads as the click having missed.
     */
    void test_picking_the_nearest_entity() {
        section("picking the entity under a ray");

        engine::scene::World world;
        const entt::entity near_box = add_box(world, { 0.0F, 0.0F, -2.0F });
        const entt::entity far_box = add_box(world, { 0.0F, 0.0F, -8.0F });

        const engine::Ray down_z{ .origin = { 0.0F, 0.0F, 0.0F },
                                  .direction = { 0.0F, 0.0F, -1.0F } };
        check(engine::editor::pick_entity(world, down_z, unit_cubes()) == near_box,
              "the nearer of two boxes wins");

        // From behind the far one, looking the other way, the order reverses.
        const engine::Ray up_z{ .origin = { 0.0F, 0.0F, -12.0F },
                                .direction = { 0.0F, 0.0F, 1.0F } };
        check(engine::editor::pick_entity(world, up_z, unit_cubes()) == far_box,
              "and from the other side the other one does");

        const engine::Ray past{ .origin = { 9.0F, 0.0F, 0.0F },
                                .direction = { 0.0F, 0.0F, -1.0F } };
        check(engine::editor::pick_entity(world, past, unit_cubes()) == entt::null,
              "a ray through empty space picks nothing");
    }

    /**
     * A room does not swallow every click inside it.
     *
     * This is the shape of every scene the editor opens: a box that contains the
     * camera and everything in it. A ray starting inside a box used to hit at
     * zero, and nothing can be nearer than zero, so clicking anything selected
     * the room. Reported from a real session.
     */
    void test_a_room_does_not_swallow_every_click() {
        section("picking inside a room");

        engine::scene::World world;
        // The room: a box around everything, with the camera inside it.
        const entt::entity room =
            add_box(world, { 0.0F, 0.0F, 0.0F }, { 40.0F, 40.0F, 40.0F });
        const entt::entity crate = add_box(world, { 0.0F, 0.0F, -5.0F });

        const engine::Ray at_the_crate{ .origin = { 0.0F, 0.0F, 0.0F },
                                        .direction = { 0.0F, 0.0F, -1.0F } };
        check(engine::editor::pick_entity(world, at_the_crate, unit_cubes()) == crate,
              "a click on the crate selects the crate, not the room around it");

        // And the room is still selectable where nothing else is in the way.
        const engine::Ray at_the_wall{ .origin = { 0.0F, 0.0F, 0.0F },
                                       .direction = { 0.0F, 1.0F, 0.0F } };
        check(engine::editor::pick_entity(world, at_the_wall, unit_cubes()) == room,
              "a click at the bare wall still selects the room");
    }

    /**
     * The test is against the box the entity actually occupies.
     *
     * The ray goes into the local space of each candidate, so a scaled entity is
     * bigger to click and a turned one is picked at the angle it sits at. A
     * picker that tested a world-axis box around the entity would select the
     * empty corners of anything turned.
     */
    void test_picking_respects_the_transform() {
        section("picking a scaled and a turned entity");

        {
            engine::scene::World world;
            const entt::entity big = add_box(world, { 0.0F, 0.0F, -5.0F }, { 4.0F, 4.0F, 4.0F });

            // Well outside a unit cube, inside one scaled by four.
            const engine::Ray edge{ .origin = { 1.5F, 0.0F, 0.0F },
                                    .direction = { 0.0F, 0.0F, -1.0F } };
            check(engine::editor::pick_entity(world, edge, unit_cubes()) == big,
                  "a scaled entity is bigger to click");
        }
        {
            engine::scene::World world;
            const entt::entity flat =
                add_box(world, { 0.0F, 0.0F, -5.0F }, { 8.0F, 8.0F, 0.1F });
            // Turn the wall a quarter turn about up. It is then edge on, so a
            // ray that hit its face now goes past it. A test against a world
            // axis box would still report a hit.
            world.set_local(flat, { .position = { 0.0F, 0.0F, -5.0F },
                                    .rotation = glm::angleAxis(glm::radians(90.0F),
                                                               engine::Vec3{ 0.0F, 1.0F, 0.0F }),
                                    .scale = { 8.0F, 8.0F, 0.1F } });
            world.update();

            const engine::Ray beside{ .origin = { 3.0F, 0.0F, 0.0F },
                                      .direction = { 0.0F, 0.0F, -1.0F } };
            check(engine::editor::pick_entity(world, beside, unit_cubes()) == entt::null,
                  "a wall turned edge on is no longer under a ray beside it");
        }
    }

    /// Only an entity that occupies space can be picked.
    void test_picking_skips_what_has_no_mesh() {
        section("picking something with no mesh");

        engine::scene::World world;
        const entt::entity empty = world.create();
        world.set_local(empty, { .position = { 0.0F, 0.0F, -2.0F } });
        world.update();

        const engine::Ray down_z{ .origin = { 0.0F, 0.0F, 0.0F },
                                  .direction = { 0.0F, 0.0F, -1.0F } };
        check(engine::editor::pick_entity(world, down_z, unit_cubes()) == entt::null,
              "an entity with no mesh occupies nothing and is not picked");

        // A mesh the lookup does not know is not pickable either, which is what
        // a mesh that has not finished loading looks like.
        const entt::entity unknown = add_box(world, { 0.0F, 0.0F, -2.0F });
        (void)unknown;
        const engine::editor::BoundsLookup nothing_known =
            [](engine::Guid, engine::Vec3&, engine::Vec3&) { return false; };
        check(engine::editor::pick_entity(world, down_z, nothing_known) == entt::null,
              "and neither is a mesh whose bounds are not known yet");
    }

    /**
     * Dropping a prefab makes an instance where it was dropped.
     *
     * The drag is ImGui and cannot be tested. Everything under it can: the
     * prefab the library holds, the instance it builds, where that instance
     * lands, and whether the scene file keeps it. A drop that did not survive a
     * save would look right until somebody reopened the scene.
     */
    void test_dropping_a_prefab() {
        section("dropping a prefab into the world");

        engine::assets::Content content;
        engine::scene::World world;
        check(load_shipped(world, content), "the shipped content loads");

        const std::size_t before = world.size();
        const engine::scene::Prefab* crate = engine::scene::prefabs().find("crate.prefab");
        check(crate != nullptr, "the library holds the crate prefab");

        const engine::Vec3 at{ 1.5F, 0.5F, -2.5F };
        const entt::entity root = engine::editor::drop_prefab(world, *crate, at);
        check(root != entt::null, "the drop made an instance");
        check(world.size() == before + crate->size(),
              "and the world grew by everything the prefab holds");

        const engine::Vec3 landed{ world.world_matrix(root)[3] };
        check(near_enough(landed, at), "the root landed where it was dropped");

        // It is an instance rather than loose entities, which is what keeps the
        // scene file short and what lets the prefab change under it later.
        check(world.registry().all_of<engine::scene::PrefabInstance>(root),
              "the root carries the prefab link");

        // Through the scene file and back.
        const nlohmann::json saved = engine::scene::save_scene(world);
        engine::scene::World reopened;
        check(engine::scene::load_scene(saved, reopened), "the scene with the new instance loads");
        reopened.update();
        check(reopened.size() == world.size(), "with every entity");
        check(engine::scene::save_scene(reopened) == saved,
              "and saving what was loaded gives the same document");
    }

    /**
     * Deleting a prefab member saves as a removal, not as a rebuilt instance.
     *
     * The scene format has carried `removed` since M4.5, and the editor's delete
     * button is the first thing that produces one by hand. If the writer lost it,
     * the deleted member would come back the next time the scene loaded, which
     * looks like the delete silently failing.
     */
    void test_deleting_a_prefab_member() {
        section("deleting part of a prefab instance");

        engine::assets::Content content;
        engine::scene::World world;
        check(load_shipped(world, content), "the shipped content loads");

        // The lid is a member of a crate instance, so removing it is a change to
        // the instance rather than to the prefab.
        entt::entity lid = entt::null;
        for (const auto [entity, named] :
             world.registry().view<const engine::scene::Name>().each()) {
            if (named.value == "lid" && lid == entt::null) {
                lid = entity;
            }
        }
        check(lid != entt::null, "a crate instance has a lid to remove");

        const std::size_t before = world.size();
        world.destroy(lid);
        world.update();
        check(world.size() == before - 1, "the lid went");

        const nlohmann::json saved = engine::scene::save_scene(world);

        // Somewhere in the document an instance now says what it removed. The
        // whole instance must not have been written out entity by entity.
        bool records_a_removal = false;
        for (const auto& record : saved.at("entities")) {
            if (record.contains("removed") && !record.at("removed").empty()) {
                records_a_removal = true;
            }
        }
        check(records_a_removal, "the instance records the member it lost");

        engine::scene::World reopened;
        check(engine::scene::load_scene(saved, reopened), "the scene loads again");
        reopened.update();
        check(reopened.size() == world.size(), "and the lid stays gone");
        check(engine::scene::save_scene(reopened) == saved,
              "saving what was loaded gives the same document");
    }

    /**
     * A component added by hand survives the scene file.
     *
     * What a person does after dropping a prefab: give it a body so it falls.
     * The add is `ComponentOps::create`, and this is the rest of that path.
     */
    void test_a_component_added_by_hand_is_saved() {
        section("adding a component to a placed entity");

        engine::assets::Content content;
        engine::scene::World world;
        check(load_shipped(world, content), "the shipped content loads");

        const engine::scene::Prefab* crate = engine::scene::prefabs().find("crate.prefab");
        check(crate != nullptr, "the library holds the crate prefab");
        const entt::entity placed =
            engine::editor::drop_prefab(world, *crate, engine::Vec3{ 0.0F, 3.0F, 0.0F });
        check(placed != entt::null, "the drop made an instance");

        const engine::scene::ComponentOps* light = engine::scene::components().find("PointLight");
        check(light != nullptr, "the registry holds PointLight");
        check(!light->has(world.registry(), placed), "the crate carries none");

        light->create(world.registry(), placed);
        check(light->has(world.registry(), placed), "and now it does");

        const nlohmann::json saved = engine::scene::save_scene(world);
        engine::scene::World reopened;
        check(engine::scene::load_scene(saved, reopened), "the scene loads again");
        reopened.update();

        std::size_t lights = 0;
        for (const auto [entity, unused] :
             reopened.registry().view<const engine::scene::PointLight>().each()) {
            (void)unused;
            ++lights;
        }
        check(lights == 3, "the light came back, beside the two the scene already had");
    }

    /**
     * A dropped prefab survives the path the panel actually writes.
     *
     * The other tests here go through `scene::save_scene`. The World panel goes
     * through `editor::save_scene_source`, which also puts the asset references
     * back and writes through a temporary and a rename. A drop that reached the
     * document but not the file would look exactly like the save doing nothing,
     * which is what was reported in the M9.8 run.
     */
    void test_a_dropped_prefab_reaches_the_file() {
        section("a dropped prefab through the panel's own save");

        engine::assets::Content content;
        engine::scene::World world;
        check(load_shipped(world, content), "the shipped content loads");

        const engine::scene::Prefab* crate = engine::scene::prefabs().find("crate.prefab");
        check(crate != nullptr, "the library holds the crate prefab");
        const engine::Vec3 at{ -2.0F, 4.5F, 1.0F };
        check(engine::editor::drop_prefab(world, *crate, at) != entt::null, "the drop worked");

        const std::filesystem::path file =
            std::filesystem::temp_directory_path() / "camina_dropped.scene";
        std::error_code ignored;
        std::filesystem::remove(file, ignored);

        check(engine::editor::save_scene_source(file, world, content.manifest()), "the panel's save writes");
        check(std::filesystem::exists(file), "and the file is there");

        // Read the file rather than load it. A source scene names its assets by
        // reference, `asset:models/crate/crate.gltf#mesh:0` and the like, and
        // turning those back into identities is the cooker's job. Loading one
        // directly reports every reference it could not read, which says
        // nothing about whether the drop reached the file.
        // The stream closes before the file is removed at the end. Windows
        // refuses to delete a file another handle holds open, which check.h
        // says at remove_tree() and which this test found the hard way.
        nlohmann::json written;
        {
            std::ifstream reading(file);
            written = nlohmann::json::parse(reading, nullptr, false);
        }
        check(!written.is_discarded(), "the written file parses");

        std::size_t crates_at_the_drop = 0;
        for (const auto& record : written.at("entities")) {
            if (!record.contains("prefab") || record.at("prefab") != "crate.prefab") {
                continue;
            }
            const auto& overrides = record.value("overrides", nlohmann::json::object());
            const auto& root = overrides.value("0", nlohmann::json::object());
            const auto& transform = root.value("Transform", nlohmann::json::object());
            if (!transform.contains("position")) {
                continue;
            }
            const auto& p = transform.at("position");
            if (near_enough(engine::Vec3{ p[0].get<float>(), p[1].get<float>(),
                                          p[2].get<float>() },
                            at)) {
                ++crates_at_the_drop;
            }
        }
        check(crates_at_the_drop == 1,
              "the file holds one crate instance at the place it was dropped");

        test::remove_tree(file);
    }

} // namespace

/// The first entity carrying a Name, so a test can name one of the shipped ones.
[[nodiscard]] entt::entity find_named(const engine::scene::World& world, const char* wanted) {
    for (const auto [entity, name] :
         world.registry().view<const engine::scene::Name>().each()) {
        if (name.value == wanted) {
            return entity;
        }
    }
    return entt::null;
}

/**
 * A named entity of the shipped scene, for a test to edit.
 *
 * **A scene restores its entities down two different paths**, and an
 * identity is put back by each one separately. An entity the file lists
 * itself goes through `take_identity` in `scene/scene_file.cpp`, and a
 * prefab instance goes through `assign_identities` in `scene/prefab.cpp`.
 * A test that only reaches one of them passes while the other is broken,
 * so the caller asks for each kind by name.
 *
 * @param world The world to search.
 * @param inside_prefab True for an entity a prefab supplied, false for one
 * the scene file lists on its own.
 * @return The entity with the smallest number that matches, or entt::null.
 */
[[nodiscard]] entt::entity any_named(const engine::scene::World& world,
                                     bool inside_prefab) {
    entt::entity best = entt::null;
    for (const auto [entity, name] :
         world.registry().view<const engine::scene::Name>().each()) {
        const bool member = world.registry().all_of<engine::scene::PrefabMember>(entity);
        if (member != inside_prefab) {
            continue;
        }
        if (best == entt::null || entt::to_integral(entity) < entt::to_integral(best)) {
            best = entity;
        }
    }
    return best;
}

/**
 * An edit made before a play can be undone after a stop.
 *
 * This is M12.5. Pressing play to see whether a change works, then stopping
 * and undoing it, is the loop the milestone is for, and a history that
 * emptied itself at either end would be worth very little.
 *
 * It works because a stop reads back a document that carries the identity
 * of every entity, so an entry finds its entity by asking for it by name.
 * The entity numbers are all different afterwards and none of them matter.
 */
void test_an_edit_before_play_undoes_after_stop() {
    section("an edit survives a play and a stop");

    engine::assets::Content content;
    engine::scene::World world;
    check(load_shipped(world, content), "the shipped scene loads");

    // One of each kind, because the two are restored down different paths
    // and an identity is put back by each one separately.
    const std::array<entt::entity, 2> moved{ any_named(world, true),
                                             any_named(world, false) };
    check(moved[0] != entt::null, "the scene holds an entity a prefab supplied");
    check(moved[1] != entt::null, "and one the file lists itself");
    if (moved[0] == entt::null || moved[1] == entt::null) {
        return;
    }

    std::array<engine::Guid, 2> moved_id{};
    std::array<std::uint32_t, 2> before_number{};
    std::array<float, 2> was_x{};

    // Move each one the way a gizmo drag does: keep the value, change the
    // world, then record one entry.
    engine::editor::History history;
    for (std::size_t i = 0; i < moved.size(); ++i) {
        moved_id[i] = world.identity(moved[i]);
        before_number[i] = entt::to_integral(moved[i]);

        engine::editor::Interaction drag;
        check(drag.begin(world, moved[i], "Transform"), "the drag opens");

        engine::Transform local = world.local(moved[i]);
        was_x[i] = local.position.x;
        local.position.x = was_x[i] + 5.0F;
        world.set_local(moved[i], local);
        check(drag.end(world, history), "and one entry comes out");
    }
    check(history.size() == 2, "the stack holds both");

    // A whole session over it: the crates fall and a script may create and
    // destroy things.
    engine::editor::PlayMode play;
    const engine::editor::PlayDesc desc{ .content = &content,
                                         .bind_actions = &sandbox::bind_actions };
    check(play.play(world, desc), "the session starts");
    run(play, world, 60);
    play.stop(world);

    check(history.size() == 2, "the stack is still there after the stop");

    for (std::size_t i = 0; i < moved.size(); ++i) {
        const entt::entity again = world.find(moved_id[i]);
        check(again != entt::null, "the entity answers to the same identity");
        check(entt::to_integral(again) != before_number[i],
              "and it is a different entity number, so the lookup is doing the work");
        check(world.local(again).position.x == was_x[i] + 5.0F,
              "the move is still applied");
    }

    // Undone in the order they were made, last one first.
    check(history.undo(world), "undo runs after the stop");
    check(world.local(world.find(moved_id[1])).position.x == was_x[1],
          "and it undoes the right thing");
    check(history.undo(world), "the entry under it runs too");
    check(world.local(world.find(moved_id[0])).position.x == was_x[0],
          "and reaches the other restore path");

    check(history.redo(world), "redo runs too");
    check(world.local(world.find(moved_id[0])).position.x == was_x[0] + 5.0F,
          "and puts the move back");
}

/**
 * A delete made before a play is undone after a stop.
 *
 * The harder half. Undo has to build the entity again into a world whose
 * every entity was built a moment ago, and every other entry has to keep
 * resolving.
 */
void test_a_delete_before_play_undoes_after_stop() {
    section("a delete survives a play and a stop");

    engine::assets::Content content;
    engine::scene::World world;
    check(load_shipped(world, content), "the shipped scene loads");

    const entt::entity going = any_named(world, false);
    check(going != entt::null, "the scene holds an entity to delete");
    if (going == entt::null) {
        return;
    }
    const engine::Guid going_id = world.identity(going);
    const std::size_t before = world.size();

    engine::editor::History history;
    check(engine::editor::delete_entity(world, going, &history), "the delete runs");
    check(world.find(going_id) == entt::null, "the entity is gone");

    engine::editor::PlayMode play;
    const engine::editor::PlayDesc desc{ .content = &content,
                                         .bind_actions = &sandbox::bind_actions };
    check(play.play(world, desc), "the session starts");
    run(play, world, 60);
    play.stop(world);

    check(world.find(going_id) == entt::null,
          "the snapshot was taken without it, so it is still gone");

    check(history.undo(world), "undo runs after the stop");
    check(world.find(going_id) != entt::null, "and the entity is back");
    check(world.size() == before, "with everything that went with it");
}

/**
 * The selection survives a play and a stop.
 *
 * The editor holds an entity number, and every number changes at a stop.
 * The identity is what carries across, and this is the lookup the editor
 * does around `PlayMode::stop`.
 */
void test_the_selection_survives_a_play_and_a_stop() {
    section("the selection survives a play and a stop");

    engine::assets::Content content;
    engine::scene::World world;
    check(load_shipped(world, content), "the shipped scene loads");

    const entt::entity selected = any_named(world, true);
    check(selected != entt::null, "something is selected");
    if (selected == entt::null) {
        return;
    }
    const std::string name = world.registry().get<engine::scene::Name>(selected).value;

    // What the editor does: read the identity before the stop.
    const engine::Guid was_selected = world.identity(selected);
    check(was_selected.valid(), "the selection has an identity");

    engine::editor::PlayMode play;
    const engine::editor::PlayDesc desc{ .content = &content,
                                         .bind_actions = &sandbox::bind_actions };
    check(play.play(world, desc), "the session starts");
    run(play, world, 60);
    play.stop(world);

    // And the lookup after it.
    const entt::entity again = world.find(was_selected);
    check(again != entt::null, "the selection is found again after the stop");
    check(world.registry().get<engine::scene::Name>(again).value == name,
          "and it is the same entity somebody was looking at");

    // A null selection asks for nothing and gets nothing, rather than
    // asserting on a stale entity.
    check(!world.identity(entt::null).valid(), "nothing selected has no identity");
}

/**
 * An entry naming an entity that no stop can bring back is reported.
 *
 * The entity is destroyed without an entry, so no undo builds it again and
 * the snapshot never held it. The edit that names it then reaches nothing.
 * It reports on the error channel and changes no other entity, which is
 * what "reported rather than passed over" has to mean for a stack that
 * still has to work for every other entry on it.
 */
void test_an_entry_naming_a_lost_entity_is_reported() {
    section("an entry that names a lost entity");

    engine::assets::Content content;
    engine::scene::World world;
    check(load_shipped(world, content), "the shipped scene loads");

    const entt::entity doomed = any_named(world, false);
    check(doomed != entt::null, "the scene holds an entity");
    if (doomed == entt::null) {
        return;
    }
    const engine::Guid doomed_id = world.identity(doomed);

    engine::editor::History history;
    engine::editor::Interaction drag;
    check(drag.begin(world, doomed, "Transform"), "an edit opens on it");
    engine::Transform local = world.local(doomed);
    local.position.x += 2.0F;
    world.set_local(doomed, local);
    check(drag.end(world, history), "and is recorded");

    // Destroyed with no entry of its own, so nothing can bring it back.
    world.destroy(doomed);
    check(world.find(doomed_id) == entt::null, "the entity is gone for good");

    engine::editor::PlayMode play;
    const engine::editor::PlayDesc desc{ .content = &content,
                                         .bind_actions = &sandbox::bind_actions };
    check(play.play(world, desc), "the session starts");
    run(play, world, 30);
    play.stop(world);

    const nlohmann::json before = engine::scene::save_scene(world);

    // The error line above this check is the report. Undo still advances,
    // so a second one goes further back rather than retrying this one.
    check(history.undo(world), "undo runs");
    check(engine::scene::save_scene(world) == before,
          "and it changed nothing, rather than moving another entity");
    check(!history.can_undo(), "the stack moved past it");
}

#if defined(ENGINE_WITH_LUA)
/**
 * A game that asks to quit stops the session, and never the editor.
 *
 * The runtime leaves its frame loop on the same request. Here it has to mean
 * something else, because a game that could end this process would take a
 * person's unsaved work with it. So the world comes back exactly as a stop
 * leaves it.
 */
void test_a_game_that_quits_stops_play() {
    section("a game that quits stops play rather than the editor");

    engine::assets::Content content;
    engine::scene::World world;
    check(load_shipped(world, content), "the shipped content loads");

    const nlohmann::json authored = engine::scene::save_scene(world);

    engine::editor::PlayMode play;
    const engine::editor::PlayDesc desc{ .content = &content,
                                         .bind_actions = &sandbox::bind_actions };
    check(play.play(world, desc), "the session starts");

    run(play, world, 2);
    check(play.state() == engine::editor::PlayState::Playing, "and it is playing");

    // Asked directly rather than through a button, because a click needs a
    // pointer and this test has no window. What the button does is this call.
    check(play.session() != nullptr, "the session is there to ask");
    play.session()->request_quit();

    run(play, world, 1);
    check(play.state() == engine::editor::PlayState::Edit,
          "the next frame stopped play");
    check(engine::scene::save_scene(world) == authored,
          "and the world came back as it was authored");
}
#endif

#if defined(ENGINE_WITH_AUDIO)

/// A cooked PCM sound, built by hand. This needs no cooker and no WAV.
std::vector<std::byte> cooked_tone() {
    engine::assets::SoundHeader header;
    header.storage = static_cast<std::uint32_t>(engine::assets::SoundStorage::Pcm);
    header.channels = 2;
    header.sample_rate = 48000;
    header.frame_count = 48000;
    const std::size_t samples = static_cast<std::size_t>(header.frame_count) *
                                header.channels;
    header.payload_size = static_cast<std::uint32_t>(samples * sizeof(float));

    std::vector<std::byte> out(sizeof(header) + header.payload_size);
    std::memcpy(out.data(), &header, sizeof(header));
    const std::vector<float> values(samples, 0.25F);
    std::memcpy(out.data() + sizeof(header), values.data(), header.payload_size);
    return out;
}

/// A project holding one sound and one script that plays it forever.
class NoisyProject final : public engine::assets::AssetSource {
public:
    [[nodiscard]] bool assets_for(std::string_view /*source*/,
                                  std::vector<engine::assets::AssetRecord>& /*out*/) const override {
        return false;
    }

    [[nodiscard]] bool assets_of_kind(
        std::string_view suffix,
        std::vector<engine::assets::AssetRecord>& out) const override {
        out.clear();
        if (suffix == engine::assets::kSoundExtension) {
            out.push_back(engine::assets::AssetRecord{ .guid = kSound,
                                                       .source = "sounds/hum.wav",
                                                       .name = "sounds/hum.wav.snd" });
        }
        if (suffix == engine::assets::kScriptExtension) {
            out.push_back(engine::assets::AssetRecord{ .guid = kScript,
                                                       .source = "scripts/noisy.lua",
                                                       .name = "scripts/noisy.lua" });
        }
        return true;
    }

    [[nodiscard]] bool read(engine::Guid guid, std::vector<std::byte>& out) const override {
        if (guid == kSound) {
            out = cooked_tone();
            return true;
        }
        if (guid == kScript) {
            static constexpr std::string_view kText = R"(
                    function on_start()
                        audio.play("sounds/hum.wav", { looping = true })
                    end
                )";
            out.assign(reinterpret_cast<const std::byte*>(kText.data()),                 // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
                       reinterpret_cast<const std::byte*>(kText.data()) + kText.size()); // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
            return true;
        }
        return false;
    }

    static constexpr engine::Guid kSound{ 42, 1 };
    static constexpr engine::Guid kScript{ 42, 2 };
};

/**
 * A stopped session is silent, looping sounds included.
 *
 * This is the trap issue #427 named. A session plays what a scene and its
 * scripts ask for, and a person who presses Stop expects the room to go
 * quiet. A looping voice left behind would have nothing holding its number,
 * so nothing could ever stop it: only closing the editor would.
 *
 * Both kinds of voice are here, because they are stopped by different
 * paths. The script's voice goes when the host destroys its instance, and
 * the component's goes because PlayMode silences the scene.
 */
void test_a_stopped_session_is_silent() {
    section("a stopped session is silent");

    NoisyProject project;
    engine::audio::Mixer mixer;
    check(mixer.create(2, 48000), "the mixer builds");

    engine::audio::SceneAudio scene_audio;
    scene_audio.bind(mixer, project);
    engine::audio::ScriptAudio script_audio;
    script_audio.bind(mixer, project);

    engine::scene::World world;

    // One entity plays through a component, and one through a script.
    const entt::entity speaker = world.create();
    world.registry().emplace<engine::scene::AudioSource>(
        speaker, engine::scene::AudioSource{ .sound = NoisyProject::kSound,
                                             .looping = true,
                                             .play_on_start = true });
    const entt::entity noisy = world.create();
    world.registry().emplace<engine::script::ScriptComponent>(
        noisy, engine::script::ScriptComponent{ NoisyProject::kScript });
    world.update();

    engine::editor::PlayMode play;
    const engine::editor::PlayDesc desc{ .content = &project,
                                         .bind_actions = nullptr,
                                         .script_audio = &script_audio,
                                         .scene_audio = &scene_audio };
    check(play.play(world, desc), "the session starts");

    run(play, world, 4);
    check(scene_audio.playing() == 1, "the scene is playing its sound");
    check(script_audio.voices() == 1, "and the script is playing one of its own");
    check(mixer.voices() == 2, "so the mixer holds two voices");

    play.stop(world);
    check(scene_audio.playing() == 0, "stopping the session stopped the scene's voice");
    check(script_audio.voices() == 0, "and the script's voice with it");
    check(mixer.voices() == 0, "and the mixer holds none at all");

    // Nothing starts again on its own once the session is over. The world
    // is the authored one now, and nothing is stepping it.
    check(!play.running(), "the session is over");
}

#endif

int main() {
    register_everything();

    // The physics world runs its solver on the scheduler, so a session needs
    // the pool up before it builds one.
    engine::jobs::init();

    test_dropping_a_prefab();
    test_a_dropped_prefab_reaches_the_file();
    test_deleting_a_prefab_member();
    test_a_component_added_by_hand_is_saved();
    test_picking_the_nearest_entity();
    test_a_room_does_not_swallow_every_click();
    test_picking_respects_the_transform();
    test_picking_skips_what_has_no_mesh();

    test_a_transform_round_trips_through_a_matrix();
    test_placing_an_entity_under_a_parent();
    test_placing_a_root_entity();

    test_a_dragged_entity_survives_the_scene_file();
#if defined(ENGINE_WITH_AUDIO)
    test_a_stopped_session_is_silent();
#endif
    test_stop_restores_the_authored_world();
    test_a_second_session_starts_from_the_same_place();
    test_pause_holds_the_step();
    test_the_states_refuse_what_they_cannot_do();
#if defined(ENGINE_WITH_LUA)
    test_a_game_that_quits_stops_play();
#endif

    test_an_edit_before_play_undoes_after_stop();
    test_a_delete_before_play_undoes_after_stop();
    test_the_selection_survives_a_play_and_a_stop();
    test_an_entry_naming_a_lost_entity_is_reported();

    engine::jobs::shutdown();
    return test::report();
}
