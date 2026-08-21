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
 * `assets::AssetSource` and passes the bytes to load(), which keeps the interpreter
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
#include "script/game_clock.h"
#include "script/ui_surface.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>

#include "scene/component_registry.h"

namespace engine::scene {
    class World;
    class PrefabLibrary;
    class StepMotion;
} // namespace engine::scene

namespace engine::physics {
    class Simulation;
}

namespace engine::platform {
    class Input;
}

namespace engine::script {

    /**
     * @brief The seven calls a script may declare.
     *
     * A script declares what it needs and leaves out the rest. A script with no
     * `on_update` costs nothing each step, because the host looks the function
     * up once when the instance starts.
     */
    enum class Callback : std::uint8_t {
        Start,   ///< `on_start()`, once, when the entity gets its instance.
        Update,  ///< `on_update(seconds)`, once for each fixed step.
        Destroy, ///< `on_destroy()`, once, when the entity loses its instance.
        Trigger, ///< `on_trigger(other, began)`, on the entity that is the trigger.
        Contact, ///< `on_contact(other, began)`, on each of the two bodies.
        Press,   ///< `on_ui_press(layout, node)`, on every instance that declares it.
        Reload,  ///< `on_ui_reload(layout)`, after a layout was built again.
    };

    /**
     * @brief Where the view is, for a script that acts along the line of sight.
     *
     * The throw needs it, and nothing else does yet. A camera is the
     * application's rather than the scene's today, so this is a pose handed over
     * for the step rather than something a script can find in the world.
     *
     * A `scene::Camera` component is where this belongs once a scene carries a
     * camera of its own. Then a script finds it the way it finds any entity, and
     * this struct goes. See `DESIGN.md` §10 M9.
     */
    struct CameraView {
        Vec3 position{ 0.0F, 0.0F, 0.0F }; ///< Where the view is, in world space.
        Vec3 forward{ 0.0F, 0.0F, -1.0F }; ///< Which way it looks. A unit vector.
    };

    /**
     * @brief What a script may reach besides the world and its components.
     *
     * Every one is optional. A test that binds no physics passes none, and a
     * script that asks for a velocity gets nil rather than a crash.
     *
     * **These arrive on each update() rather than once at startup, for the same
     * reason the world does.** A caller may pass a different simulation after a
     * reload, and an instance lives across steps. Holding them from the call
     * that made the instance is how a handle ends up naming something nobody is
     * stepping. See issue #273.
     */
    struct Services {
        /// @brief Velocities, impulses and sleep. Null leaves those unbound.
        physics::Simulation* physics = nullptr;
        /// @brief Actions by name. Null makes every action read false.
        const platform::Input* input = nullptr;
        /// @brief Prefabs a script may instance. Null makes instancing fail.
        const scene::PrefabLibrary* prefabs = nullptr;
        /// @brief The view pose. Null makes every camera read nil.
        const CameraView* camera = nullptr;
        /// @brief The game UI. Null makes every call in the `ui` table answer false.
        UiSurface* ui = nullptr;
        /// @brief The fixed step. Null makes every call in the `game` table answer false.
        GameClock* clock = nullptr;

        /**
         * @brief Where a transform a script writes is recorded for blending.
         *
         * A script runs on the fixed step and a frame draws between two of
         * them, so an entity a script turns holds still and then jumps. This is
         * what makes it move smoothly instead, the same way it does for a body
         * the solver moves.
         *
         * Null skips the recording, which is what a test with no frames wants.
         */
        scene::StepMotion* motion = nullptr;
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
         * Loading a GUID that is already loaded replaces the text. Any instance
         * already running the old one keeps running it until the entity
         * restarts. Use reload() to change what is already running.
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

        /**
         * @brief Loads a script again and restarts everything running it.
         *
         * **A reload restarts the instance. The script table is thrown away.**
         * The next update() runs `on_destroy` on each old instance, builds a
         * fresh environment, and runs `on_start` on it. So a reload is a destroy
         * and a create, and nothing a script kept in its own table survives one.
         *
         * That is deliberate rather than a limit. Carrying a table across two
         * versions of a chunk has no answer for a value whose shape changed, and
         * the wrong answer there is a bug that looks like a game bug. **State
         * that has to survive belongs on a component**, which a script reaches
         * with `entity:get` and `entity:set`. A reload rebuilds no entity and
         * touches no component, so component state carries across untouched.
         *
         * **A text that will not compile changes nothing.** The old text stays,
         * every instance keeps running, and the message names the file and the
         * line. So a save in the middle of an edit cannot take the game down.
         *
         * The restart lands on the next update() rather than inside this call,
         * because `on_destroy` and `on_start` need the world and the services of
         * a step and this call has neither. One step therefore passes with no
         * instance, the same as changing which script an entity names.
         *
         * An instance an error had stopped is restarted too, so fixing a script
         * and saving it brings the entity back without a scene reload.
         *
         * @param script The identity to load again.
         * @param name   A readable name for the log and for a Lua traceback.
         * @param source The new script text.
         * @return True when the text compiled and the restart is pending. False
         *         when it did not, in which case nothing changed.
         */
        [[nodiscard]] bool reload(Guid script, std::string_view name,
                                  std::span<const std::byte> source);

        /// @brief Whether load() has accepted this identity.
        /// @param script The identity to look for.
        /// @return True when a chunk is held for it.
        [[nodiscard]] bool loaded(Guid script) const;

        /**
         * @brief How many instances a reload has restarted.
         *
         * A reload that names a script no entity runs restarts nothing, and a
         * reload of a script five entities share restarts five. This is what
         * measures that rather than asserting it.
         *
         * @return The count since the host was built.
         */
        [[nodiscard]] std::size_t restart_count() const;

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
         * @param services What else a script may reach this step. The default
         *                 binds none of it, which is what a test wants.
         */
        void update(scene::World& world, double seconds, const Services& services = {});

        /**
         * @brief Hands the scripts what the last solver step reported.
         *
         * @warning **Call this inside the fixed step, right after
         * `physics::Simulation::step()`, and never once for each frame.** The
         * simulation keeps the events of one step only, so a frame that ran
         * three steps and read them once would report the third step and throw
         * the first two away. A puzzle that missed an overlap because two things
         * happened in one frame is the kind nobody reproduces. See issue #263.
         *
         * **A trigger overlap goes to the trigger, and a contact goes to both
         * sides.** The two are not the same shape. A trigger has a direction,
         * because one side is the volume and the other crossed it, so
         * `on_trigger(other, began)` runs on the volume alone and `other` is the
         * visitor. A contact has no direction that Box3D promises, so a
         * one-sided call would land on whichever body the solver listed first.
         * `on_contact(other, began)` therefore runs on both, each with the other
         * as `other`.
         *
         * An entity with no instance is skipped, which is the normal case: most
         * things that touch carry no script. An entity the world no longer holds
         * is skipped too, because Box3D reports the end of an overlap when a
         * shape is destroyed.
         *
         * @param world The world the events name. The same one step() ran on.
         * @param simulation The simulation to read the events out of.
         * @param services What a callback may reach, the same as update().
         */
        void deliver_physics_events(scene::World& world, const physics::Simulation& simulation,
                                    const Services& services = {});

        /**
         * @brief Hands the scripts every UI event gathered since the last step.
         *
         * Two kinds, and the reloads go first. A rebuilt layout has thrown away
         * every text a script wrote into it, so the script writes them again on
         * `on_ui_reload` before it acts on any press of the same batch.
         *
         * @warning **Call this inside the fixed step, beside
         * `deliver_physics_events`, and never once for each frame.** M10.5
         * settled that a UI event is a frame event, so a press is recorded on
         * the frame clock. A frame often runs no step at all, so the presses
         * gather between two steps and this drains them. Draining them on the
         * frame clock instead would run game logic off the fixed step, which is
         * what issue #245 closed.
         *
         * **A press goes to every instance that declares `on_ui_press`.** A
         * press names a node and not an entity, so there is no one entity it
         * belongs to. A menu is normally one script, and that script asks which
         * node it was.
         *
         * **The instances are called in entity order.** The host keeps them in a
         * hash map, and the walk order of one is neither creation order nor
         * stable. Two scripts that both answer a press would otherwise run in an
         * order nothing promises, and a reproducible run rests on there being
         * none of that. See `DESIGN.md` section 9.
         *
         * **A reload goes to every instance that declares `on_ui_reload`**, under
         * the same rule and in the same order. A layout belongs to no one entity
         * either.
         *
         * The surface is drained at the end, so one event is delivered once.
         *
         * @param world The world the instances belong to.
         * @param services What a callback may reach. **The surface comes from
         * here**, so a call with no `ui` service delivers nothing.
         */
        void deliver_ui_events(scene::World& world, const Services& services = {});

        /**
         * @brief Runs `on_destroy` on every instance and drops them all.
         *
         * Call this before the world goes away, so a script sees its own
         * teardown rather than the process ending under it.
         *
         * @param world The world the instances belong to.
         * @param services What a teardown may still reach. **The default is
         * nothing**, because this is called as the world goes away and the
         * simulation usually goes with it. A caller whose services are still
         * alive passes them and lets `on_destroy` use them.
         */
        void stop(scene::World& world, const Services& services = {});

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
        /// Binds the `entity` type and its scene verbs, once, at build time.
        void bind_entity();

        /// Adds the physics verbs to the `entity` type bind_entity() made.
        /// Separate only because the two together are past what clang-tidy
        /// allows one function, and the split falls on a real seam.
        void bind_entity_physics();

        /// Binds the `world` table: find, create, destroy and instance.
        void bind_world();

        /// Binds the `input` table, by action name.
        void bind_input();

        /// Binds the `camera` table: where the view is and which way it looks.
        void bind_camera();

        /// Binds the `ui` table and the node handle `ui.find` gives back.
        void bind_ui();

        /// Binds the `game` table, which holds and releases the fixed step.
        void bind_game();

        struct Impl;
        std::unique_ptr<Impl> impl_;
    };

} // namespace engine::script
