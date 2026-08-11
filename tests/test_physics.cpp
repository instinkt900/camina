// M7.1 tests for the Box3D conventions.
//
// DESIGN.md section 3 carried an open item from the day the conventions were
// settled. It asked somebody to confirm that Box3D agrees about which way is
// up. This is that check, written so it runs on every build rather than once.
//
// A physics library that disagrees with the renderer about up produces mirrored
// or inverted motion. The symptom points at the gameplay code rather than at
// the library. That is why this is a test and not a comment.

#include "check.h"
#include "core/jobs.h"
#include "core/timestep.h"
#include "physics/components.h"
#include "physics/conventions.h"
#include "physics/simulation.h"
#include "physics/world.h"
#include "reflect/json.h"
#include "scene/component_registry.h"
#include "scene/prefab.h"
#include "scene/world.h"

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <vector>

namespace {

    using engine::Quat;
    using engine::Vec3;
    using engine::physics::default_gravity;
    using test::check;
    using test::section;

    /// Box3D uses -10 rather than -9.81. See physics/conventions.h.
    constexpr float kExpectedMagnitude = 10.0F;

    /// Half a percent of a meter per second squared. The value is exact today,
    /// and an update that trims it towards 9.81 is not what this test guards.
    constexpr float kTolerance = 0.05F;

    void gravity_points_down_y() {
        section("Box3D agrees with DESIGN.md section 3");

        const Vec3 gravity = default_gravity();

        // The engine is +Y up, so the default gravity has to be -Y. The two
        // checks either side of it are what tell a wrong axis from a wrong
        // sign. A library that used +Z up would pass neither.
        check(gravity.y < 0.0F, "default gravity points down -Y");
        check(gravity.x == 0.0F, "default gravity has no X");
        check(gravity.z == 0.0F, "default gravity has no Z");

        check(std::fabs(std::fabs(gravity.y) - kExpectedMagnitude) < kTolerance,
              "default gravity is about 10 meters per second squared");
    }

    constexpr float kBoxHalfSize = 0.5F;
    constexpr float kStep = 1.0F / 60.0F;
    constexpr std::uint32_t kStepCount = 120;

    /// A millimeter, which is far below anything a wrong blend would produce.
    constexpr float kBlendTolerance = 1e-3F;

    /// About 23 degrees. Enough that the box tips rather than lands flat.
    constexpr float kTiltRadians = 0.4F;

    /// A tenth of a millimeter. A body that has settled moves less than this,
    /// and one that is shaking on the spot moves far more.
    constexpr float kRestTolerance = 1e-4F;

    /// Tall enough to fall over and be seen doing it, short enough to settle
    /// quickly. This is the stack the sandbox carries.
    constexpr std::uint32_t kStackHeight = 4;

    /// A millimeter between crates at the start, so the stack settles under
    /// gravity rather than resolving an overlap it began with.
    constexpr float kStackGap = 1e-3F;

    /// Two centimeters. A settled stack sinks a little into its contacts,
    /// because a solver resolves penetration rather than forbidding it.
    constexpr float kStackTolerance = 2e-2F;

    /**
     * Meters each second, along +X.
     *
     * Fast enough that the drop over the flight is small. A throw is a velocity
     * set once, so gravity bends the path for as long as it is in the air. At
     * 25 metres each second it crosses the three metres to the stack in an
     * eighth of a second and falls under eight centimeters doing it, so it
     * arrives where it was aimed.
     */
    constexpr float kThrowSpeed = 25.0F;

    /**
     * Where the throw starts, and what it is aimed at.
     *
     * **The height is the whole test.** A first attempt threw at 1.2 metres and
     * the ball dropped to the floor before it arrived, so it struck the bottom
     * crate at its base and bounced off. The stack shifted 11 centimeters and
     * stood. Toppling a stack is leverage: a hit near the top turns the crates
     * above the contact over, and a hit at the bottom just shoves the whole
     * mass along.
     */
    constexpr Vec3 kThrowFrom{ -4.0F, 2.6F, 0.0F };

    /// A quarter of a crate. Less than this is a stack that shifted, and more
    /// is one that came apart.
    constexpr float kKnockedTolerance = 0.25F;

    /// Two seconds at the fixed rate, which settles this stack with room over.
    constexpr std::uint32_t kSettleSteps = 120;

    /// A stack big enough that the solver has real work to split.
    constexpr std::uint32_t kLargeWidth = 8;
    constexpr std::uint32_t kLargeHeight = 12;

    /// A stack the size the sandbox will actually hold. The issue behind this
    /// work asks whether the job system pays for itself at that size, and only a
    /// measurement at that size can answer it.
    constexpr std::uint32_t kSmallWidth = 2;
    constexpr std::uint32_t kSmallHeight = 3;

    /// No rotation, which every body in these tests starts with.
    constexpr Quat kUpright{ 1.0F, 0.0F, 0.0F, 0.0F };

    /// Puts one box on a body at a position, and hands the body back.
    engine::physics::BodyId add_box_body(engine::physics::World& world,
                                         engine::physics::BodyType type, const Vec3& center,
                                         const Vec3& half_extents) {
        const engine::physics::BodyId body = world.add_body(type, center, kUpright);
        world.add_box(body, half_extents, engine::physics::SurfaceMaterial{});
        return body;
    }

    /// Builds the same scene every time, so two runs differ only in the worker
    /// count they were given.
    std::vector<engine::physics::BodyId> build_stack(engine::physics::World& world,
                                                     std::uint32_t width, std::uint32_t height) {
        add_box_body(world, engine::physics::BodyType::Static, Vec3{ 0.0F, -1.0F, 0.0F },
                     Vec3{ 50.0F, 1.0F, 50.0F });

        std::vector<engine::physics::BodyId> boxes;
        for (std::uint32_t y = 0; y < height; ++y) {
            for (std::uint32_t x = 0; x < width; ++x) {
                for (std::uint32_t z = 0; z < width; ++z) {
                    const Vec3 center{ static_cast<float>(x) * 1.2F,
                                       (static_cast<float>(y) * 1.2F) + kBoxHalfSize,
                                       static_cast<float>(z) * 1.2F };
                    boxes.push_back(add_box_body(world, engine::physics::BodyType::Dynamic, center,
                                                 Vec3{ kBoxHalfSize, kBoxHalfSize, kBoxHalfSize }));
                }
            }
        }
        return boxes;
    }

    /// Steps the scene and returns how long the steps took, in milliseconds.
    double run_steps(engine::physics::World& world) {
        const auto start = std::chrono::steady_clock::now();
        for (std::uint32_t i = 0; i < kStepCount; ++i) {
            world.step(kStep);
        }
        const auto end = std::chrono::steady_clock::now();
        return std::chrono::duration<double, std::milli>(end - start).count();
    }

    void a_box_falls_and_lands() {
        section("A body falls, and the floor stops it");

        engine::physics::World world;
        const engine::physics::BodyId box =
            add_box_body(world, engine::physics::BodyType::Dynamic, Vec3{ 0.0F, 8.0F, 0.0F },
                         Vec3{ kBoxHalfSize, kBoxHalfSize, kBoxHalfSize });
        add_box_body(world, engine::physics::BodyType::Static, Vec3{ 0.0F, -1.0F, 0.0F },
                     Vec3{ 50.0F, 1.0F, 50.0F });

        const float start_height = world.body_position(box).y;
        for (std::uint32_t i = 0; i < kStepCount; ++i) {
            world.step(kStep);
        }
        const Vec3 landed = world.body_position(box);

        // Falling proves the step ran at all. Landing proves the solver ran, and
        // that is the part the task callbacks carry.
        check(landed.y < start_height, "the box fell");
        check(landed.y > 0.0F, "the floor stopped it above the origin");
        check(std::fabs(landed.x) < 0.1F && std::fabs(landed.z) < 0.1F,
              "it fell straight down");
    }

    void the_worker_count_matches_the_job_system() {
        section("Box3D is told what the job system actually has");

        const engine::physics::World shared;
        check(shared.worker_count() == engine::jobs::worker_count(),
              "a default world takes the job system worker count");

        const engine::physics::World single(1);
        check(single.worker_count() == 1, "a world can be asked for one worker");
    }

    void the_solver_runs_on_the_job_system() {
        section("The solver runs on the job system and not on threads of its own");

        // Box3D falls back to its own scheduler and its own threads whenever a
        // world definition leaves either callback null. A step that ran that way
        // would land the boxes in the same place and take about the same time, so
        // the count is the only thing that tells the two apart.
        const std::uint64_t before = engine::physics::tasks_enqueued();

        engine::physics::World world;
        build_stack(world, kSmallWidth, kSmallHeight);
        world.step(kStep);

        check(engine::physics::tasks_enqueued() > before,
              "one step handed work to the job system");
    }

    /// Reports the cost of a step on the pool against the cost on one thread.
    ///
    /// This asserts nothing about the time. A test machine is shared, a CI runner
    /// is shared harder, and a threshold on wall time fails for reasons that have
    /// nothing to do with this code. The number is here to be read, and the pull
    /// request records what it said on the reference machine.
    ///
    /// It does assert that the two runs agree about where the stack ended up. A
    /// scheduler that drops a task, or returns from finish before the task has
    /// finished, changes the answer rather than the timing.
    void report_step_cost(const char* label, std::uint32_t width, std::uint32_t height) {
        engine::physics::World threaded;
        const std::vector<engine::physics::BodyId> threaded_boxes =
            build_stack(threaded, width, height);
        const double threaded_ms = run_steps(threaded);

        engine::physics::World single(1);
        const std::vector<engine::physics::BodyId> single_boxes =
            build_stack(single, width, height);
        const double single_ms = run_steps(single);

        std::printf("  ----  %s: %u bodies, %.3f ms per step on %u workers, %.3f ms on 1\n", label,
                    threaded.body_count(), threaded_ms / kStepCount, threaded.worker_count(),
                    single_ms / kStepCount);
        std::fflush(stdout);

        std::uint32_t moved = 0;
        for (std::size_t i = 0; i < threaded_boxes.size(); ++i) {
            const Vec3 a = threaded.body_position(threaded_boxes[i]);
            const Vec3 b = single.body_position(single_boxes[i]);
            if (std::fabs(a.y - b.y) > 0.05F) {
                ++moved;
            }
        }

        check(threaded_boxes.size() == single_boxes.size(), "both runs built the same scene");
        check(moved == 0, "the threaded run and the single-threaded run agree");
    }

    namespace sc = engine::scene;
    namespace ph = engine::physics;

    /// A registry that knows the engine components and the physics ones.
    sc::ComponentRegistry make_registry() {
        sc::ComponentRegistry registry;
        sc::register_builtin_components(registry);
        ph::register_components(registry);
        return registry;
    }

    void the_components_reflect() {
        section("The physics components describe themselves");

        // Nothing was written for this. The descriptors are the whole of it,
        // which is hard rule 4.5.
        check(engine::reflect::field_count<ph::RigidBody>() == 4, "RigidBody describes 4 fields");
        check(engine::reflect::field_count<ph::BoxCollider>() == 1, "BoxCollider describes 1");
        check(engine::reflect::field_count<ph::SphereCollider>() == 1, "SphereCollider describes 1");

        check(engine::reflect::DescribedEnum<ph::BodyType>, "BodyType is a described enum");
        check(engine::reflect::enumerator_count<ph::BodyType>() == 3,
              "static, dynamic and kinematic are each expressible");

        const sc::ComponentRegistry registry = make_registry();
        check(registry.find("RigidBody") != nullptr, "a scene file can carry a RigidBody");
        check(registry.find("BoxCollider") != nullptr, "and a BoxCollider");
        check(registry.find("SphereCollider") != nullptr, "and a SphereCollider");
    }

    void a_body_round_trips() {
        section("A component survives a write and a read");

        ph::RigidBody body;
        body.type = ph::BodyType::Kinematic;
        body.density = 700.0F;
        body.friction = 0.25F;
        body.restitution = 0.5F;

        const nlohmann::json out = engine::reflect::to_json(body);

        // The name rather than the number, so inserting an enumerator above
        // Kinematic does not turn every lift in every scene into a wall.
        check(out["type"] == "Kinematic", "the body type writes its name");

        ph::RigidBody loaded;
        check(engine::reflect::from_json(out, loaded), "the document reads back");
        check(loaded.type == ph::BodyType::Kinematic, "the type survives");
        check(loaded.density == 700.0F, "the density survives");
        check(loaded.friction == 0.25F, "the friction survives");
        check(loaded.restitution == 0.5F, "the restitution survives");

        ph::BoxCollider box;
        box.half_extents = Vec3{ 1.0F, 2.0F, 3.0F };
        ph::BoxCollider loaded_box;
        check(engine::reflect::from_json(engine::reflect::to_json(box), loaded_box) &&
                  loaded_box.half_extents == box.half_extents,
              "a box collider survives");

        ph::SphereCollider sphere;
        sphere.radius = 2.5F;
        ph::SphereCollider loaded_sphere;
        check(engine::reflect::from_json(engine::reflect::to_json(sphere), loaded_sphere) &&
                  loaded_sphere.radius == sphere.radius,
              "a sphere collider survives");
    }

    /// What the crate prefab carries. None of it is a default value, so a check
    /// against one of these fails when the prefab data does not reach the
    /// instance. Checking against a default proves nothing, because an
    /// instantiate that dropped the data and default-constructed the components
    /// would pass.
    constexpr float kCrateDensity = 700.0F;
    constexpr Vec3 kCrateHalfExtents{ 0.75F, 0.25F, 1.5F };

    /// A one-entity prefab carrying a body and a box.
    nlohmann::json crate_document() {
        ph::RigidBody body;
        body.density = kCrateDensity;

        ph::BoxCollider box;
        box.half_extents = kCrateHalfExtents;

        nlohmann::json root = nlohmann::json::object();
        root["parent"] = -1;
        root["components"]["Transform"] = engine::reflect::to_json(engine::Transform{});
        root["components"]["RigidBody"] = engine::reflect::to_json(body);
        root["components"]["BoxCollider"] = engine::reflect::to_json(box);

        nlohmann::json document = nlohmann::json::object();
        document["__version"] = sc::kPrefabVersion;
        document["entities"] = nlohmann::json::array({ root });
        return document;
    }

    void a_prefab_instance_overrides_one_field() {
        section("A prefab instance overrides one physics field");

        const sc::ComponentRegistry registry = make_registry();

        sc::Prefab crate;
        check(sc::Prefab::parse("crate", crate_document(), crate), "the crate prefab parses");

        // One field of one component. Everything else has to come from the
        // prefab, which is what makes an override an override.
        nlohmann::json record = nlohmann::json::object();
        record["overrides"]["0"]["RigidBody"]["type"] = "Static";

        sc::World world;
        const entt::entity entity = sc::instantiate(world, crate, record, registry);
        check(entity != entt::null, "the instance builds");

        const auto& body = world.registry().get<ph::RigidBody>(entity);
        check(body.type == ph::BodyType::Static, "the overridden field took the new value");

        // Against the prefab value rather than the default. An instantiate that
        // dropped the prefab data and default-constructed the component would
        // pass a check against the default, which is what this used to do.
        check(body.density == kCrateDensity, "the field next to it comes from the prefab");

        const auto& box = world.registry().get<ph::BoxCollider>(entity);
        check(box.half_extents == kCrateHalfExtents,
              "the component next to it comes from the prefab");
    }

    /// Puts a body and a box on an entity, and places it.
    entt::entity add_physics_entity(sc::World& world, ph::BodyType type, const Vec3& position) {
        const entt::entity entity = world.create();
        world.set_local(entity, engine::Transform{ .position = position });
        world.registry().emplace<ph::RigidBody>(entity, ph::RigidBody{ .type = type });
        world.registry().emplace<ph::BoxCollider>(entity);
        return entity;
    }

    void the_transforms_follow_the_solver() {
        section("A world steps the bodies and the transforms follow");

        sc::World world;
        const entt::entity crate = add_physics_entity(world, ph::BodyType::Dynamic,
                                                      Vec3{ 0.0F, 6.0F, 0.0F });

        const entt::entity floor = add_physics_entity(world, ph::BodyType::Static,
                                                      Vec3{ 0.0F, -1.0F, 0.0F });
        world.registry().get<ph::BoxCollider>(floor).half_extents = Vec3{ 50.0F, 1.0F, 50.0F };

        ph::Simulation simulation;
        simulation.build(world);
        check(simulation.body_count() == 2, "both entities got a body");

        const float floor_start = world.local(floor).position.y;
        for (std::uint32_t i = 0; i < kStepCount; ++i) {
            simulation.step(world, kStep);
        }
        // The step records the pose and interpolate() writes it. An alpha of 1
        // asks for the newest step with no blending.
        simulation.interpolate(world, 1.0F);

        const engine::Transform& landed = world.local(crate);
        check(landed.position.y < 6.0F, "the dynamic entity fell");
        check(landed.position.y > 0.0F, "the floor stopped it");
        check(world.local(floor).position.y == floor_start, "the static entity did not move");
    }

    void the_scale_survives_the_write_back() {
        section("The write-back leaves the scale alone");

        sc::World world;
        const entt::entity crate = world.create();
        world.set_local(crate, engine::Transform{ .position = { 0.0F, 4.0F, 0.0F },
                                                  .scale = { 2.0F, 2.0F, 2.0F } });
        world.registry().emplace<ph::RigidBody>(crate);
        world.registry().emplace<ph::BoxCollider>(crate);

        ph::Simulation simulation;
        simulation.build(world);
        for (std::uint32_t i = 0; i < 10; ++i) {
            simulation.step(world, kStep);
        }
        simulation.interpolate(world, 1.0F);

        // A rigid body has no scale to give back, so the one the scene set has
        // to survive. Writing a whole transform back would reset it to 1.
        check(world.local(crate).scale == Vec3{ 2.0F, 2.0F, 2.0F },
              "the scale the scene set is still there");
        check(world.local(crate).position.y < 4.0F, "and the body still moved it");
    }

    void a_parented_dynamic_body_is_refused() {
        section("A dynamic body under a parent is refused");

        sc::World world;
        const entt::entity parent = world.create();
        world.set_local(parent, engine::Transform{ .position = { 0.0F, 5.0F, 0.0F } });

        const entt::entity child = add_physics_entity(world, ph::BodyType::Dynamic,
                                                      Vec3{ 0.0F, 1.0F, 0.0F });
        check(world.set_parent(child, parent), "the child attaches to the parent");

        ph::Simulation simulation;
        simulation.build(world);

        // DESIGN.md section 9. It gets no body at all rather than a body that
        // quietly ignores the parent link.
        check(!simulation.has_body(child), "the parented dynamic entity got no body");
        check(simulation.body_count() == 0, "and nothing else did either");
    }

    void a_parented_static_body_is_allowed() {
        section("A static body under a parent is allowed");

        sc::World world;
        const entt::entity parent = world.create();
        world.set_local(parent, engine::Transform{ .position = { 3.0F, 0.0F, 0.0F } });

        const entt::entity child = add_physics_entity(world, ph::BodyType::Static,
                                                      Vec3{ 1.0F, 0.0F, 0.0F });
        check(world.set_parent(child, parent), "the child attaches to the parent");

        ph::Simulation simulation;
        simulation.build(world);

        // The restriction covers dynamic bodies alone, because the entity owns
        // the transform of a static or kinematic one.
        check(simulation.has_body(child), "the parented static entity got a body");

        // And it went where the hierarchy put it, not where its local transform
        // alone would have. The body starts from the world matrix, so a build
        // that read the local transform would put it at x = 1.
        check(world.world_matrix(child)[3][0] == 4.0F, "the parent carried it to x = 4");
    }

    void a_step_alone_moves_nothing() {
        section("A step records the pose and interpolate writes it");

        sc::World world;
        const entt::entity crate = add_physics_entity(world, ph::BodyType::Dynamic,
                                                      Vec3{ 0.0F, 6.0F, 0.0F });

        ph::Simulation simulation;
        simulation.build(world);

        const float started = world.local(crate).position.y;
        for (std::uint32_t i = 0; i < 10; ++i) {
            simulation.step(world, kStep);
        }

        // The split is what lets one frame run several steps and draw once. A
        // step that wrote the entity would write it several times and still
        // draw a pose that ignores where the frame sits between two of them.
        check(world.local(crate).position.y == started, "ten steps left the entity alone");

        simulation.interpolate(world, 1.0F);
        check(world.local(crate).position.y < started, "and interpolate moved it");
    }

    void a_frame_between_two_steps_blends() {
        section("A frame between two steps draws between them");

        sc::World world;
        const entt::entity crate = add_physics_entity(world, ph::BodyType::Dynamic,
                                                      Vec3{ 0.0F, 6.0F, 0.0F });

        ph::Simulation simulation;
        simulation.build(world);

        // Two steps, so the pair the blend reads are both real poses the solver
        // produced rather than one pose and the place the scene started.
        simulation.step(world, kStep);
        simulation.interpolate(world, 1.0F);
        const float older = world.local(crate).position.y;

        simulation.step(world, kStep);
        simulation.interpolate(world, 0.0F);
        const float at_zero = world.local(crate).position.y;

        simulation.interpolate(world, 1.0F);
        const float newer = world.local(crate).position.y;

        simulation.interpolate(world, 0.5F);
        const float middle = world.local(crate).position.y;

        check(at_zero == older, "alpha 0 draws the older of the two steps");
        check(newer < older, "the body fell further over the second step");
        check(middle < older && middle > newer, "alpha 0.5 draws between them");

        // Halfway in the blend is halfway in distance, because the blend is a
        // straight line between two points and nothing else.
        const float halfway = (older + newer) * 0.5F;
        check(std::fabs(middle - halfway) < kBlendTolerance, "and it sits at the midpoint");
    }

    void interpolating_twice_gives_one_answer() {
        section("Interpolating twice with no step between gives one answer");

        sc::World world;
        const entt::entity crate = add_physics_entity(world, ph::BodyType::Dynamic,
                                                      Vec3{ 0.0F, 6.0F, 0.0F });

        ph::Simulation simulation;
        simulation.build(world);
        for (std::uint32_t i = 0; i < 5; ++i) {
            simulation.step(world, kStep);
        }

        simulation.interpolate(world, 0.5F);
        const engine::Transform once = world.local(crate);
        simulation.interpolate(world, 0.5F);
        const engine::Transform twice = world.local(crate);

        // The two recorded poses are the only input. Reading the entity back as
        // one of them would make each call blend against the last answer, and
        // then a frame that drew twice would creep forward on its own.
        check(once.position == twice.position, "the position did not move again");
        check(once.rotation == twice.rotation, "and neither did the rotation");
    }

    void a_blended_rotation_stays_a_rotation() {
        section("A blended rotation is still a rotation");

        sc::World world;
        const entt::entity crate = add_physics_entity(world, ph::BodyType::Dynamic,
                                                      Vec3{ 0.0F, 6.0F, 0.0F });
        // Off-axis, so the box tips as it falls and the two poses differ by a
        // real turn rather than by nothing.
        world.set_local(crate, engine::Transform{ .position = { 0.0F, 6.0F, 0.0F },
                                                  .rotation = glm::angleAxis(
                                                      kTiltRadians, glm::normalize(Vec3{
                                                                        1.0F, 0.0F, 1.0F })) });

        const entt::entity floor = add_physics_entity(world, ph::BodyType::Static,
                                                      Vec3{ 0.0F, -1.0F, 0.0F });
        world.registry().get<ph::BoxCollider>(floor).half_extents = Vec3{ 50.0F, 1.0F, 50.0F };

        ph::Simulation simulation;
        simulation.build(world);
        for (std::uint32_t i = 0; i < kStepCount; ++i) {
            simulation.step(world, kStep);
        }

        // A straight blend of two quaternions shortens as they part, and a
        // rotation matrix built from one that is not unit length scales the
        // mesh. slerp is what keeps the length at one.
        simulation.interpolate(world, 0.5F);
        const float length = glm::length(world.local(crate).rotation);
        check(std::fabs(length - 1.0F) < kBlendTolerance, "the blended quaternion is unit length");
    }

    /// A crate above a floor, which is the smallest scene that falls and lands.
    entt::entity drop_scene(sc::World& world, float height) {
        const entt::entity crate = add_physics_entity(world, ph::BodyType::Dynamic,
                                                      Vec3{ 0.0F, height, 0.0F });
        const entt::entity floor = add_physics_entity(world, ph::BodyType::Static,
                                                      Vec3{ 0.0F, -1.0F, 0.0F });
        world.registry().get<ph::BoxCollider>(floor).half_extents = Vec3{ 50.0F, 1.0F, 50.0F };
        return crate;
    }

    /**
     * Runs the frame loop the runtime runs, and records the drawn height.
     *
     * This is the whole of M7.4 joined up: a clock at one rate, a frame at
     * another, whole steps, and a blend for what is left over. It needs no GPU,
     * because the drawn pose is the entity transform and that is what the
     * renderer reads.
     */
    std::vector<float> drawn_heights(float step_hz, float frame_seconds, std::uint32_t frames) {
        sc::World world;
        const entt::entity crate = drop_scene(world, 6.0F);

        ph::Simulation simulation;
        simulation.build(world);

        engine::FixedTimestep clock(step_hz);
        std::vector<float> heights;
        heights.reserve(frames);

        for (std::uint32_t frame = 0; frame < frames; ++frame) {
            for (std::uint32_t left = clock.advance(frame_seconds); left > 0; --left) {
                simulation.step(world, clock.step_seconds());
            }
            simulation.interpolate(world, clock.alpha());
            heights.push_back(world.local(crate).position.y);
        }
        return heights;
    }

    void a_faster_display_than_the_step_rate_does_not_judder() {
        section("A frame rate that is not a multiple of the step rate is smooth");

        // 60 frames each second against 24 steps each second. That is 2.5 frames
        // for each step, so it never lines up and every kind of frame happens:
        // one that runs no step, one that runs one, and one that runs two.
        constexpr float kStepHz = 24.0F;
        constexpr float kFrameSeconds = 1.0F / 60.0F;
        constexpr std::uint32_t kFrames = 60;

        const std::vector<float> heights = drawn_heights(kStepHz, kFrameSeconds, kFrames);

        // The frames before the first step are not judder, and they have to be
        // skipped rather than counted. A step is 1/24 of a second and a frame is
        // 1/60, so the first step does not run until the third frame. Until then
        // the two recorded poses are both the pose the scene started in, and
        // every blend of them is that pose. The body has not moved, so drawing
        // it twice in the same place is right.
        std::size_t moving = 1;
        while (moving < heights.size() && heights[moving] == heights[0]) {
            ++moving;
        }
        check(moving < heights.size(), "the crate starts moving");

        // Judder is a pose drawn twice and then a jump. Without the blend the
        // height would hold for two or three frames at a time, and about 60
        // percent of the frames would repeat the one before.
        std::uint32_t repeated = 0;
        for (std::size_t i = moving + 1; i < heights.size(); ++i) {
            if (heights[i] == heights[i - 1]) {
                ++repeated;
            }
        }
        check(repeated == 0, "no moving frame draws the pose the frame before it drew");

        // And the motion is even. A jump after a repeat shows up as one step
        // much larger than the rest, so the largest is compared against the
        // middle one. Gravity makes each step a little larger than the last, so
        // this is a loose bound rather than a tight one.
        // From the same frame, because a leading zero delta would drag the
        // median down and make the bound below far looser than it reads.
        std::vector<float> deltas;
        deltas.reserve(heights.size());
        for (std::size_t i = moving + 1; i < heights.size(); ++i) {
            deltas.push_back(heights[i - 1] - heights[i]);
        }
        std::vector<float> sorted = deltas;
        std::sort(sorted.begin(), sorted.end());
        const float median = sorted[sorted.size() / 2];
        const float largest = sorted.back();

        check(median > 0.0F, "the crate is falling throughout");
        check(largest < median * 3.0F, "no frame moves it much further than the usual frame");
    }

    void a_crate_comes_to_rest() {
        section("A crate lands and stops");

        // Long enough to fall 5.5 metres and settle. Falling that far under the
        // Box3D gravity takes about a second, so this is well past it.
        const std::vector<float> heights = drawn_heights(60.0F, 1.0F / 60.0F, 240);

        check(heights.back() < 6.0F, "the crate fell");
        check(heights.back() > 0.0F, "the floor stopped it");

        // A solver that cannot settle a body leaves it shaking on the spot, and
        // that reads as a fault in the interpolation rather than in the solver.
        // The last half second is where it has to be still.
        float widest = 0.0F;
        for (std::size_t i = heights.size() - 30; i < heights.size(); ++i) {
            widest = std::max(widest, std::fabs(heights[i] - heights.back()));
        }
        check(widest < kRestTolerance, "and it is still over the last half second");
    }

    /// Builds a tower of crates on a floor, and hands back the crate entities.
    std::vector<entt::entity> stack_scene(sc::World& world, std::uint32_t height) {
        const entt::entity floor = add_physics_entity(world, ph::BodyType::Static,
                                                      Vec3{ 0.0F, -1.0F, 0.0F });
        world.registry().get<ph::BoxCollider>(floor).half_extents = Vec3{ 50.0F, 1.0F, 50.0F };

        std::vector<entt::entity> crates;
        for (std::uint32_t level = 0; level < height; ++level) {
            // A hair of a gap, so the stack starts apart and settles under
            // gravity rather than starting inside itself. A stack built already
            // overlapping resolves that overlap by pushing, which looks like an
            // explosion and is not what this measures.
            const float y = (static_cast<float>(level) * (kBoxHalfSize * 2.0F + kStackGap)) +
                            kBoxHalfSize;
            crates.push_back(
                add_physics_entity(world, ph::BodyType::Dynamic, Vec3{ 0.0F, y, 0.0F }));
        }
        return crates;
    }

    /// Steps a settled scene at the fixed rate and writes the drawn pose out.
    void settle(sc::World& world, ph::Simulation& simulation, std::uint32_t steps) {
        for (std::uint32_t i = 0; i < steps; ++i) {
            simulation.step(world, kStep);
        }
        simulation.interpolate(world, 1.0F);
    }

    void a_stack_stands_and_settles() {
        section("A stack of crates stands and comes to rest");

        sc::World world;
        const std::vector<entt::entity> crates = stack_scene(world, kStackHeight);

        ph::Simulation simulation;
        simulation.build(world);
        settle(world, simulation, kSettleSteps);

        // Each crate ends up about its own height above the one below. A stack
        // that sank into the floor, or exploded, fails this rather than needing
        // somebody to look at it.
        for (std::size_t level = 0; level < crates.size(); ++level) {
            const float expected = (static_cast<float>(level) * kBoxHalfSize * 2.0F) + kBoxHalfSize;
            const float actual = world.local(crates[level]).position.y;
            check(std::fabs(actual - expected) < kStackTolerance,
                  "the crate rests where the one below it puts it");
        }

        // And it is still. A stack that never settles keeps shivering, which
        // reads as a rendering fault and is a solver one.
        const float before = world.local(crates.back()).position.y;
        settle(world, simulation, 60);
        check(std::fabs(world.local(crates.back()).position.y - before) < kRestTolerance,
              "and it does not move over the second after that");
    }

    void something_thrown_knocks_the_stack_over() {
        section("Something thrown at the stack knocks it over");

        sc::World world;
        const std::vector<entt::entity> crates = stack_scene(world, kStackHeight);

        ph::Simulation simulation;
        simulation.build(world);
        settle(world, simulation, kSettleSteps);

        const float top_before = world.local(crates.back()).position.y;
        const float top_x_before = world.local(crates.back()).position.x;

        // The projectile joins a world that is already running. build() would
        // answer this by rebuilding every body, which puts the settled stack
        // back where the scene file put it and loses what this test is about.
        const entt::entity ball = world.create();
        world.set_local(ball, engine::Transform{ .position = kThrowFrom });
        world.registry().emplace<ph::RigidBody>(ball);
        world.registry().emplace<ph::SphereCollider>(ball, ph::SphereCollider{ .radius = 0.4F });

        check(simulation.add_body(world, ball), "the thrown body joins the running world");
        check(simulation.body_count() == crates.size() + 2, "and every other body is still there");
        check(simulation.set_linear_velocity(ball, Vec3{ kThrowSpeed, 0.0F, 0.0F }),
              "and it was given a velocity");

        settle(world, simulation, kSettleSteps);

        // The stack is a stack no longer. Either the top crate came down, or it
        // was carried sideways off the one below. Both are it falling over, and
        // demanding one of them in particular would be describing this throw
        // rather than the milestone.
        const float top_after = world.local(crates.back()).position.y;
        const float moved_sideways = std::fabs(world.local(crates.back()).position.x - top_x_before);
        check(top_after < top_before - kKnockedTolerance || moved_sideways > kKnockedTolerance,
              "the top crate is no longer where the stack left it");
    }

    void a_body_added_late_leaves_the_others_alone() {
        section("Adding one body does not disturb the bodies already there");

        sc::World world;
        const std::vector<entt::entity> crates = stack_scene(world, kStackHeight);

        ph::Simulation simulation;
        simulation.build(world);
        settle(world, simulation, kSettleSteps);

        // The whole pose rather than the height. A crate that slid sideways or
        // turned on the spot has been disturbed just as much as one that
        // dropped, and comparing the height alone would call that undisturbed.
        std::vector<engine::Transform> before;
        before.reserve(crates.size());
        for (const entt::entity crate : crates) {
            before.push_back(world.local(crate));
        }

        // Far enough away that it touches nothing. Anything that moves in the
        // stack moved because the add disturbed it, which is what build()
        // would do and what add_body() exists not to do.
        const entt::entity spare = world.create();
        world.set_local(spare, engine::Transform{ .position = { 20.0F, 0.5F, 0.0F } });
        world.registry().emplace<ph::RigidBody>(spare);
        world.registry().emplace<ph::BoxCollider>(spare);
        check(simulation.add_body(world, spare), "the spare body is added");

        settle(world, simulation, 1);

        bool unmoved = true;
        for (std::size_t i = 0; i < crates.size(); ++i) {
            const engine::Transform& now = world.local(crates[i]);
            unmoved = unmoved && glm::length(now.position - before[i].position) < kRestTolerance;

            // The absolute value of the dot product, because a quaternion and
            // its negation are the same rotation. Comparing the four numbers
            // one by one would report a sign flip as a body that spun round.
            const float alignment = std::fabs(glm::dot(now.rotation, before[i].rotation));
            unmoved = unmoved && std::fabs(alignment - 1.0F) < kRestTolerance;
        }
        check(unmoved, "every crate in the stack is where it was, and still faces the same way");

        check(!simulation.add_body(world, spare), "adding the same entity twice is refused");
        check(!simulation.set_linear_velocity(world.create(), Vec3{ 1.0F, 0.0F, 0.0F }),
              "an entity with no body cannot be given a velocity");
    }

} // namespace

int main() {
    // The physics world needs the scheduler, because Box3D runs its solver on it.
    engine::jobs::init();

    gravity_points_down_y();
    the_components_reflect();
    a_body_round_trips();
    a_prefab_instance_overrides_one_field();
    the_transforms_follow_the_solver();
    the_scale_survives_the_write_back();
    a_parented_dynamic_body_is_refused();
    a_parented_static_body_is_allowed();
    a_step_alone_moves_nothing();
    a_frame_between_two_steps_blends();
    interpolating_twice_gives_one_answer();
    a_blended_rotation_stays_a_rotation();
    a_faster_display_than_the_step_rate_does_not_judder();
    a_crate_comes_to_rest();
    a_stack_stands_and_settles();
    something_thrown_knocks_the_stack_over();
    a_body_added_late_leaves_the_others_alone();
    a_box_falls_and_lands();
    the_worker_count_matches_the_job_system();
    the_solver_runs_on_the_job_system();

    // Two sizes, either side of the crossover DESIGN.md section 5.1 records. The
    // small one is what the sandbox will hold, and the pool loses there. The
    // large one is where it wins. Both are printed, because a single number
    // would hide which side of the crossover the reader is on.
    test::section("What the job system is worth, by scene size");
    report_step_cost("sandbox size", kSmallWidth, kSmallHeight);
    report_step_cost("large stack ", kLargeWidth, kLargeHeight);

    engine::jobs::shutdown();
    return test::report();
}
