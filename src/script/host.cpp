#include "script/host.h"

#include "core/entt.h"
#include "core/log.h"
#include "scene/world.h"
#include "script/bindings.h"
#include "script/components.h"

#include <sol/sol.hpp>

#include <array>
#include <string>
#include <unordered_map>
#include <utility>

namespace engine::script {

    namespace {

        /// How many values Callback names, so the counters are one array.
        constexpr std::size_t kCallbackCount = 3;

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

        /// The component registry the host was built with, or nullptr.
        [[nodiscard]] const scene::ComponentOps* ops_of(const EntityHandle& self,
                                                        const std::string& component) {
            if (self.context == nullptr || self.context->components == nullptr) {
                return nullptr;
            }
            return self.context->components->find(component);
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
            sol::environment env;
            sol::protected_function on_update;
            sol::protected_function on_destroy;
            /// An error stopped this one. It is never called again.
            bool stopped = false;
        };

        std::unordered_map<Guid, Script> scripts;
        std::unordered_map<entt::entity, Instance> instances;
        std::array<std::size_t, kCallbackCount> counts{};
        std::size_t stopped = 0;

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

        bind_entity();
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
            "get", &read_component, "set", &write_component);
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

        impl_->scripts.insert_or_assign(
            script, Impl::Script{ std::string{ name }, std::string{ text } });
        return true;
    }

    bool Host::loaded(Guid script) const { return impl_->scripts.contains(script); }

    void Host::update(scene::World& world, double seconds) {
        // The world arrives on each call, so every handle reads it from here
        // rather than from whatever world made the instance. A scene reload
        // that builds a new world would otherwise leave every running instance
        // pointing at the old one.
        impl_->context.world = &world;

        entt::registry& registry = world.registry();

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

            const sol::protected_function on_start = instance.env[name_of(Callback::Start)];
            impl_->call(instance, entity, Callback::Start, on_start);

            impl_->instances.emplace(entity, std::move(instance));
        }

        // Drop what is gone. A reload recycles entity numbers, so an instance
        // that kept its number would attach to whatever took it.
        for (auto it = impl_->instances.begin(); it != impl_->instances.end();) {
            const entt::entity entity = it->first;
            if (registry.valid(entity) && registry.all_of<ScriptComponent>(entity)) {
                ++it;
                continue;
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

    void Host::stop(scene::World& world) {
        // on_destroy may read the entity, so the handles have to name this
        // world and not the one the last step used.
        impl_->context.world = &world;

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
    }

    std::size_t Host::instance_count() const { return impl_->instances.size(); }

    std::size_t Host::stopped_count() const { return impl_->stopped; }

    std::size_t Host::call_count(Callback callback) const {
        return impl_->counts.at(index_of(callback));
    }

} // namespace engine::script
