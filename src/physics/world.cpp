#include "physics/world.h"

#include "core/assert.h"
#include "core/jobs.h"
#include "core/log.h"
#include "core/profile.h"

#include <box3d/box3d.h>

#include <algorithm>
#include <atomic>
#include <bit>

namespace engine::physics {

    namespace {

        /// Box3D clamps the worker count to this. See B3_MAX_WORKERS in
        /// box3d/constants.h. The clamp is silent, so the world reports back what
        /// it actually got rather than what it asked for.
        constexpr std::uint32_t kMaxWorkers = 32;

        /// Counts what Box3D handed over. See tasks_enqueued() in the header for
        /// why the count is worth keeping. Relaxed order is enough, because
        /// nothing reads it to decide anything.
        std::atomic<std::uint64_t> g_tasks_enqueued{ 0 };

        /**
         * Hands one Box3D task to the engine job system.
         *
         * A b3TaskCallback is void(void*), which is what jobs::TaskFn already is,
         * so the function pointer passes straight through with no trampoline and
         * no allocation.
         *
         * Returning null tells Box3D the work ran here and needs no finish call.
         * jobs::enqueue() returns null under exactly that condition, so the two
         * contracts line up with no translation.
         */
        void* enqueue_task(b3TaskCallback* task, void* task_context, void* user_context,
                           const char* name) {
            (void)user_context;
            g_tasks_enqueued.fetch_add(1, std::memory_order_relaxed);
            return jobs::enqueue(task, task_context, name);
        }

        /**
         * Waits for one Box3D task.
         *
         * Box3D requires this to block until the task has finished, because the
         * solver reads what the task wrote as soon as this returns. jobs::wait()
         * blocks, and it runs other pending tasks while it does, which is what
         * stops a step from stalling behind its own work.
         */
        void finish_task(void* user_task, void* user_context) {
            (void)user_context;
            jobs::wait(static_cast<jobs::Task*>(user_task));
        }

        /// Turns a Box3D world id into the integer World stores, so the header
        /// names no Box3D type.
        [[nodiscard]] std::uint64_t pack(b3WorldId id) {
            return std::bit_cast<std::uint32_t>(id);
        }

        [[nodiscard]] b3WorldId unpack_world(std::uint64_t id) {
            return std::bit_cast<b3WorldId>(static_cast<std::uint32_t>(id));
        }

        [[nodiscard]] BodyId pack(b3BodyId id) {
            return std::bit_cast<BodyId>(id);
        }

        [[nodiscard]] b3BodyId unpack_body(BodyId id) {
            return std::bit_cast<b3BodyId>(id);
        }

        [[nodiscard]] b3Vec3 to_box3d(const Vec3& v) {
            return b3Vec3{ v.x, v.y, v.z };
        }

        /**
         * Converts a quaternion field by field, and never by copying the bytes.
         *
         * A b3Quat holds the vector part first and the scalar last. GLM holds w
         * first, because GLM_FORCE_QUAT_DATA_WXYZ says so and DESIGN.md section 3
         * settles it. So the two have the same four floats in a different order,
         * and a memcpy between them turns every rotation into a different one.
         */
        [[nodiscard]] b3Quat to_box3d(const Quat& q) {
            return b3Quat{ .v = b3Vec3{ q.x, q.y, q.z }, .s = q.w };
        }

        [[nodiscard]] Quat from_box3d(const b3Quat& q) {
            // glm::quat takes w first in its constructor whatever its storage is.
            return Quat{ q.s, q.v.x, q.v.y, q.v.z };
        }

        [[nodiscard]] b3BodyType to_box3d(BodyType type) {
            switch (type) {
            case BodyType::Static:
                return b3_staticBody;
            case BodyType::Kinematic:
                return b3_kinematicBody;
            case BodyType::Dynamic:
                break;
            }
            return b3_dynamicBody;
        }

        [[nodiscard]] b3ShapeDef to_box3d(const SurfaceMaterial& material) {
            b3ShapeDef def = b3DefaultShapeDef();
            def.density = material.density;
            def.baseMaterial.friction = material.friction;
            def.baseMaterial.restitution = material.restitution;
            return def;
        }

    } // namespace

    std::uint64_t tasks_enqueued() {
        return g_tasks_enqueued.load(std::memory_order_relaxed);
    }

    World::World(std::uint32_t worker_count) {
        ENGINE_CHECK(jobs::worker_count() > 0, "Call jobs::init before building a physics world.");

        // jobs::worker_count() counts the calling thread, and Box3D counts the
        // same way: the thread inside step() enters the solver as a worker rather
        // than waiting for one. So the two numbers mean the same thing and pass
        // across with no adjustment.
        const std::uint32_t requested = worker_count != 0 ? worker_count : jobs::worker_count();
        m_worker_count = std::clamp(requested, 1U, kMaxWorkers);

        b3WorldDef def = b3DefaultWorldDef();
        def.workerCount = m_worker_count;
        def.enqueueTask = enqueue_task;
        def.finishTask = finish_task;

        // Nothing here needs a context. The job system is one global, and the
        // callbacks reach it by name. Box3D still passes the pointer back, so a
        // later world that needs its own scheduler can fill this in.
        def.userTaskContext = nullptr;

        m_world = pack(b3CreateWorld(&def));

        ENGINE_LOG_INFO("Physics world started on {} job system workers.", m_worker_count);
    }

    World::~World() {
        if (m_world != 0) {
            b3DestroyWorld(unpack_world(m_world));
        }
    }

    // The world id is a value this object hands back to Box3D, so nothing here
    // writes a member and clang-tidy offers to make the method const. Stepping is
    // the one call that changes everything the world holds, and a const step()
    // would say the opposite to every reader.
    // NOLINTNEXTLINE(readability-make-member-function-const)
    void World::step(float delta_seconds, std::uint32_t substeps) {
        ENGINE_PROFILE_ZONE_N("physics step");
        b3World_Step(unpack_world(m_world), delta_seconds, static_cast<int>(substeps));
    }

    BodyId World::add_body(BodyType type, const Vec3& position, const Quat& rotation) {
        b3BodyDef body_def = b3DefaultBodyDef();
        body_def.type = to_box3d(type);
        body_def.position = to_box3d(position);
        body_def.rotation = to_box3d(rotation);

        ++m_body_count;
        return pack(b3CreateBody(unpack_world(m_world), &body_def));
    }

    // Adding a shape reads no member, and clang-tidy offers to make these static
    // for the reason body_position() carries a note about. They stay methods
    // because a shape belongs to a body in one world.
    // NOLINTNEXTLINE(readability-convert-member-functions-to-static)
    void World::add_box(BodyId body, const Vec3& half_extents, const SurfaceMaterial& material) {
        // The world clones the hull into its own database, so this stack copy can
        // go out of scope. b3AddHullToDatabase is where it happens.
        const b3BoxHull hull = b3MakeBoxHull(half_extents.x, half_extents.y, half_extents.z);
        const b3ShapeDef shape_def = to_box3d(material);
        b3CreateHullShape(unpack_body(body), &shape_def, &hull.base);
    }

    // NOLINTNEXTLINE(readability-convert-member-functions-to-static)
    void World::add_sphere(BodyId body, float radius, const SurfaceMaterial& material) {
        const b3Sphere sphere{ .center = b3Vec3{ 0.0F, 0.0F, 0.0F }, .radius = radius };
        const b3ShapeDef shape_def = to_box3d(material);
        b3CreateSphereShape(unpack_body(body), &shape_def, &sphere);
    }

    // NOLINTNEXTLINE(readability-convert-member-functions-to-static)
    void World::set_body_transform(BodyId body, const Vec3& position, const Quat& rotation) {
        b3Body_SetTransform(unpack_body(body), to_box3d(position), to_box3d(rotation));
    }

    // NOLINTNEXTLINE(readability-convert-member-functions-to-static)
    void World::set_body_target(BodyId body, const Vec3& position, const Quat& rotation,
                                float delta_seconds) {
        const b3WorldTransform target{ .p = to_box3d(position), .q = to_box3d(rotation) };

        // Waking it is the point. A lift that stopped for a while has gone to
        // sleep, and a sleeping body carries nothing that rests on it.
        b3Body_SetTargetTransform(unpack_body(body), target, delta_seconds, true);
    }

    void World::destroy_body(BodyId body) {
        b3DestroyBody(unpack_body(body));
        if (m_body_count > 0) {
            --m_body_count;
        }
    }

    // A Box3D body id carries its own world, so reading a position needs no
    // member and clang-tidy offers to make this static. It stays a method because
    // a body belongs to one world, and a static call would invite reading a body
    // through a world that does not hold it.
    // NOLINTNEXTLINE(readability-convert-member-functions-to-static)
    void World::set_linear_velocity(BodyId body, const Vec3& velocity) {
        // Box3D wakes the body itself when the velocity is not zero. See
        // b3Body_SetLinearVelocity in third_party/box3d/src/body.c, which calls
        // b3WakeBodyWithLock before it writes. Waking it here as well would
        // cost a settled stack its sleep every time something asked it to stop.
        b3Body_SetLinearVelocity(unpack_body(body), to_box3d(velocity));
    }

    // NOLINTNEXTLINE(readability-convert-member-functions-to-static)
    Vec3 World::linear_velocity(BodyId body) const {
        const b3Vec3 velocity = b3Body_GetLinearVelocity(unpack_body(body));
        return Vec3{ velocity.x, velocity.y, velocity.z };
    }

    // NOLINTNEXTLINE(readability-convert-member-functions-to-static)
    Vec3 World::body_position(BodyId body) const {
        const b3Pos position = b3Body_GetPosition(unpack_body(body));
        return Vec3{ position.x, position.y, position.z };
    }

    // NOLINTNEXTLINE(readability-convert-member-functions-to-static)
    Quat World::body_rotation(BodyId body) const {
        return from_box3d(b3Body_GetRotation(unpack_body(body)));
    }

    std::uint32_t World::body_count() const {
        return m_body_count;
    }

    std::uint32_t World::worker_count() const {
        return m_worker_count;
    }

} // namespace engine::physics
