#pragma once

/**
 * @file
 * @brief The Lua interpreter, and the lifecycle it runs on each entity.
 *
 * One Host owns one `lua_State`. It holds the chunk for each loaded script and
 * one instance for each entity that carries a `ScriptComponent`.
 *
 * **This header names no sol2 type and no Lua type.** The state sits behind an
 * opaque pointer, the same way `platform/window.h` keeps SDL out. So Lua and
 * sol2 stay a PRIVATE link of `engine_core`, and nothing above this file needs
 * their include directories.
 *
 * The host reads no asset. The caller resolves a GUID through
 * `assets::Content` and passes the bytes to load(), which keeps the interpreter
 * free of the asset layer and lets a test drive it with a string literal.
 *
 * @code
 * engine::script::Host host;
 * host.load(guid, "scripts/spin.lua", bytes);
 *
 * // Once for each fixed step, before the solver runs.
 * host.update(world, simulated_seconds);
 * @endcode
 */

#include "core/guid.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>

#include "scene/component_registry.h"

namespace engine::scene {
    class World;
}

namespace engine::script {

    /**
     * @brief The three calls a script may declare.
     *
     * A script declares what it needs and leaves out the rest. A script with no
     * `on_update` costs nothing each step, because the host looks the function
     * up once when the instance starts.
     */
    enum class Callback : std::uint8_t {
        Start,   ///< `on_start()`, once, when the entity gets its instance.
        Update,  ///< `on_update(seconds)`, once for each fixed step.
        Destroy, ///< `on_destroy()`, once, when the entity loses its instance.
    };

    /**
     * @brief Owns the interpreter and runs the scripts an entity names.
     *
     * A Host owns a live `lua_State`, so it cannot be copied or moved.
     *
     * @warning **update() must run on the fixed step and not once for each
     * frame.** The seconds it passes on are simulated seconds, which is what
     * makes a run reproducible. See DESIGN.md section 9 and issue #245.
     */
    class Host {
    public:
        /**
         * @brief Builds a host that reads components through @p components.
         *
         * The registry is what turns a component name into a live component.
         * Passing one lets a test register two types and nothing else, rather
         * than working against whatever the process happens to hold.
         *
         * @param components Where a component name is looked up. Defaults to
         * the process-wide registry.
         */
        explicit Host(const scene::ComponentRegistry& components = scene::components());
        ~Host();

        Host(const Host&) = delete;
        Host& operator=(const Host&) = delete;
        Host(Host&&) = delete;
        Host& operator=(Host&&) = delete;

        /**
         * @brief Compiles one script and keeps it under its identity.
         *
         * Loading a GUID that is already loaded replaces the chunk. Any
         * instance already running the old one keeps running it until the
         * entity restarts, because M8.1 has no reload. Issue #264 adds one.
         *
         * @param script The identity a ScriptComponent names.
         * @param name   A readable name for the log and for a Lua traceback,
         *               normally the source path.
         * @param source The script text, as the cooked file holds it.
         * @return True when the text compiled. On a syntax error this reports
         *         the message with the name and the line, and returns false.
         */
        [[nodiscard]] bool load(Guid script, std::string_view name,
                                std::span<const std::byte> source);

        /// @brief Whether load() has accepted this identity.
        /// @param script The identity to look for.
        /// @return True when a chunk is held for it.
        [[nodiscard]] bool loaded(Guid script) const;

        /**
         * @brief Brings the instances in step with the world, then runs one step.
         *
         * Three things happen, in this order.
         *
         * 1. An entity that carries a ScriptComponent and has no instance gets
         *    one, and its `on_start` runs.
         * 2. An instance whose entity is gone, or which no longer carries the
         *    component, gets `on_destroy` and is dropped.
         * 3. Every live instance gets `on_update`.
         *
         * The sync happens here rather than in a separate call because a scene
         * reload recycles entity numbers. Comparing against the world each step
         * is what keeps an instance from attaching to whatever took its number.
         *
         * @param world   The world to read. Nothing here writes to it yet.
         * @param seconds Simulated seconds since the game started, not the wall
         *                clock and not the length of one step.
         */
        void update(scene::World& world, double seconds);

        /**
         * @brief Runs `on_destroy` on every instance and drops them all.
         *
         * Call this before the world goes away, so a script sees its own
         * teardown rather than the process ending under it.
         *
         * @param world The world the instances belong to.
         */
        void stop(scene::World& world);

        /// @brief How many entities have a live instance.
        /// @return The instance count after the last update().
        [[nodiscard]] std::size_t instance_count() const;

        /**
         * @brief How many instances an error has stopped.
         *
         * **An error stops that one instance and reports once.** A script that
         * threw in `on_update` would otherwise throw sixty times each second
         * and fill the log, and the frame would pay for it every time. The
         * entity keeps its component, so a later reload can start it again.
         *
         * @return The number of stopped instances.
         */
        [[nodiscard]] std::size_t stopped_count() const;

        /// @brief How many times a callback of this kind has run.
        /// @param callback Which callback to count.
        /// @return The count since the host was built.
        [[nodiscard]] std::size_t call_count(Callback callback) const;

    private:
        /// Binds the `entity` type, once, when the host is built.
        void bind_entity();

        struct Impl;
        std::unique_ptr<Impl> impl_;
    };

} // namespace engine::script
