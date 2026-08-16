#pragma once

/**
 * @file
 * @brief Play, pause, and stop a scene without losing what a person authored.
 *
 * Play-in-editor is a snapshot and a restore. The world is written to a
 * document before the first step, and that document is read back when the
 * session stops. So a session can move anything, destroy anything, and create
 * anything, and stop puts the authored scene back.
 *
 * The snapshot is `scene::save_scene`, which is the same writer the World panel
 * saves a source scene with. Nothing new had to be built for it, which is what
 * `DESIGN.md` section 10 means when it says M2 and M3 already provide the
 * mechanism.
 *
 * This header names no ImGui type, the same as the panels beside it. The panel
 * draws the three buttons and reports a `PlayRequest`, and the application
 * calls the matching method here.
 */

#include "play/session.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <memory>

namespace engine::assets {
    class Content;
}

namespace engine::scene {
    class World;
}

namespace engine::editor {

    /// @brief What a play session is doing.
    enum class PlayState : std::uint8_t {
        Edit,    ///< No session. The world is the authored one.
        Playing, ///< A session is running and the steps advance.
        Paused,  ///< A session is held. No step runs and nothing is lost.
    };

    /// @brief What the play bar asked for this frame.
    enum class PlayRequest : std::uint8_t {
        None,   ///< Nothing was clicked.
        Play,   ///< Start a session from the world as it stands.
        Pause,  ///< Hold the steps of the running session.
        Resume, ///< Let a held session run again.
        Stop,   ///< End the session and put the authored world back.
    };

    /**
     * @brief What a session needs from the application when it starts.
     *
     * The game names the actions its scripts read, and the application owns the
     * keys those actions are bound to. So the binding arrives here rather than
     * being written into the editor, which knows no game.
     */
    struct PlayDesc {
        /// @brief The cooked tree the scripts are read from. Null loads none.
        assets::Content* content = nullptr;

        /// @brief Binds the game's actions on the session input. Null binds
        /// nothing, and every action a script reads is then false.
        void (*bind_actions)(platform::Input&) = nullptr;
    };

    /**
     * @brief One editor play session, and the authored world it goes back to.
     *
     * @code
     * engine::editor::PlayMode play;
     * play.play(world, { .content = &content, .bind_actions = &sandbox::bind_actions });
     * play.advance(world, view, delta_seconds); // once for each frame
     * play.stop(world);                         // the world is authored again
     * @endcode
     *
     * @warning **The entity numbers change at both ends of a session.** EnTT
     * hands the same numbers out again after a clear, so anything holding an
     * entity, a selection above all, has to let go across a play and a stop.
     */
    class PlayMode {
    public:
        PlayMode() = default;

        PlayMode(const PlayMode&) = delete;
        PlayMode& operator=(const PlayMode&) = delete;
        PlayMode(PlayMode&&) = delete;
        PlayMode& operator=(PlayMode&&) = delete;

        ~PlayMode() = default;

        /**
         * @brief Snapshots the world and starts a session on it.
         *
         * The session is built here rather than held, so every part of it
         * starts clean: a script host with no instances, a clock at zero, and
         * bodies read from the world as it stands. That is what makes a second
         * play the same as the first.
         *
         * @param world The world to snapshot and then run.
         * @param desc The content the scripts come from, and the key bindings.
         * @return True when the session started. False leaves the world alone
         * and the state at Edit.
         */
        bool play(scene::World& world, const PlayDesc& desc);

        /// @brief Holds the steps. The session and everything in it stays.
        void pause();

        /// @brief Lets a held session run again.
        void resume();

        /**
         * @brief Ends the session and puts the authored world back.
         *
         * Every script gets `on_destroy` while the simulation it may reach is
         * still alive. Then the session goes, every entity goes with it, and
         * the snapshot is read into the empty world.
         *
         * **A restored transform never fights a body.** Writing a transform
         * onto a dynamic body does nothing, which is issue #284. That cannot
         * happen here, because the session and every body it built are gone
         * before the first entity is read back.
         *
         * Calling this in Edit state does nothing.
         *
         * @param world The world to clear and to read the snapshot into.
         */
        void stop(scene::World& world);

        /**
         * @brief Runs the session for one frame.
         *
         * Does nothing unless a session is playing. A paused session
         * accumulates no time at all, so a long pause cannot make the frame
         * after it run a burst of steps.
         *
         * @param world The world to run and to write the drawn pose into.
         * @param view Where the camera stands, which a script may read.
         * @param delta_seconds How much wall time this frame took.
         */
        void advance(scene::World& world, const play::View& view, float delta_seconds);

        /**
         * @brief Folds one device frame into what the next step reads.
         *
         * Does nothing without a session, so a frame the editor spends in Edit
         * state cannot leave a key held for the session after it.
         *
         * @param frame What the devices read this frame, or a default frame for
         * a reader that must see nothing.
         */
        void feed_input(const platform::InputFrame& frame);

        /// @brief What the session is doing.
        /// @return The state the buttons are drawn from.
        [[nodiscard]] PlayState state() const { return state_; }

        /// @brief Whether a session exists, playing or paused.
        /// @return True in Playing and in Paused.
        [[nodiscard]] bool running() const { return state_ != PlayState::Edit; }

        /// @brief The running session, or null in Edit state.
        /// @return The session, for a caller that draws its bodies.
        [[nodiscard]] play::Session* session() { return session_.get(); }

        /**
         * @brief The document the world goes back to.
         *
         * Empty in Edit state. A test compares this against the world after a
         * stop, which is the whole of the restore guarantee.
         *
         * @return The snapshot taken when the session started.
         */
        [[nodiscard]] const nlohmann::json& snapshot() const { return snapshot_; }

    private:
        /// The running session, built by play() and dropped by stop().
        std::unique_ptr<play::Session> session_;

        /// The authored world, as a document. Taken before the first step.
        nlohmann::json snapshot_;

        PlayState state_ = PlayState::Edit;
    };

} // namespace engine::editor
