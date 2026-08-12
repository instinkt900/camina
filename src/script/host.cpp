#include "script/host.h"

#include "core/entt.h"
#include "core/log.h"
#include "scene/world.h"
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

    struct Host::Impl {
        sol::state lua;

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

    Host::Host()
        : impl_(std::make_unique<Impl>()) {
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
        const entt::registry& registry = world.registry();
        for (auto& [entity, instance] : impl_->instances) {
            if (registry.valid(entity)) {
                impl_->call(instance, entity, Callback::Destroy, instance.on_destroy);
            }
        }
        impl_->instances.clear();
        impl_->stopped = 0;
    }

    std::size_t Host::instance_count() const { return impl_->instances.size(); }

    std::size_t Host::stopped_count() const { return impl_->stopped; }

    std::size_t Host::call_count(Callback callback) const {
        return impl_->counts.at(index_of(callback));
    }

} // namespace engine::script
