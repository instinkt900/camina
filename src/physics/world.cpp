#include "physics/world.h"

#include "core/assert.h"
#include "core/jobs.h"
#include "core/log.h"
#include "core/profile.h"

#include <box3d/box3d.h>
#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstddef>
#include <vector>

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

        /// How many segments make one circle of a sphere wireframe. Twelve reads
        /// as round at the size a collider is normally looked at, and it keeps a
        /// sphere to 36 lines across its three rings.
        constexpr int kCircleSegments = 12;

        /// The sRGB transfer exponent. The exact curve has a linear toe near
        /// zero, and this is the approximation everything uses for a color that
        /// somebody picked by eye rather than measured.
        constexpr float kSrgbGamma = 2.2F;

        /**
         * The wireframe of one shape, in the local space of its body.
         *
         * Box3D calls for this once, the first time it draws a shape, and hands
         * the pointer back on every draw after that. So the edges of a hull are
         * walked once rather than once for each frame.
         *
         * The points are pairs. Points 0 and 1 are a line, 2 and 3 are the next.
         */
        struct Wireframe {
            std::vector<Vec3> points;
        };

        /// Turns a Box3D debug color into the linear working space of section 3.
        [[nodiscard]] Vec3 from_box3d(b3HexColor color) {
            constexpr float kByte = 255.0F;
            const auto value = static_cast<std::uint32_t>(color);
            const Vec3 encoded{ static_cast<float>((value >> 16U) & 0xFFU) / kByte,
                                static_cast<float>((value >> 8U) & 0xFFU) / kByte,
                                static_cast<float>(value & 0xFFU) / kByte };

            // Box3D names its colors the way CSS does, so they are sRGB. The
            // engine works in linear light and converts at the write, so a color
            // handed over encoded arrives at the display twice encoded and far
            // too bright. See DESIGN.md section 3.
            return glm::pow(encoded, Vec3{ kSrgbGamma, kSrgbGamma, kSrgbGamma });
        }

        /// Appends one circle around `axis`, in the local space of the shape.
        void add_circle(std::vector<Vec3>& points, const Vec3& center, float radius,
                        const Vec3& first, const Vec3& second) {
            for (int i = 0; i < kCircleSegments; ++i) {
                const auto angle = [](int step) {
                    return glm::two_pi<float>() * static_cast<float>(step) /
                           static_cast<float>(kCircleSegments);
                };
                const float from = angle(i);
                const float to = angle(i + 1);
                points.push_back(center + (first * std::cos(from) * radius) +
                                 (second * std::sin(from) * radius));
                points.push_back(center + (first * std::cos(to) * radius) +
                                 (second * std::sin(to) * radius));
            }
        }

        /**
         * Walks the half-edges of a hull and writes each edge once.
         *
         * A hull stores half-edges, so every edge is in the array twice, once
         * from each end. Emitting both would draw the whole wireframe twice.
         * Taking only the half whose index is below its twin's is what picks
         * exactly one of each pair.
         *
         * The arrays are reached through byte offsets from the struct itself
         * rather than by pointer members, which is how Box3D keeps a hull one
         * contiguous block it can hash and share.
         */
        void add_hull(std::vector<Vec3>& points, const b3HullData* hull) {
            const auto* base = reinterpret_cast<const std::byte*>(hull);
            const auto* vertices = reinterpret_cast<const b3Vec3*>(base + hull->pointOffset);
            const auto* edges = reinterpret_cast<const b3HullHalfEdge*>(base + hull->edgeOffset);

            for (int i = 0; i < hull->edgeCount; ++i) {
                const b3HullHalfEdge& edge = edges[i];
                if (i >= static_cast<int>(edge.twin)) {
                    continue;
                }
                const b3Vec3 from = vertices[edge.origin];
                const b3Vec3 to = vertices[edges[edge.twin].origin];
                points.push_back(Vec3{ from.x, from.y, from.z });
                points.push_back(Vec3{ to.x, to.y, to.z });
            }
        }

        /// Builds the wireframe Box3D will hand back on every later draw.
        void* create_debug_shape(const b3DebugShape* shape, void* context) {
            (void)context;
            // Raw new, because Box3D owns this pointer from here and hands it to
            // destroy_debug_shape when the shape changes or goes away. That is
            // exactly the lifetime, and no smart pointer can cross a void*.
            auto* frame = new Wireframe{};

            switch (shape->type) {
            case b3_hullShape:
                add_hull(frame->points, shape->hull);
                break;
            case b3_sphereShape: {
                const Vec3 center{ shape->sphere->center.x, shape->sphere->center.y,
                                   shape->sphere->center.z };
                const float radius = shape->sphere->radius;
                add_circle(frame->points, center, radius, Vec3{ 1, 0, 0 }, Vec3{ 0, 1, 0 });
                add_circle(frame->points, center, radius, Vec3{ 0, 1, 0 }, Vec3{ 0, 0, 1 });
                add_circle(frame->points, center, radius, Vec3{ 1, 0, 0 }, Vec3{ 0, 0, 1 });
                break;
            }
            default:
                // A capsule, a mesh, a height field or a compound. Nothing adds
                // one today, and an empty wireframe draws nothing rather than
                // drawing something wrong.
                ENGINE_LOG_WARN("Physics debug draw has no wireframe for shape type {}, so that "
                                "collider draws nothing.",
                                static_cast<int>(shape->type));
                break;
            }
            return frame;
        }

        void destroy_debug_shape(void* user_shape, void* context) {
            (void)context;
            delete static_cast<Wireframe*>(user_shape);
        }

        /// Where the lines land while Box3D walks the world.
        struct LineSink {
            std::vector<DebugLine>* out = nullptr;
        };

        /// Puts one shape's cached wireframe into world space.
        void draw_shape(void* user_shape, b3WorldTransform transform, b3HexColor color,
                        void* context) {
            const auto* frame = static_cast<const Wireframe*>(user_shape);
            auto* sink = static_cast<LineSink*>(context);
            if (frame == nullptr || sink == nullptr) {
                return;
            }

            const Vec3 origin{ transform.p.x, transform.p.y, transform.p.z };
            const Quat rotation = from_box3d(transform.q);
            const Vec3 linear = from_box3d(color);

            for (std::size_t i = 0; i + 1 < frame->points.size(); i += 2) {
                sink->out->push_back(DebugLine{
                    .from = origin + (rotation * frame->points[i]),
                    .to = origin + (rotation * frame->points[i + 1]),
                    .color = linear });
            }
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

        // Box3D asks for the wireframe of a shape once and hands the pointer
        // back on every draw. Registering the pair here rather than at the first
        // draw is what lets it destroy them when a shape changes, which is the
        // only thing that knows a cached wireframe went stale.
        def.createDebugShape = create_debug_shape;
        def.destroyDebugShape = destroy_debug_shape;
        def.userDebugShapeContext = nullptr;

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

    void World::debug_lines(std::vector<DebugLine>& out) const {
        ENGINE_PROFILE_ZONE_N("physics debug lines");

        out.clear();

        LineSink sink{ .out = &out };
        b3DebugDraw draw = b3DefaultDebugDraw();
        draw.DrawShapeFcn = draw_shape;
        draw.drawShapes = true;
        draw.context = &sink;

        // Every mask bit, so a shape that was given a category of its own is
        // still reported. A collider nobody can see is the thing this exists to
        // find, and filtering it out here would hide exactly that.
        b3World_Draw(unpack_world(m_world), &draw, UINT64_MAX);
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
