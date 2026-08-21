#pragma once

/**
 * @file
 * @brief How a game asks to stop, as a script sees it.
 *
 * M10.7f. A game's own main menu carries the Quit button, so a script has to be
 * able to ask. `src/script/` sits below `src/play/` and may not name it, so this
 * is the seam and `play::Session` is the one implementation.
 * `script::GameClock` and `script::UiSurface` are the same shape.
 *
 * **A request rather than an exit.** Nothing here ends a process. The
 * application reads the request and decides what it means: the runtime leaves
 * its frame loop, and the editor stops the session and gives the world back to
 * whoever was editing it. A script that called `exit()` would take the editor
 * down with the game.
 *
 * A caller that passes none leaves `game.quit()` answering false, the same way
 * an action reads false when nobody bound an input module.
 */

namespace engine::script {

    /**
     * @brief A game asking to stop running.
     *
     * The request is sticky. It is raised once and read by whoever drives the
     * session, which may be several frames later, so nothing clears it but a
     * caller that has acted on it.
     */
    class GameExit {
    public:
        GameExit() = default;
        GameExit(const GameExit&) = delete;
        GameExit& operator=(const GameExit&) = delete;
        GameExit(GameExit&&) = delete;
        GameExit& operator=(GameExit&&) = delete;
        virtual ~GameExit() = default;

        /// @brief Asks whoever is driving this game to stop it.
        virtual void request_quit() = 0;

        /// @brief Whether a script has asked to stop.
        /// @return True once `request_quit` has been called.
        [[nodiscard]] virtual bool quit_requested() const = 0;
    };

} // namespace engine::script
