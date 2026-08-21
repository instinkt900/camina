#include "script/host.h"

#include "core/entt.h"
#include "core/log.h"
#include "physics/simulation.h"
#include "platform/input.h"
#include "scene/components.h"
#include "scene/prefab.h"
#include "scene/step_motion.h"
#include "scene/world.h"
#include "script/bindings.h"
#include "script/components.h"

#include <sol/sol.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace engine::script {

    namespace {

        /// How many values Callback names, so the counters are one array.
        ///
        /// `counts` is indexed with `.at`, so a value added to the enum without
        /// this number moving throws rather than writing past the array.
        constexpr std::size_t kCallbackCount = 7;

        [[nodiscard]] std::size_t index_of(Callback callback) {
            return static_cast<std::size_t>(callback);
        }

        /// The name a script declares for each callback.
        [[nodiscard]] const char* name_of(Callback callback) {
            switch (callback) {
            case Callback::Start:
                return "on_start";
            case Callback::Update:
                return "on_update";
            case Callback::Destroy:
                return "on_destroy";
            case Callback::Trigger:
                return "on_trigger";
            case Callback::Contact:
                return "on_contact";
            case Callback::Press:
                return "on_ui_press";
            case Callback::Reload:
                return "on_ui_reload";
            }
            return "";
        }

    } // namespace

    namespace {

        /// A Lua table holding a vector, so a script writes `p.x` and not `p[1]`.
        [[nodiscard]] sol::table vector_table(sol::state_view lua, const reflect::Value& value,
                                              int components) {
            sol::table out = lua.create_table();
            const std::array<const char*, 4> keys{ "x", "y", "z", "w" };
            for (int i = 0; i < components; ++i) {
                out[keys.at(static_cast<std::size_t>(i))] = value.vector[i];
            }
            return out;
        }

        /// Turns one field value into what a script sees.
        [[nodiscard]] sol::object to_lua(sol::state_view lua, const reflect::Value& value) {
            switch (value.kind) {
            case reflect::ValueKind::Bool:
                return sol::make_object(lua, value.boolean);
            case reflect::ValueKind::Number:
                return sol::make_object(lua, value.number);
            case reflect::ValueKind::Text:
            case reflect::ValueKind::Enum:
                // An enum reaches a script as its name. A number would say
                // nothing a person can read and would change meaning the moment
                // somebody inserted an enumerator above it.
                return sol::make_object(lua, value.text);
            case reflect::ValueKind::Vec3:
                // The engine type, so `p + v` works and the value crosses back
                // to a field with no conversion. See script/bindings.h.
                return sol::make_object(
                    lua, Vec3{ value.vector.x, value.vector.y, value.vector.z });
            case reflect::ValueKind::Quat:
                return sol::make_object(lua, value.quat);
            case reflect::ValueKind::Vec2:
                // No component carries either of these, so they stay tables
                // rather than earning a usertype nobody asked for. Rule 4.6.
                return vector_table(lua, value, 2);
            case reflect::ValueKind::Vec4:
                return vector_table(lua, value, 4);
            case reflect::ValueKind::None:
            case reflect::ValueKind::Unsupported:
                break;
            }
            return sol::lua_nil;
        }

        /// Reads one component of a vector table, keeping what is already there.
        [[nodiscard]] float component_or(const sol::table& table, const char* key,
                                         float fallback) {
            const sol::optional<float> value = table[key];
            return value.value_or(fallback);
        }

        /**
         * Turns what a script wrote into a field value of the kind @p wanted.
         *
         * The kind comes from reading the field first rather than from guessing
         * at the Lua value. A table of three numbers could be a Vec3, a Vec4 or
         * a quaternion, and only the field knows which.
         *
         * A table that names some of the components keeps the rest, so a script
         * can write `{ y = 2 }` and move one axis.
         */
        [[nodiscard]] bool from_lua(const sol::object& from, const reflect::Value& current,
                                    reflect::Value& out) {
            out = current;

            switch (current.kind) {
            case reflect::ValueKind::Bool:
                if (!from.is<bool>()) {
                    return false;
                }
                out.boolean = from.as<bool>();
                return true;
            case reflect::ValueKind::Number:
                if (!from.is<double>()) {
                    return false;
                }
                out.number = from.as<double>();
                return true;
            case reflect::ValueKind::Text:
            case reflect::ValueKind::Enum:
                if (!from.is<std::string>()) {
                    return false;
                }
                out.text = from.as<std::string>();
                return true;
            case reflect::ValueKind::Vec2:
            case reflect::ValueKind::Vec3:
            case reflect::ValueKind::Vec4: {
                // A vec3 the script built or read back, which is the usual way.
                if (from.is<Vec3>()) {
                    const Vec3 value = from.as<Vec3>();
                    out.vector.x = value.x;
                    out.vector.y = value.y;
                    out.vector.z = value.z;
                    return true;
                }
                // A table naming some of the components, which keeps the rest.
                // Both spellings work, because writing one axis is worth the
                // second branch.
                if (!from.is<sol::table>()) {
                    return false;
                }
                const sol::table table = from.as<sol::table>();
                out.vector.x = component_or(table, "x", current.vector.x);
                out.vector.y = component_or(table, "y", current.vector.y);
                out.vector.z = component_or(table, "z", current.vector.z);
                out.vector.w = component_or(table, "w", current.vector.w);
                return true;
            }
            case reflect::ValueKind::Quat: {
                if (from.is<Quat>()) {
                    out.quat = from.as<Quat>();
                    return true;
                }
                if (!from.is<sol::table>()) {
                    return false;
                }
                const sol::table table = from.as<sol::table>();
                out.quat.w = component_or(table, "w", current.quat.w);
                out.quat.x = component_or(table, "x", current.quat.x);
                out.quat.y = component_or(table, "y", current.quat.y);
                out.quat.z = component_or(table, "z", current.quat.z);
                return true;
            }
            case reflect::ValueKind::None:
            case reflect::ValueKind::Unsupported:
                break;
            }
            return false;
        }

        /**
         * What `ui.find` gives a script back.
         *
         * Two strings and nothing else. It names a node rather than pointing at
         * one, so a hot reload that frees the whole node tree leaves every
         * handle a script is holding still correct. `DESIGN.md` section 8.4 has
         * that trap three times over, and this is the shape that cannot repeat
         * it.
         */
        struct UiNodeHandle {
            std::string layout; ///< The source path of the layout.
            std::string node;   ///< The id of the node inside it.
        };

    } // namespace

    /**
     * What the bindings need that is not the entity itself.
     *
     * The host owns one of these and rewrites the world at the top of every
     * update(). An EntityHandle points at it rather than holding a world of its
     * own, so a handle cannot outlive the world it was built against.
     *
     * That matters because the world arrives on each call. A caller may pass a
     * different one, and an instance lives across steps. A handle that captured
     * the world when it was made would then read a world nobody is stepping,
     * and a script that stored `entity` in a table would keep that stale
     * pointer for as long as it kept the table.
     */
    struct ScriptContext {
        scene::World* world = nullptr;
        const scene::ComponentRegistry* components = nullptr;
        Services services;

        /**
         * Entities already told that the solver owns their pose.
         *
         * A script that writes a Transform to a dynamic body usually does it
         * on every step, so this is said once rather than sixty times a
         * second. update() drops an entity the world no longer holds, because
         * EnTT hands the same number out again after a reload.
         */
        mutable std::unordered_set<entt::entity> warned_solver_owns;
    };

    /**
     * What a script means by `entity`.
     *
     * One of these is bound into each instance's environment, naming the entity
     * the script runs on. The entity number is the whole value it carries.
     */
    struct EntityHandle {
        entt::entity id = entt::null;
        const ScriptContext* context = nullptr;
    };

    namespace {

        /**
         * The world this step is running against, or nullptr.
         *
         * Null when a script kept an `entity` past the step that made it, which
         * is the one way a handle outlives its world. Every caller checks.
         */
        [[nodiscard]] scene::World* world_of(const EntityHandle& self) {
            return self.context == nullptr ? nullptr : self.context->world;
        }

        /// The simulation this step is running against, or nullptr.
        [[nodiscard]] physics::Simulation* physics_of(const EntityHandle& self) {
            return self.context == nullptr ? nullptr : self.context->services.physics;
        }

        /// The component registry the host was built with, or nullptr.
        [[nodiscard]] const scene::ComponentOps* ops_of(const EntityHandle& self,
                                                        const std::string& component) {
            if (self.context == nullptr || self.context->components == nullptr) {
                return nullptr;
            }
            return self.context->components->find(component);
        }

        /// The entity a script names, or entt::null when the world is gone.
        [[nodiscard]] EntityHandle handle_for(const EntityHandle& self, entt::entity id) {
            return EntityHandle{ id, self.context };
        }

        /**
         * The direct children of an entity, as a Lua sequence.
         *
         * A sequence and not a keyed table on purpose. `ipairs` walks the array
         * part in order, where `pairs` over string keys does not, and that
         * order is the difference between a reproducible run and one that
         * fails once in ten. See @ref script_determinism.
         */
        [[nodiscard]] sol::object children_of(const EntityHandle& self, sol::this_state state) {
            sol::state_view lua{ state };
            scene::World* world = world_of(self);
            if (world == nullptr) {
                return sol::lua_nil;
            }

            sol::table out = lua.create_table();
            const entt::registry& registry = world->registry();
            if (!registry.valid(self.id)) {
                return sol::object{ out };
            }

            const auto* hierarchy = registry.try_get<scene::Hierarchy>(self.id);
            if (hierarchy == nullptr) {
                return sol::object{ out };
            }

            // The list is doubly linked inside the entities, so this walks it
            // rather than asking for a container nobody stores.
            int index = 1;
            for (entt::entity child = hierarchy->first_child; child != entt::null;) {
                out[index++] = handle_for(self, child);
                const auto* link = registry.try_get<scene::Hierarchy>(child);
                child = link == nullptr ? entt::null : link->next_sibling;
            }
            return sol::object{ out };
        }

        /**
         * Reads every described field of one component into a Lua table.
         *
         * A field type no Value carries is left out rather than given a wrong
         * value, so a script sees nil and can say so. See issue #271.
         */
        [[nodiscard]] sol::object read_component(const EntityHandle& self,
                                                 const std::string& component,
                                                 sol::this_state state) {
            sol::state_view lua{ state };
            scene::World* world = world_of(self);
            const scene::ComponentOps* ops = ops_of(self, component);
            if (world == nullptr || ops == nullptr || ops->const_instance == nullptr) {
                return sol::lua_nil;
            }
            const void* instance = ops->const_instance(world->registry(), self.id);
            if (instance == nullptr) {
                return sol::lua_nil;
            }

            sol::table out = lua.create_table();
            for (const char* field : ops->field_names) {
                reflect::Value value;
                if (ops->get_field(instance, field, value)) {
                    out[field] = to_lua(lua, value);
                }
            }
            return sol::object{ out };
        }

        /**
         * Whether the solver owns this entity's pose.
         *
         * A dynamic body integrates its own pose, and step() writes the result
         * onto the entity. So a Transform written from a script is overwritten
         * on the next step, and `Simulation::teleport` is the verb that moves
         * one. A static or a kinematic body is the other way round: the entity
         * owns the pose and step() reads it, so writing the Transform is right
         * for those two.
         */
        [[nodiscard]] bool solver_owns_pose(const EntityHandle& self, const scene::World& world) {
            const physics::Simulation* simulation = physics_of(self);
            if (simulation == nullptr || !simulation->has_body(self.id)) {
                return false;
            }
            const auto* body = world.registry().try_get<physics::RigidBody>(self.id);
            return body != nullptr && body->type == physics::BodyType::Dynamic;
        }

        /// Says once, for one entity, that a Transform written there is lost.
        void warn_solver_owns_pose(const EntityHandle& self) {
            if (self.context == nullptr ||
                !self.context->warned_solver_owns.insert(self.id).second) {
                return;
            }
            ENGINE_LOG_WARN("Entity {} has a dynamic body, so the solver owns its pose. The "
                            "Transform written here is thrown away by the next step. Use "
                            "entity:teleport() to move it, or make the body kinematic.",
                            static_cast<std::uint32_t>(entt::to_integral(self.id)));
        }

        /**
         * Writes the fields the table names and leaves the rest alone.
         *
         * A script can move one axis without reading the whole component back.
         */
        [[nodiscard]] bool write_component(const EntityHandle& self,
                                           const std::string& component,
                                           const sol::table& fields) {
            scene::World* world = world_of(self);
            const scene::ComponentOps* ops = ops_of(self, component);
            if (world == nullptr || ops == nullptr || ops->instance == nullptr) {
                return false;
            }
            void* instance = ops->instance(world->registry(), self.id);
            if (instance == nullptr) {
                return false;
            }

            bool wrote = false;
            for (const auto& entry : fields) {
                if (!entry.first.template is<std::string>()) {
                    continue;
                }
                const std::string field = entry.first.template as<std::string>();

                // Read first, to learn the kind. A table of three numbers could
                // be a Vec3, a Vec4 or a quaternion, and only the field knows.
                reflect::Value current;
                if (!ops->get_field(instance, field, current)) {
                    continue;
                }
                reflect::Value wanted;
                if (from_lua(entry.second, current, wanted) &&
                    ops->set_field(instance, field, wanted)) {
                    wrote = true;
                }
            }

            // A Transform written this way went around World::set_local(), so
            // the hierarchy does not know. Without this the entity draws where
            // it was and every child stays behind it.
            if (wrote && ops->owns_transform) {
                world->mark_dirty(self.id);

                if (solver_owns_pose(self, *world)) {
                    // Recording it here is worse than doing nothing. Issue
                    // #284: begin_step() puts a recorded pose back at the top
                    // of every step, so the body integrates and never moves,
                    // while its velocity reads back correctly the whole time.
                    warn_solver_owns_pose(self);
                } else if (self.context->services.motion != nullptr) {
                    // The step owns the pose now, so a frame between two steps
                    // blends it rather than showing the newest one. Without
                    // this a script that turns something steps it at 60 Hz and
                    // holds it still in between, which is the judder a fixed
                    // step exists to remove. owns_transform rather than a name
                    // compare, because the registry already records which
                    // component this is.
                    self.context->services.motion->record(*world, self.id);
                }
            }
            return wrote;
        }

    } // namespace

    struct Host::Impl {
        sol::state lua;

        /// What every EntityHandle points at. update() rewrites the world.
        ScriptContext context;

        /**
         * One script, held under the identity a component names.
         *
         * The text is kept rather than a compiled chunk, and that is not an
         * oversight. **A Lua closure captures the `_ENV` of its parent as a
         * shared upvalue cell, not as a copy.** So loading once and pointing
         * that one chunk at a different environment for each entity rewrites
         * the environment of every closure already made from it, and all the
         * entities end up sharing one table.
         *
         * Loading the text again for each instance gives each one its own chunk
         * object and its own `_ENV`. That costs one parse for each entity, which
         * is nothing next to a step, and it is the thing that makes two crates
         * running one script keep separate state.
         */
        struct Script {
            std::string name; ///< The source path, for the log and a traceback.
            std::string text; ///< The source, loaded again for each instance.

            /**
             * Counts up on each reload, and never on a plain load.
             *
             * An instance records the value it was built from. The two
             * disagreeing is what says the instance is running text nobody
             * holds any more, and the sync in update() then drops it the same
             * way it drops an instance whose entity named a different script.
             *
             * A counter rather than a flag on the script, because the flag
             * would have to be cleared and there is no one moment to clear it
             * in: several entities share one script and each is restarted on
             * the step its own sync reaches it.
             */
            std::uint64_t generation = 0;
        };

        /**
         * One entity's copy of a script.
         *
         * The environment is what keeps two entities running one script apart.
         * Each gets a fresh table whose fallback is the globals, so a script
         * that writes a variable writes it into its own copy.
         */
        struct Instance {
            Guid script;
            /// The Script::generation this was built from. See that field.
            std::uint64_t generation = 0;
            sol::environment env;
            sol::protected_function on_update;
            sol::protected_function on_destroy;
            sol::protected_function on_trigger;
            sol::protected_function on_contact;
            sol::protected_function on_ui_press;
            sol::protected_function on_ui_reload;
            /// An error stopped this one. It is never called again.
            bool stopped = false;
        };

        std::unordered_map<Guid, Script> scripts;
        std::unordered_map<entt::entity, Instance> instances;
        std::array<std::size_t, kCallbackCount> counts{};
        std::size_t stopped = 0;
        std::size_t restarted = 0;

        /**
         * Reports one Lua error, and stops the instance that raised it.
         *
         * Stopping is the whole point. An error in on_update would otherwise
         * repeat sixty times each second, fill the log, and cost the frame
         * every time. One message names the entity, the script and the call.
         */
        void fail(Instance& instance, entt::entity entity, Callback callback,
                  const sol::protected_function_result& result) {
            if (!instance.stopped) {
                const sol::error error = result;
                const auto script = scripts.find(instance.script);
                const std::string_view name =
                    script == scripts.end() ? std::string_view{ "?" }
                                            : std::string_view{ script->second.name };
                ENGINE_LOG_ERROR("{} raised an error in {} on entity {}. {} "
                                 "The script is stopped on that entity.",
                                 name, name_of(callback),
                                 static_cast<std::uint32_t>(entt::to_integral(entity)),
                                 error.what());
                instance.stopped = true;
                ++stopped;
            }
        }

        /// Calls one callback, counts it, and stops the instance on an error.
        void call(Instance& instance, entt::entity entity, Callback callback,
                  const sol::protected_function& fn) {
            if (instance.stopped || !fn.valid()) {
                return;
            }
            const sol::protected_function_result result = fn();
            ++counts.at(index_of(callback));
            if (!result.valid()) {
                fail(instance, entity, callback, result);
            }
        }

        /// The update overload, which is the only callback that takes a value.
        void call_update(Instance& instance, entt::entity entity, double seconds) {
            if (instance.stopped || !instance.on_update.valid()) {
                return;
            }
            const sol::protected_function_result result = instance.on_update(seconds);
            ++counts.at(index_of(Callback::Update));
            if (!result.valid()) {
                fail(instance, entity, Callback::Update, result);
            }
        }

        /**
         * The overload for the two physics callbacks, which take the same pair.
         *
         * @p other is handed over as a handle rather than as a number, so a
         * script reads its name and its components the same way it reads its
         * own entity. The handle may name an entity that has gone, because Box3D
         * reports the end of an overlap when a shape is destroyed, and
         * `other:valid()` is what a script asks about that.
         */
        void call_touch(Instance& instance, entt::entity entity, Callback callback,
                        const sol::protected_function& fn, EntityHandle other, bool began) {
            if (instance.stopped || !fn.valid()) {
                return;
            }
            const sol::protected_function_result result = fn(other, began);
            ++counts.at(index_of(callback));
            if (!result.valid()) {
                fail(instance, entity, callback, result);
            }
        }

        /**
         * Runs `on_ui_press` on one instance.
         *
         * Two strings rather than a handle, because a press names a node and a
         * node is not an entity. A script compares them against what it showed.
         */
        void call_press(Instance& instance, entt::entity entity, const UiPress& press) {
            if (instance.stopped || !instance.on_ui_press.valid()) {
                return;
            }
            const sol::protected_function_result result =
                instance.on_ui_press(press.layout, press.node);
            ++counts.at(index_of(Callback::Press));
            if (!result.valid()) {
                fail(instance, entity, Callback::Press, result);
            }
        }

        /**
         * The instances that declare one callback, in entity order.
         *
         * The instances live in a hash map, whose walk order is neither creation
         * order nor stable across an insert. Two scripts that both answer one
         * event would then run in an order nothing promises, and a reproducible
         * run rests on there being none of that. See `DESIGN.md` section 9.
         *
         * @param hook Which callback to gather the listeners of.
         * @return The entities, sorted.
         */
        [[nodiscard]] std::vector<entt::entity>
        listeners(sol::protected_function Instance::* hook) const {
            std::vector<entt::entity> found;
            found.reserve(instances.size());
            for (const auto& [entity, instance] : instances) {
                if (!instance.stopped && (instance.*hook).valid()) {
                    found.push_back(entity);
                }
            }
            std::sort(found.begin(), found.end());
            return found;
        }

        /**
         * Runs `on_ui_reload` on one instance.
         *
         * One string, because a rebuilt layout is named the way everything else
         * in the `ui` table names one. What the script does with it is write its
         * own values back, and it already knows which those are.
         */
        void call_reload(Instance& instance, entt::entity entity, const std::string& layout) {
            if (instance.stopped || !instance.on_ui_reload.valid()) {
                return;
            }
            const sol::protected_function_result result = instance.on_ui_reload(layout);
            ++counts.at(index_of(Callback::Reload));
            if (!result.valid()) {
                fail(instance, entity, Callback::Reload, result);
            }
        }
    };

    Host::Host(const scene::ComponentRegistry& components)
        : impl_(std::make_unique<Impl>()) {
        impl_->context.components = &components;
        // Deliberately not the whole standard library. `io` and `os` reach the
        // file system and the clock, and a script that reads either one breaks
        // the reproducible run that DESIGN.md section 9 rests on.
        //
        // `math` is open, and `math.random` seeded from the clock is the other
        // half of that problem. Issue #262 owns the seeding decision, along
        // with what to do about the walk order of `pairs`.
        impl_->lua.open_libraries(sol::lib::base, sol::lib::string, sol::lib::table,
                                  sol::lib::math);

        // The whole binding surface at M8.1. Issue #261 adds component access
        // and #262 adds the engine API.
        sol::table log = impl_->lua.create_named_table("log");
        log.set_function("info", [](const std::string& message) {
            ENGINE_LOG_INFO("[lua] {}", message);
        });
        log.set_function("warn", [](const std::string& message) {
            ENGINE_LOG_WARN("[lua] {}", message);
        });
        log.set_function("error", [](const std::string& message) {
            ENGINE_LOG_ERROR("[lua] {}", message);
        });

        // The curated half. M8.2 gave a script every component field, and this
        // gives it the types and the guarantees. See script/bindings.h.
        bind_math(impl_->lua);
        bind_random(impl_->lua);

        // Each one named here rather than chained out of the last, so dropping
        // one is visible in this list. bind_entity_physics() is the exception,
        // because it adds to the type bind_entity() has to have made first.
        bind_entity();
        bind_world();
        bind_input();
        bind_camera();
        bind_ui();
        bind_game();
    }

    /**
     * Binds what a script can do to the entity it runs on.
     *
     * Every component reaches Lua through the reflection descriptors, so a
     * component the game defined works with no engine code naming it. That is
     * the half of the surface nobody hand-writes. The curated half is #262.
     */
    void Host::bind_entity() {
        impl_->lua.new_usertype<EntityHandle>(
            "Entity", sol::no_constructor,

            // The entity number, so a script can tell two apart and use one as
            // a table key.
            "id",
            sol::readonly_property([](const EntityHandle& self) {
                return static_cast<std::uint32_t>(entt::to_integral(self.id));
            }),

            "has",
            [](const EntityHandle& self, const std::string& component) {
                const scene::World* world = world_of(self);
                const scene::ComponentOps* ops = ops_of(self, component);
                return world != nullptr && ops != nullptr &&
                       ops->has(world->registry(), self.id);
            },

            // A table of every field the component describes. A field type no
            // Value carries is left out rather than given a wrong value, so a
            // script sees nil and can say so. See issue #271.
            //
            // "set" writes a Transform like any other component, and that is
            // the wrong verb for a dynamic body. The solver owns that pose and
            // throws the write away on the next step, so write_component warns
            // and points at teleport(). See issue #284.
            "get", &read_component, "set", &write_component,

            // The hierarchy. A sequence, so ipairs walks it in order.
            "children", &children_of,

            "parent",
            [](const EntityHandle& self) -> sol::optional<EntityHandle> {
                const scene::World* world = world_of(self);
                if (world == nullptr || !world->registry().valid(self.id)) {
                    return sol::nullopt;
                }
                const auto* hierarchy = world->registry().try_get<scene::Hierarchy>(self.id);
                if (hierarchy == nullptr || hierarchy->parent == entt::null) {
                    return sol::nullopt;
                }
                return handle_for(self, hierarchy->parent);
            },

            "name",
            [](const EntityHandle& self) -> sol::optional<std::string> {
                const scene::World* world = world_of(self);
                if (world == nullptr || !world->registry().valid(self.id)) {
                    return sol::nullopt;
                }
                const auto* named = world->registry().try_get<scene::Name>(self.id);
                return named == nullptr ? sol::nullopt : sol::optional<std::string>{ named->value };
            },

            "valid",
            [](const EntityHandle& self) {
                const scene::World* world = world_of(self);
                return world != nullptr && world->registry().valid(self.id);
            },

            // Where the entity ended up after the hierarchy composed, which is
            // not the local pose a script writes through get and set.
            "world_position", [](const EntityHandle& self) -> sol::optional<Vec3> {
                const scene::World* world = world_of(self);
                if (world == nullptr || !world->registry().valid(self.id)) {
                    return sol::nullopt;
                }
                const Mat4& matrix = world->world_matrix(self.id);
                return Vec3{ matrix[3][0], matrix[3][1], matrix[3][2] }; });

        bind_entity_physics();
    }

    /**
     * Adds the physics verbs to the type bind_entity() made.
     *
     * sol lets a usertype take members after it is created, so this is the same
     * `entity` a script sees and not a second type. The split is only because
     * the two halves together are past the cognitive complexity clang-tidy
     * allows one function, and the seam between reading a scene and driving a
     * solver is a real one.
     *
     * Each verb answers nil or false when the entity has no body, or when no
     * simulation was passed this step.
     */
    void Host::bind_entity_physics() {
        sol::usertype<EntityHandle> entity = impl_->lua["Entity"];

        entity["velocity"] = [](const EntityHandle& self) -> sol::optional<Vec3> {
            physics::Simulation* physics = physics_of(self);
            Vec3 velocity{ 0.0F, 0.0F, 0.0F };
            if (physics == nullptr || !physics->linear_velocity(self.id, velocity)) {
                return sol::nullopt;
            }
            return velocity;
        };

        entity["set_velocity"] = [](const EntityHandle& self, const Vec3& velocity) {
            physics::Simulation* physics = physics_of(self);
            return physics != nullptr && physics->set_linear_velocity(self.id, velocity);
        };

        // A change of momentum rather than a speed, so a heavy body moves less
        // for the same push. It wakes a sleeping body, because one that dropped
        // the push would report nothing.
        entity["impulse"] = [](const EntityHandle& self, const Vec3& impulse) {
            physics::Simulation* physics = physics_of(self);
            return physics != nullptr && physics->apply_linear_impulse(self.id, impulse);
        };

        entity["is_awake"] = [](const EntityHandle& self) {
            physics::Simulation* physics = physics_of(self);
            return physics != nullptr && physics->is_awake(self.id);
        };

        entity["wake"] = [](const EntityHandle& self) {
            physics::Simulation* physics = physics_of(self);
            return physics != nullptr && physics->set_awake(self.id, true);
        };

        // Writing the Transform does not do this. A dynamic body owns its pose
        // and the next step overwrites whatever an entity carries, so a reset
        // that set the component would put every crate back for one step and
        // then lose them all again.
        entity["teleport"] = [](const EntityHandle& self, const Vec3& position,
                                const sol::optional<Quat>& rotation) {
            physics::Simulation* physics = physics_of(self);
            if (physics == nullptr) {
                return false;
            }
            // Upright when the caller gives no rotation, which is what a reset
            // wants and is one fewer thing to spell out.
            const scene::World* world = world_of(self);
            if (world == nullptr) {
                return false;
            }
            return physics->teleport(*world, self.id, position,
                                     rotation.value_or(Quat{ 1.0F, 0.0F, 0.0F, 0.0F }));
        };

        // An entity a script just instanced carries the collider components and
        // no body, because the simulation reads the world once at build and does
        // not scan for new ones each step. Without this a thrown crate hangs in
        // the air.
        entity["add_body"] = [](const EntityHandle& self) {
            scene::World* world = world_of(self);
            physics::Simulation* physics = physics_of(self);
            return world != nullptr && physics != nullptr && physics->add_body(*world, self.id);
        };

        entity["has_body"] = [](const EntityHandle& self) {
            const physics::Simulation* physics = physics_of(self);
            return physics != nullptr && physics->has_body(self.id);
        };
    }

    /**
     * Binds where the view is, which the throw reads.
     *
     * A free table rather than a method on `entity`, because the camera belongs
     * to no entity. It is the application's today, so the pose arrives in the
     * services for the step. Both read nil when nobody passed one, which is what
     * a test that binds no camera gets.
     */
    void Host::bind_camera() {
        const ScriptContext* context = &impl_->context;

        sol::table camera = impl_->lua.create_named_table("camera");

        camera.set_function("position", [context]() -> sol::optional<Vec3> {
            if (context->services.camera == nullptr) {
                return sol::nullopt;
            }
            return context->services.camera->position;
        });

        camera.set_function("forward", [context]() -> sol::optional<Vec3> {
            if (context->services.camera == nullptr) {
                return sol::nullopt;
            }
            return context->services.camera->forward;
        });
    }

    /**
     * Binds the `ui` table and the node handle it gives back.
     *
     * The whole surface goes through `script::UiSurface`, because `src/script/`
     * is in `engine_core` and `DESIGN.md` section 8.5 keeps moth_ui out of it.
     * A build with no game UI passes no surface, and then every call here
     * answers false rather than failing.
     *
     * **A handle holds two strings and never a node.** `ui.find` reads better
     * than four flat calls that each repeat the layout and the node, and it is
     * sugar over exactly that: each method looks the node up again through the
     * surface of the current step. A handle that cached a node would be holding
     * freed memory the first time somebody saved the layout.
     */
    void Host::bind_ui() {
        const ScriptContext* context = &impl_->context;

        // Reads the surface of the current step rather than one kept from the
        // step that made the handle. See Services.
        const auto surface = [context]() -> UiSurface* { return context->services.ui; };

        impl_->lua.new_usertype<UiNodeHandle>(
            "ui_node", sol::no_constructor,

            // A property with a lambda rather than sol::readonly on the member
            // itself. Every other usertype in this engine is bound this way,
            // and the member form is the one construct sol2 could not compile
            // here: `upvalue_this_member_variable::call` carries a computed
            // noexcept, and its address then does not match `lua_CFunction`.
            // It built on this machine and on MSVC and failed on the Linux CI
            // runner, which is the worst way for it to fail.
            "layout",
            sol::readonly_property([](const UiNodeHandle& self) { return self.layout; }),
            "node", sol::readonly_property([](const UiNodeHandle& self) { return self.node; }),

            "text",
            [surface](const UiNodeHandle& self) -> std::string {
                UiSurface* ui = surface();
                return ui == nullptr ? std::string{} : ui->text(self.layout, self.node);
            },
            "set_text",
            [surface](const UiNodeHandle& self, const std::string& text) {
                UiSurface* ui = surface();
                return ui != nullptr && ui->set_text(self.layout, self.node, text);
            },
            "visible",
            [surface](const UiNodeHandle& self) {
                UiSurface* ui = surface();
                return ui != nullptr && ui->node_visible(self.layout, self.node);
            },
            "set_visible",
            [surface](const UiNodeHandle& self, bool visible) {
                UiSurface* ui = surface();
                return ui != nullptr && ui->set_node_visible(self.layout, self.node, visible);
            },
            "set_image", [surface](const UiNodeHandle& self, const std::string& image) {
                UiSurface* ui = surface();
                return ui != nullptr && ui->set_image(self.layout, self.node, image); });

        sol::table ui = impl_->lua.create_named_table("ui");

        ui.set_function("show", [surface](const std::string& layout) {
            UiSurface* value = surface();
            return value != nullptr && value->show(layout);
        });
        ui.set_function("hide", [surface](const std::string& layout) {
            UiSurface* value = surface();
            return value != nullptr && value->hide(layout);
        });
        ui.set_function("visible", [surface](const std::string& layout) {
            UiSurface* value = surface();
            return value != nullptr && value->visible(layout);
        });

        // nil rather than a handle that answers nothing, so a script can write
        // `if node then` and a typo in a node id reports itself at the find
        // rather than five calls later.
        ui.set_function("find", [surface](const std::string& layout, const std::string& node) -> sol::optional<UiNodeHandle> {
            UiSurface* value = surface();
            if (value == nullptr || !value->has_node(layout, node)) {
                return sol::nullopt;
            }
            return UiNodeHandle{ layout, node };
        });
    }

    Host::~Host() = default;

    bool Host::load(Guid script, std::string_view name, std::span<const std::byte> source) {
        const std::string_view text{ reinterpret_cast<const char*>(source.data()),
                                     source.size() };

        // Compiled here only to find a syntax error at load, where the message
        // belongs. The result is thrown away, because each instance loads the
        // text again for its own environment. See Impl::Script.
        //
        // load rather than script, so a syntax error is a value to report and
        // not something thrown through the caller.
        const sol::load_result compiled = impl_->lua.load(text, std::string{ name });
        if (!compiled.valid()) {
            const sol::error error = compiled;
            ENGINE_LOG_ERROR("{} will not compile. {}", name, error.what());
            return false;
        }

        // The generation is kept across a plain load, so loading the same text
        // twice at startup does not look like a reload to the instances.
        const auto held = impl_->scripts.find(script);
        const std::uint64_t generation =
            held == impl_->scripts.end() ? 0 : held->second.generation;

        impl_->scripts.insert_or_assign(
            script, Impl::Script{ std::string{ name }, std::string{ text }, generation });
        return true;
    }

    bool Host::reload(Guid script, std::string_view name, std::span<const std::byte> source) {
        // The text is compiled before anything is replaced. A save in the middle
        // of an edit must leave the running game alone, and load() reports the
        // file and the line when it will not compile.
        const auto held = impl_->scripts.find(script);
        const std::uint64_t before = held == impl_->scripts.end() ? 0 : held->second.generation;

        if (!load(script, name, source)) {
            return false;
        }

        // Every instance built from the old text now disagrees with the script,
        // and the sync in update() drops each one and starts it again. That is
        // where on_destroy and on_start run, because both need the world and the
        // services of a step and this call has neither.
        impl_->scripts.at(script).generation = before + 1;
        return true;
    }

    bool Host::loaded(Guid script) const { return impl_->scripts.contains(script); }

    /**
     * Binds the verbs that belong to the scene rather than to one entity.
     *
     * A free table rather than methods on `entity`, because none of these is
     * about the entity a script happens to run on.
     */
    void Host::bind_world() {
        const ScriptContext* context = &impl_->context;

        sol::table world = impl_->lua.create_named_table("world");

        // The first entity carrying this name. A scene is authored by a person,
        // so two entities with one name is a mistake rather than a shape to
        // support, and taking the first is the readable answer.
        world.set_function("find", [context](const std::string& name) -> sol::optional<EntityHandle> {
            if (context->world == nullptr) {
                return sol::nullopt;
            }
            for (const auto [entity, named] :
                 context->world->registry().view<const scene::Name>().each()) {
                if (named.value == name) {
                    return EntityHandle{ entity, context };
                }
            }
            return sol::nullopt;
        });

        world.set_function("create", [context]() -> sol::optional<EntityHandle> {
            if (context->world == nullptr) {
                return sol::nullopt;
            }
            return EntityHandle{ context->world->create(), context };
        });

        world.set_function("destroy", [context](const EntityHandle& entity) {
            if (context->world == nullptr || !context->world->registry().valid(entity.id)) {
                return false;
            }
            // World::destroy takes the children with it, which is what a script
            // means by destroying a thing that has parts.
            context->world->destroy(entity.id);
            return true;
        });

        // What the crate throw does. The prefab name is the source path, the
        // same one a scene file writes.
        world.set_function("instance", [context](const std::string& name) -> sol::optional<EntityHandle> {
            if (context->world == nullptr) {
                ENGINE_LOG_ERROR("A script asked for prefab {} and no world is being "
                                 "stepped.",
                                 name);
                return sol::nullopt;
            }
            if (context->services.prefabs == nullptr) {
                ENGINE_LOG_ERROR("A script asked for prefab {} and this step has no prefab "
                                 "library.",
                                 name);
                return sol::nullopt;
            }
            const scene::Prefab* prefab = context->services.prefabs->find(name);
            if (prefab == nullptr) {
                ENGINE_LOG_ERROR("A script asked for prefab {}, which is not loaded.", name);
                return sol::nullopt;
            }
            const entt::entity entity = scene::instantiate(
                *context->world, *prefab, nlohmann::json::object(), *context->components);
            if (entity == entt::null) {
                return sol::nullopt;
            }
            return EntityHandle{ entity, context };
        });
    }

    /**
     * Binds the `game` table, which is the fixed step a script may hold.
     *
     * `pause` and `resume` rather than one call taking a boolean, because a
     * menu reads better for it and neither has anything to get the wrong way
     * round. `paused` is what a script asks before it decides which menu to
     * put up.
     *
     * A step with no clock answers false and pauses nothing, the same way an
     * action with no input module reads false.
     */
    void Host::bind_game() {
        const ScriptContext* context = &impl_->context;

        sol::table game = impl_->lua.create_named_table("game");

        // Each returns whether it reached a clock, so a script can report a
        // build that has none rather than pausing silently into nothing.
        game.set_function("pause", [context] {
            if (context->services.clock == nullptr) {
                return false;
            }
            context->services.clock->set_paused(true);
            return true;
        });
        game.set_function("resume", [context] {
            if (context->services.clock == nullptr) {
                return false;
            }
            context->services.clock->set_paused(false);
            return true;
        });
        game.set_function("paused", [context] {
            return context->services.clock != nullptr && context->services.clock->paused();
        });
    }

    /**
     * Binds the input module by action name.
     *
     * A script never names an SDL constant, which is what issue #207 asked for.
     * A step with no input module reads every action as false, so an offscreen
     * run needs no special case in a script.
     */
    void Host::bind_input() {
        const ScriptContext* context = &impl_->context;

        sol::table input = impl_->lua.create_named_table("input");

        input.set_function("held", [context](const std::string& action) {
            return context->services.input != nullptr && context->services.input->held(action);
        });
        input.set_function("pressed", [context](const std::string& action) {
            return context->services.input != nullptr &&
                   context->services.input->pressed(action);
        });
        input.set_function("released", [context](const std::string& action) {
            return context->services.input != nullptr &&
                   context->services.input->released(action);
        });
    }

    void Host::update(scene::World& world, double seconds, const Services& services) {
        // Everything that can change between steps arrives here rather than
        // being captured when an instance was made. See issue #273.
        impl_->context.services = services;

        // The world arrives on each call, so every handle reads it from here
        // rather than from whatever world made the instance. A scene reload
        // that builds a new world would otherwise leave every running instance
        // pointing at the old one.
        impl_->context.world = &world;

        entt::registry& registry = world.registry();

        // A reload builds a new world and EnTT hands the same entity numbers
        // out again, so a number left here would silence the warning for
        // whoever holds that number next.
        std::erase_if(impl_->context.warned_solver_owns,
                      [&registry](entt::entity entity) { return !registry.valid(entity); });

        // Start what is new. An entity that names a script nobody loaded gets
        // one message and no instance, so the next step does not repeat it.
        for (const auto [entity, component] : registry.view<const ScriptComponent>().each()) {
            if (impl_->instances.contains(entity)) {
                continue;
            }

            const auto script = impl_->scripts.find(component.script);
            if (script == impl_->scripts.end()) {
                ENGINE_LOG_ERROR("Entity {} names script {}, which is not loaded. "
                                 "It gets no instance.",
                                 static_cast<std::uint32_t>(entt::to_integral(entity)),
                                 component.script.to_text());
                // A stopped instance with no callbacks, so this reports once
                // rather than on every step.
                Impl::Instance dead;
                dead.script = component.script;
                dead.stopped = true;
                impl_->instances.emplace(entity, std::move(dead));
                ++impl_->stopped;
                continue;
            }

            // A fresh table for each entity, falling back to the globals. Two
            // crates running one script keep separate state this way.
            Impl::Instance instance;
            instance.script = component.script;
            instance.generation = script->second.generation;
            instance.env = sol::environment(impl_->lua, sol::create, impl_->lua.globals());

            // The entity the script runs on. It goes in the environment rather
            // than the globals, because each instance names a different one.
            instance.env["entity"] = EntityHandle{ entity, &impl_->context };

            // Loaded again for this instance, so the chunk carries an `_ENV`
            // upvalue of its own. Reusing one chunk would point every closure
            // already built from it at whichever environment was set last. See
            // Impl::Script. load() already proved the text compiles.
            sol::load_result loaded = impl_->lua.load(script->second.text, script->second.name);
            sol::protected_function body = loaded;
            sol::set_environment(instance.env, body);

            const sol::protected_function_result ran = body();
            if (!ran.valid()) {
                impl_->fail(instance, entity, Callback::Start, ran);
                impl_->instances.emplace(entity, std::move(instance));
                continue;
            }

            // Looked up once, so a script with no on_update costs nothing on a
            // step rather than a table lookup that misses.
            instance.on_update = instance.env[name_of(Callback::Update)];
            instance.on_destroy = instance.env[name_of(Callback::Destroy)];
            instance.on_trigger = instance.env[name_of(Callback::Trigger)];
            instance.on_contact = instance.env[name_of(Callback::Contact)];
            instance.on_ui_press = instance.env[name_of(Callback::Press)];
            instance.on_ui_reload = instance.env[name_of(Callback::Reload)];

            const sol::protected_function on_start = instance.env[name_of(Callback::Start)];
            impl_->call(instance, entity, Callback::Start, on_start);

            impl_->instances.emplace(entity, std::move(instance));
        }

        // Drop what is gone. A reload recycles entity numbers, so an instance
        // that kept its number would attach to whatever took it.
        for (auto it = impl_->instances.begin(); it != impl_->instances.end();) {
            const entt::entity entity = it->first;
            const auto* component =
                registry.valid(entity) ? registry.try_get<const ScriptComponent>(entity) : nullptr;

            // An entity that named a different script is not the same instance
            // any more. Keeping the old one would leave the entity running a
            // script it no longer names, and nothing would say so. The start
            // pass above gives it a new instance on the next step.
            //
            // A reload counts as a different script for the same reason. The
            // instance is running text the host no longer holds, so it is torn
            // down and built again, which is what makes a reload a restart. Both
            // arrive here, so there is one path that runs on_destroy and one
            // that runs on_start, rather than a second copy for reloading.
            const auto held = component == nullptr ? impl_->scripts.end()
                                                   : impl_->scripts.find(component->script);
            const bool named_the_same =
                component != nullptr && component->script == it->second.script;
            // A script the host does not hold counts as current, and that is
            // load-bearing. An entity naming a script nobody loaded is given a
            // stopped instance with no callbacks, so that the error is reported
            // once rather than on every step. Treating an absent script as stale
            // would drop that marker and build it again each step, and the one
            // message would become sixty each second.
            const bool current = held == impl_->scripts.end() ||
                                 held->second.generation == it->second.generation;

            if (named_the_same && current) {
                ++it;
                continue;
            }

            // A restart is the entity keeping the script it named and losing the
            // instance anyway, which only a reload does. An entity that changed
            // script or went away is not one.
            if (named_the_same) {
                ++impl_->restarted;
            }
            if (registry.valid(entity)) {
                impl_->call(it->second, entity, Callback::Destroy, it->second.on_destroy);
            }
            if (it->second.stopped) {
                --impl_->stopped;
            }
            it = impl_->instances.erase(it);
        }

        for (auto& [entity, instance] : impl_->instances) {
            impl_->call_update(instance, entity, seconds);
        }
    }

    void Host::deliver_physics_events(scene::World& world, const physics::Simulation& simulation,
                                      const Services& services) {
        // The world and the services of this call rather than the ones the last
        // update() left, for the reason Services records: a reload builds a new
        // simulation, and a callback that pushed a body through a stale handle
        // would reach the one nobody is stepping.
        impl_->context.world = &world;
        impl_->context.services = services;

        const entt::registry& registry = world.registry();

        // The instance is looked up rather than walked, because most events name
        // an entity with no script. A crate landing on a floor is two entities
        // and normally no callback at all.
        const auto deliver = [&](entt::entity self, entt::entity other, Callback callback,
                                 bool began) {
            if (self == entt::null || !registry.valid(self)) {
                return;
            }
            const auto found = impl_->instances.find(self);
            if (found == impl_->instances.end()) {
                return;
            }
            Impl::Instance& instance = found->second;
            const sol::protected_function& fn =
                callback == Callback::Trigger ? instance.on_trigger : instance.on_contact;
            impl_->call_touch(instance, self, callback, fn,
                              EntityHandle{ other, &impl_->context }, began);
        };

        // The trigger only. The volume is what the event is about, and DESIGN.md
        // section 10 M8 wants the goal to ask what landed in it rather than every
        // crate to know what a goal is.
        for (const physics::Simulation::Touch& touch : simulation.trigger_events()) {
            deliver(touch.a, touch.b, Callback::Trigger, touch.began);
        }

        // Both sides. Box3D promises no order here, so a one-sided call would
        // reach whichever body the solver happened to list first, and which
        // script heard a collision would depend on solver internals.
        for (const physics::Simulation::Touch& touch : simulation.contact_events()) {
            deliver(touch.a, touch.b, Callback::Contact, touch.began);
            deliver(touch.b, touch.a, Callback::Contact, touch.began);
        }
    }

    void Host::deliver_ui_events(scene::World& world, const Services& services) {
        // The world and the services of this call, for the reason
        // deliver_physics_events gives.
        impl_->context.world = &world;
        impl_->context.services = services;

        if (services.ui == nullptr) {
            return;
        }

        const std::span<const std::string> reloads = services.ui->reloads();
        const std::span<const UiPress> presses = services.ui->presses();
        if (reloads.empty() && presses.empty()) {
            return;
        }

        // The reloads first. A rebuilt layout carries the text its file carries,
        // so a script writes its own values back before it acts on a press of
        // the same batch. The other order would let a press act on a menu that
        // still reads whatever the file says.
        // Gathered once each rather than for every event, because the set
        // cannot change while these loops run: a callback may not build or drop
        // an instance.
        const std::vector<entt::entity> on_reload =
            reloads.empty() ? std::vector<entt::entity>{}
                            : impl_->listeners(&Impl::Instance::on_ui_reload);
        const std::vector<entt::entity> on_press =
            presses.empty() ? std::vector<entt::entity>{}
                            : impl_->listeners(&Impl::Instance::on_ui_press);

        for (const std::string& layout : reloads) {
            for (const entt::entity entity : on_reload) {
                const auto found = impl_->instances.find(entity);
                if (found != impl_->instances.end()) {
                    impl_->call_reload(found->second, entity, layout);
                }
            }
        }

        for (const UiPress& press : presses) {
            for (const entt::entity entity : on_press) {
                const auto found = impl_->instances.find(entity);
                if (found != impl_->instances.end()) {
                    impl_->call_press(found->second, entity, press);
                }
            }
        }

        // One event is delivered once. The surface gathers them on the frame
        // clock, so leaving them would replay every one on every later step.
        services.ui->clear_reloads();
        services.ui->clear_presses();
    }

    void Host::stop(scene::World& world, const Services& services) {
        // on_destroy may read the entity, so the handles have to name this
        // world and not the one the last step used.
        impl_->context.world = &world;

        // And the services of this call rather than the ones the last step
        // left. Those point at a simulation the caller is usually tearing down
        // alongside the world, and a teardown that pushed a body through one of
        // them would reach freed memory. The default is nothing.
        impl_->context.services = services;

        const entt::registry& registry = world.registry();
        for (auto& [entity, instance] : impl_->instances) {
            if (registry.valid(entity)) {
                impl_->call(instance, entity, Callback::Destroy, instance.on_destroy);
            }
        }
        impl_->instances.clear();
        impl_->stopped = 0;

        // Nothing is being stepped now. A script that kept an `entity` in a
        // table still holds a live handle, and this is what makes that handle
        // report nothing rather than read a world the caller may have dropped.
        impl_->context.world = nullptr;
        impl_->context.services = Services{};
    }

    std::size_t Host::instance_count() const { return impl_->instances.size(); }

    std::size_t Host::stopped_count() const { return impl_->stopped; }

    std::size_t Host::restart_count() const { return impl_->restarted; }

    std::size_t Host::call_count(Callback callback) const {
        return impl_->counts.at(index_of(callback));
    }

} // namespace engine::script
