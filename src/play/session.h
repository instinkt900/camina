#pragma once

/**
 * @file
 * @brief The fixed step a game runs on, and everything that advances with it.
 *
 * A game is the scripts, the solver, and the clock that drives both. This class
 * owns all three and runs one frame's worth of them in one call.
 *
 * It lived inline in `apps/runtime/main.cpp` until M9.4. The editor plays a
 * scene as well now, and a second copy of this loop would let the two
 * applications run the same game differently. See `DESIGN.md` section 9 for the
 * rules the order carries, and section 6 for why this is its own directory.
 *
 * This names no window, no device, and no ImGui type. It is the simulation of a
 * world and nothing about how that world is drawn.
 */

#include "core/timestep.h"
#include "math/conventions.h"
#include "physics/simulation.h"
#include "platform/input.h"
#include "scene/step_motion.h"
#include "script/game_clock.h"
#include "script/ui_surface.h"

#if defined(ENGINE_WITH_LUA)
#include "script/host.h"
#endif

#include <cstdint>
#include <vector>

namespace engine::assets {
    class AssetSource;
    struct AssetChange;
} // namespace engine::assets

namespace engine::scene {
    class World;
}

/// @brief Running a world: the fixed step, the solver, and the scripts on it.
namespace engine::play {

    /**
     * @brief Where the view is, for a script that acts along the line of sight.
     *
     * The view belongs to the application rather than to the scene, so the
     * caller hands it over for the step. The sandbox throw reads it. A
     * `scene::Camera` component is where this belongs once a scene carries one.
     */
    struct View {
        Vec3 position{ 0.0F, 0.0F, 0.0F }; ///< Where the camera stands.
        Vec3 forward{ 0.0F, 0.0F, -1.0F }; ///< Which way it looks. Normalized.
    };

    /**
     * @brief One running game: the clock, the bodies, the scripts, and the
     *        poses a frame blends.
     *
     * Build one, bind the actions the game reads on input(), build the bodies
     * with build(), then call advance() once for each frame.
     *
     * @code
     * engine::play::Session session;
     * session.build(world);
     * session.load_scripts(content);
     *
     * // Each frame:
     * session.feed_input(device_frame);
     * session.advance(world, view, delta_seconds);
     * @endcode
     *
     * @warning **advance() writes the drawn pose into the world.** Compose the
     * matrices and draw after it, never before, or the frame is one behind.
     */
    class Session : public script::GameClock {
    public:
        Session() = default;

        Session(const Session&) = delete;
        Session& operator=(const Session&) = delete;
        Session(Session&&) = delete;
        Session& operator=(Session&&) = delete;

        ~Session() override = default;

        /**
         * @brief Builds a body for every entity that carries one.
         *
         * Call this after the world loads, and again after anything replaces
         * the entities. `physics::Simulation::build` reads the world once, so
         * every body is stale after a scene reload or a restore.
         *
         * @param world The scene to read.
         */
        void build(scene::World& world);

        /**
         * @brief Reads every cooked script in a content tree into the host.
         *
         * This asks the source for every script rather than reading a list
         * the game holds, so a tree the application never saw at build time
         * still works. A script that
         * will not compile is reported and skipped, and the rest still load.
         *
         * Without Lua this loads nothing and reports nothing.
         *
         * @param content The open project to read the scripts out of.
         */
        void load_scripts(assets::AssetSource& content);

        /**
         * @brief Loads every changed script again, and restarts what runs it.
         *
         * **A script the project no longer holds is left alone.** Its bytes
         * are already in this process and the entities running it keep running
         * it. Deleting a source file during a session should not stop a game.
         *
         * @param content The tree the new bytes are read from.
         * @param changed What the cook reported.
         */
        void reload_scripts(assets::AssetSource& content,
                            const std::vector<assets::AssetChange>& changed);

        /**
         * @brief Runs `on_destroy` on every instance and drops them all.
         *
         * Call this before the world goes away. The bodies stay, because the
         * caller decides whether the next world is a reload or a restore. So a
         * teardown that pushes a body or reads a velocity reaches a live one,
         * which is why this hands `on_destroy` the same services a step gets.
         *
         * @param world The world the instances belong to.
         * @param view Where the camera stands, for a teardown that reads it.
         */
        void stop_scripts(scene::World& world, const View& view = {});

        /**
         * @brief Folds one device frame into what the next step reads.
         *
         * A key down on any frame since the last step is down for that step, so
         * an edge cannot fall between two steps and go unseen. A frame often
         * runs no step at all, and offscreen it almost never does, which is why
         * the fold exists rather than one Input for both clocks.
         *
         * @param frame What the devices read this frame.
         */
        void feed_input(const platform::InputFrame& frame);

        /**
         * @brief Runs the whole steps this frame owes, then blends the pose it
         *        draws.
         *
         * The game runs before the solver, because the game moves the kinematic
         * bodies and the solver has to see this step's positions. The physics
         * events are read inside the loop, because the simulation keeps the
         * events of one step and a frame that ran three would otherwise report
         * the last one alone. See issue #263.
         *
         * One alpha blends both, because they blend the same pair of steps. Two
         * would let the game and the physics draw different instants.
         *
         * **A paused session runs no step**, and it advances no clock either, so
         * a pause of any length costs the step after it nothing. It still
         * delivers the UI presses, because a menu is what resumes it. See
         * set_paused().
         *
         * @param world The scene to run and to write the drawn pose into.
         * @param view Where the camera stands, which a script may read.
         * @param delta_seconds How much wall time this frame took.
         */
        void advance(scene::World& world, const View& view, float delta_seconds);

        /**
         * @brief Holds the steps, or lets them run again.
         *
         * This implements `script::GameClock`, so a script pauses the game
         * through the `game` table. A paused session runs no `on_update`, no
         * solver and no physics events, and the poses a frame draws stand still.
         *
         * @warning **A paused session still delivers UI presses**, once for each
         * advance() rather than once for each step. A press is what resumes the
         * game, and a session that delivered nothing while paused could never be
         * resumed by the menu it put up. The presses were gathered on the frame
         * clock in the first place, so this delivers them on the clock they came
         * in on. See `DESIGN.md` §8.4.
         *
         * @param paused True to hold the steps.
         */
        void set_paused(bool paused) override { paused_ = paused; }

        /// @brief Whether the steps are held.
        /// @return True while this session is paused.
        [[nodiscard]] bool paused() const override { return paused_; }

        /**
         * @brief Points the scripts at the game UI, or at nothing.
         *
         * M10.6. A session names no moth_ui type, so the application builds the
         * surface and hands it over. A build with no game UI passes nothing and
         * every call in the `ui` table answers false. Without Lua this records
         * the surface and nothing reads it.
         *
         * @param ui The surface a script drives, or null for none. Held, not
         *           owned, and it must outlive this session.
         */
        void set_ui(script::UiSurface* ui) { ui_ = ui; }

        /// @brief The actions the game reads, on the step clock. Bind on this.
        /// @return The input the scripts see.
        [[nodiscard]] platform::Input& input() { return step_input_; }

        /// @brief The bodies, for a caller that draws them or pushes one.
        /// @return The simulation this session steps.
        [[nodiscard]] physics::Simulation& simulation() { return simulation_; }

        /**
         * @brief The poses of everything the step moved.
         *
         * A caller that is about to replace every entity clears this first.
         * EnTT hands the same entity numbers out again, so a pose left behind
         * would belong to whoever takes that number.
         *
         * @return The step motion this session interpolates.
         */
        [[nodiscard]] scene::StepMotion& motion() { return motion_; }

        /// @brief The step clock, for a caller that reports what it did.
        /// @return The fixed timestep this session advances.
        [[nodiscard]] const FixedTimestep& clock() const { return clock_; }

        /// @brief Sets the step rate. Keeps the time already accumulated.
        /// @param rate_hz Steps each second.
        void set_rate_hz(float rate_hz) { clock_.set_rate_hz(rate_hz); }

        /// @brief Sets the most steps one frame may run.
        /// @param max_steps The ceiling. See engine::FixedTimestep.
        void set_max_steps(std::uint32_t max_steps) { clock_.set_max_steps(max_steps); }

        /**
         * @brief Simulated seconds since the session began.
         *
         * Simulated and never the wall clock, which is what makes a run
         * reproducible. A double all the way to the game, because a float
         * stops resolving steps of 1/60 after about three days. See issue #245.
         *
         * @return The reading the game last ran on.
         */
        [[nodiscard]] double seconds() const { return seconds_; }

#if defined(ENGINE_WITH_LUA)
        /// @brief The script host, for a caller that loads one script by hand.
        /// @return The host this session runs.
        [[nodiscard]] script::Host& scripts() { return scripts_; }
#endif

    private:
        /// Turns a frame delta into whole steps and the blend between them.
        FixedTimestep clock_;

        /// The bodies of the scene, and the solver that steps them.
        physics::Simulation simulation_;

        /// What the game moved, and where it was before the last step.
        scene::StepMotion motion_;

        /**
         * The same actions as the device input, on the fixed step instead.
         *
         * A press edge is the difference between two update() calls, so which
         * clock update() runs on decides who can see one. The camera runs on
         * the frame and reads the application's own input. The game runs on the
         * fixed step and reads this one.
         */
        platform::Input step_input_;

        /// Every device frame since the last step, folded together.
        platform::InputFrame pending_;

        /// What the devices last read, which is what `pending_` goes back to
        /// once a step has taken it.
        platform::InputFrame latest_;

        /// Simulated seconds since the session began.
        double seconds_ = 0.0;

        /// Whether the steps are held. See set_paused().
        bool paused_ = false;

#if defined(ENGINE_WITH_LUA)
        /// The interpreter, and one instance for each scripted entity.
        script::Host scripts_;

        /// The view pose a script reads. Rewritten at the top of every step,
        /// because a script holds the services only for the call they arrive on.
        script::CameraView camera_;
#endif

        /// The game UI a script drives, or null when this build has none. It
        /// sits outside the Lua block so that set_ui compiles either way.
        script::UiSurface* ui_ = nullptr;

        /**
         * What a script may reach on this step.
         *
         * Built for each call rather than held, so a reload that builds a new
         * simulation cannot leave a script driving the old one. See issue #273.
         *
         * @param view Where the camera stands and which way it looks.
         * @return The services to pass to the host.
         */
#if defined(ENGINE_WITH_LUA)
        [[nodiscard]] script::Services step_services(const View& view);
#endif
    };

} // namespace engine::play
