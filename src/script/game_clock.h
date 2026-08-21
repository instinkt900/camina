#pragma once

/**
 * @file
 * @brief Whether the game is running, as a script sees it.
 *
 * M10.7a. A pause menu has to stop the fixed step, and `play::Session` owns
 * that step. `src/script/` sits below `src/play/` and may not name it, so this
 * is the seam: `src/script/` calls the interface and `play::Session` is the one
 * implementation. `script::UiSurface` exists for the same reason and reads the
 * same way.
 *
 * A caller that passes none leaves the `game` table answering false, the same
 * way an action reads false when nobody bound an input module.
 */

namespace engine::script {

    /**
     * @brief Holds and releases the fixed step a game runs on.
     *
     * @warning **A paused session still delivers UI presses.** A press is what
     * resumes it, and a session that delivered nothing while paused could never
     * be resumed by the menu it put on the screen. `DESIGN.md` §8.4 records the
     * rule and which clock each delivery runs on.
     */
    class GameClock {
    public:
        GameClock() = default;
        GameClock(const GameClock&) = delete;
        GameClock& operator=(const GameClock&) = delete;
        GameClock(GameClock&&) = delete;
        GameClock& operator=(GameClock&&) = delete;
        virtual ~GameClock() = default;

        /**
         * @brief Holds the steps, or lets them run again.
         *
         * A paused game runs no step at all: no `on_update`, no solver, and no
         * physics events. It accumulates no time either, so a pause of any
         * length costs the step that follows it nothing.
         *
         * @param paused True to hold the steps.
         */
        virtual void set_paused(bool paused) = 0;

        /// @brief Whether the steps are held.
        /// @return True while the game is paused.
        [[nodiscard]] virtual bool paused() const = 0;
    };

} // namespace engine::script
