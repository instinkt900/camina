#pragma once

/**
 * @file
 * @brief Turns a frame of device state into moth_ui events.
 *
 * M10.5. A layout drew and answered nothing before this, because nothing routed
 * a click or a key into a moth_ui node. This is that seam.
 *
 * It reads `platform::InputFrame` rather than SDL. M8.0 put input in
 * `platform/` so that the game and the game UI read one input layer, and going
 * back to SDL here would give the UI a second one. The frame carries no device
 * type, so this file names none either.
 *
 * **The UI sees a frame before the game does, and it can take input away.** An
 * open pause menu has to swallow the key that would otherwise move the player.
 * So `InputBridge::take` mutates the frame it is given: what the UI consumed is
 * gone before any reader below it looks. See `DESIGN.md` section 8.4.
 *
 * **A UI event runs on the frame clock.** The UI sees every device frame, so a
 * click answers at the frame rate rather than at the fixed rate. The runtime
 * folds the frame into what the next fixed step reads after this call, so the
 * game never sees an event the UI took. Doing it the other way round would lose
 * a press and a release that fall between two steps, which is the problem M8.6
 * already found once.
 *
 * @code
 * engine::ui::InputBridge bridge;
 * engine::ui::LayoutListener listener{ &runtime.ui_layout };
 *
 * engine::platform::InputFrame state = engine::platform::sample(window, consumed);
 * bridge.take(state, &listener); // The UI first, and it may take from state.
 * input.update(state);           // Then the camera.
 * session.feed_input(state);     // Then the game, on the fixed step.
 * @endcode
 */

#include "platform/input.h"

#include <moth_ui/events/event_key.h>
#include <moth_ui/events/event_listener.h>
#include <moth_ui/events/event_mouse.h>
#include <moth_ui/nodes/node.h>

#include <array>
#include <memory>

namespace engine::ui {

    /**
     * @brief Sends events into a layout root the way moth_ui does itself.
     *
     * A captured node gets each event first, and then the whole tree gets a
     * depth-first broadcast. That is what `moth_ui::flow::TransitioningLayer`
     * does, and a runtime holding a bare root has to do the same or a widget in
     * an exclusive input mode never wins over its siblings.
     *
     * @warning It points at the owner of the root rather than at the root. A
     * reload frees the node tree and hands back a new one, and a raw pointer
     * taken once at start then names freed memory. That has cost this project a
     * day twice already, at M10.3 and at M10.4. See `DESIGN.md` section 8.4.
     */
    class LayoutListener : public moth_ui::IEventListener {
    public:
        /**
         * @brief Points the listener at whoever owns the layout root.
         *
         * @param root Where the root lives. It may hold null, and it may hold a
         *             different root on any later frame.
         */
        explicit LayoutListener(const std::shared_ptr<moth_ui::Node>* root)
            : root_(root) {}

        /**
         * @brief Routes one event into the layout.
         *
         * @param event The event to send.
         * @return True when the layout consumed it.
         */
        bool OnEvent(const moth_ui::Event& event) override;

    private:
        const std::shared_ptr<moth_ui::Node>* root_ = nullptr;
    };

    /**
     * @brief Turns frames of device state into moth_ui events.
     *
     * It keeps the frame before, because an event is an edge and a frame is a
     * state. It also keeps what the UI owns, so a button the UI took stays
     * taken until it comes up.
     *
     * This class names no device and opens no window, so a test drives it with
     * frames it wrote by hand. `tests/test_ui_input.cpp` is that test.
     *
     * @warning take() must run once for each frame, and exactly once. A second
     * call in one frame reports every edge as finished, for the same reason
     * `platform::Input::update` says so.
     */
    class InputBridge {
    public:
        /**
         * @brief Sends this frame's events to the UI and takes what it consumed.
         *
         * A key or a button the UI consumed is cleared out of @p frame, so no
         * reader below the UI sees it. It stays cleared on every later frame
         * until it comes up, because a press the UI took owns the release as
         * well. Without that the game would see a button held for the rest of a
         * drag that began on a menu.
         *
         * A move is never taken. moth_ui declines one, because a hover changes
         * what a widget looks like and belongs to nobody.
         *
         * @param frame The device state for this frame. Modified in place.
         * @param listener Where the events go. A null listener sends nothing and
         *                 takes nothing, which is what a runtime with no layout
         *                 loaded passes.
         * @return True when the UI took anything out of the frame.
         */
        bool take(platform::InputFrame& frame, moth_ui::IEventListener* listener);

        /**
         * @brief Drops what the UI owns, without sending anything.
         *
         * A reload replaces the node tree, and the new tree knows nothing about
         * a button the old one took. Holding that ownership would leave the game
         * unable to read the button until a person pressed and released it
         * again.
         */
        void forget();

        /// @brief Whether the UI holds the named key against every reader below it.
        /// @param key The key to ask about.
        /// @return True while the UI owns it.
        [[nodiscard]] bool owns(platform::Key key) const;

        /// @brief Whether the UI holds the named button against every reader below it.
        /// @param button The button to ask about.
        /// @return True while the UI owns it.
        [[nodiscard]] bool owns(platform::MouseButton button) const;

    private:
        /// The frame as the devices reported it, never as it was left after a take.
        platform::InputFrame previous_;
        std::array<bool, platform::kKeyCount> owned_keys_{};
        std::array<bool, platform::kMouseButtonCount> owned_buttons_{};
    };

    /**
     * @brief The moth_ui key an engine key stands for.
     *
     * @param key The engine key.
     * @return The moth_ui key, or `moth_ui::Key::Unknown` for `Key::Count`.
     */
    [[nodiscard]] moth_ui::Key moth_key(platform::Key key);

    /**
     * @brief The moth_ui button an engine mouse button stands for.
     *
     * @param button The engine button.
     * @return The moth_ui button, or `moth_ui::MouseButton::Unknown` for
     *         `MouseButton::Count`.
     */
    [[nodiscard]] moth_ui::MouseButton moth_button(platform::MouseButton button);

    /**
     * @brief The modifier bits a frame holds down.
     *
     * moth_ui carries the modifiers on the event rather than asking a device, so
     * every key event needs them worked out from the same frame.
     *
     * @param frame The frame to read.
     * @return A mask of the `moth_ui::KeyMod_` values.
     */
    [[nodiscard]] int moth_mods(const platform::InputFrame& frame);

} // namespace engine::ui
