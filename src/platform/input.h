#pragma once

/**
 * @file
 * @brief Keyboard and mouse state, sampled once for each frame.
 *
 * `DESIGN.md` §6 has always listed input under `platform/`, and the directory
 * did not hold it. Every input read sat inline in the runtime application, so
 * nothing except the debug camera could read a key. See issue #207.
 *
 * The module splits into two halves on purpose.
 *
 * sample() reads the devices and returns an InputFrame. It is the only function
 * here that names SDL, and it needs a window. Input::update() takes that frame
 * and works out the edges. It names no device and needs no window, so a test
 * drives it with a frame it wrote by hand.
 *
 * An InputFrame is a plain struct of bools and two floats. `DESIGN.md` §9 names
 * "sample input into a plain serializable struct" as one of the three decisions
 * that keep networking possible later, and this is that decision.
 *
 * @code
 * engine::platform::Input input;
 * input.bind("throw", engine::platform::Key::F);
 *
 * // Once for each frame.
 * input.update(engine::platform::sample(window, consumed));
 * if (input.pressed("throw")) {
 *     // What the action means is the caller's business. In the sandbox it is
 *     // a line of Lua, because the game logic is a script.
 * }
 * @endcode
 */

#include "math/conventions.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace engine::platform {

    class Window;

    /**
     * @brief A key, named by the engine rather than by SDL.
     *
     * The engine names its own keys so that nothing above `platform/` includes
     * SDL to ask about one. A Lua script binds an action to `Key::F` and never
     * to a scancode, which is what issue #207 asked for.
     *
     * These are physical key positions and not letters. `Key::W` is the key
     * where W sits on a US keyboard, whatever it prints on another layout. That
     * is what a movement binding wants, because WASD is a shape on the keyboard.
     */
    enum class Key : std::uint8_t {
        A,            ///< The A key.
        B,            ///< The B key.
        C,            ///< The C key.
        D,            ///< The D key.
        E,            ///< The E key.
        F,            ///< The F key.
        G,            ///< The G key.
        H,            ///< The H key.
        I,            ///< The I key.
        J,            ///< The J key.
        K,            ///< The K key.
        L,            ///< The L key.
        M,            ///< The M key.
        N,            ///< The N key.
        O,            ///< The O key.
        P,            ///< The P key.
        Q,            ///< The Q key.
        R,            ///< The R key.
        S,            ///< The S key.
        T,            ///< The T key.
        U,            ///< The U key.
        V,            ///< The V key.
        W,            ///< The W key.
        X,            ///< The X key.
        Y,            ///< The Y key.
        Z,            ///< The Z key.
        Num0,         ///< The 0 key on the number row.
        Num1,         ///< The 1 key on the number row.
        Num2,         ///< The 2 key on the number row.
        Num3,         ///< The 3 key on the number row.
        Num4,         ///< The 4 key on the number row.
        Num5,         ///< The 5 key on the number row.
        Num6,         ///< The 6 key on the number row.
        Num7,         ///< The 7 key on the number row.
        Num8,         ///< The 8 key on the number row.
        Num9,         ///< The 9 key on the number row.
        Space,        ///< The space bar.
        Enter,        ///< The return key.
        Escape,       ///< The escape key.
        Tab,          ///< The tab key.
        LeftShift,    ///< The left shift key.
        RightShift,   ///< The right shift key.
        LeftControl,  ///< The left control key.
        RightControl, ///< The right control key.
        LeftAlt,      ///< The left alt key.
        RightAlt,     ///< The right alt key.
        Left,         ///< The left arrow key.
        Right,        ///< The right arrow key.
        Up,           ///< The up arrow key.
        Down,         ///< The down arrow key.
        Count         ///< How many keys this enum names. Not a key.
    };

    /// @brief A mouse button, named by the engine rather than by SDL.
    enum class MouseButton : std::uint8_t {
        Left,   ///< The left button.
        Right,  ///< The right button.
        Middle, ///< The middle button, which is usually the wheel.
        Count   ///< How many buttons this enum names. Not a button.
    };

    /// @brief How many keys Key names, as a size.
    inline constexpr std::size_t kKeyCount = static_cast<std::size_t>(Key::Count);

    /**
     * @brief What SDL calls the key @p key is mapped to.
     *
     * The mapping from `Key` to an SDL scancode is a switch of about fifty
     * entries, and until #268 nothing checked one of them. A wrong entry
     * compiled, linked, passed every test, and bound the wrong key. The failure
     * a person met was "that key does nothing", with no message anywhere.
     *
     * This is what lets a test check the table without a window, a keyboard or
     * a video driver. It asks SDL for the name of the scancode the switch
     * returned, and the test compares that against a list of names written
     * separately. Two statements of one intention have to agree, so a typo in
     * either one is caught by the other.
     *
     * The letters and the digits are the regular part. The specials are where a
     * typo hides, because `Key::Enter` maps to `SDL_SCANCODE_RETURN` and
     * several others do not match their own name either.
     *
     * @param key The key to ask about. `Key::Count` and anything past it gives
     * an empty string.
     * @return The SDL name, or an empty string when the key maps to no
     * scancode. Never null.
     */
    [[nodiscard]] const char* scancode_name(Key key);
    /// @brief How many buttons MouseButton names, as a size.
    inline constexpr std::size_t kMouseButtonCount =
        static_cast<std::size_t>(MouseButton::Count);

    /**
     * @brief What another reader already took from this frame.
     *
     * ImGui takes the keyboard while a person types in a panel, and it takes the
     * mouse while a person drags one. Input applies this, so every reader gets
     * the gate rather than only the camera.
     *
     * The caller fills this in, because `gfx::imgui_wants_input()` reports it
     * and `platform/` sits below `gfx/`. Reading it here would turn the layer
     * order around.
     */
    struct InputConsumed {
        bool mouse = false;    ///< Another reader owns the mouse this frame.
        bool keyboard = false; ///< Another reader owns the keyboard this frame.
    };

    /**
     * @brief Every device value for one frame, and nothing else.
     *
     * A plain struct of bools and floats. It holds no pointer, no handle, and no
     * SDL type, so a later replay or a network layer can write one to a file and
     * read it back. See `DESIGN.md` §9.
     */
    struct InputFrame {
        /// @brief Whether each key is down, indexed by Key.
        std::array<bool, kKeyCount> keys{};
        /// @brief Whether each mouse button is down, indexed by MouseButton.
        std::array<bool, kMouseButtonCount> mouse_buttons{};
        /// @brief How far the mouse moved since the last sample, in pixels.
        Vec2 mouse_delta{ 0.0F, 0.0F };
        /**
         * @brief Where the pointer is, in pixels from the top left of the window.
         *
         * A delta cannot answer "what is under the pointer", and a UI hit test
         * asks exactly that. So the position rides beside the delta rather than
         * replacing it. See M10.5.
         *
         * A frame nobody sampled reads (0, 0), which is a real corner of the
         * window. So a reader works from the change between two frames rather
         * than from the value alone, and an offscreen run reports no movement
         * at all.
         */
        Vec2 mouse_position{ 0.0F, 0.0F };
        /// @brief Whether the window held the keyboard focus when this was sampled.
        bool focused = false;
    };

    /**
     * @brief Reads the keyboard and the mouse for this frame.
     *
     * This is the only function in the engine that reads a device. It needs a
     * live window, because a key counts only while that window holds focus.
     *
     * A frame that @p consumed claims arrives with those values already cleared,
     * so no caller has to remember the gate.
     *
     * @param window The window that has to hold focus for a key to count.
     * @param consumed What another reader already took this frame.
     * @return The device state, with the consumed parts cleared.
     */
    [[nodiscard]] InputFrame sample(const Window& window, InputConsumed consumed);

    /**
     * @brief Holds the pointer still so a drag never runs out of screen.
     *
     * This writes to a device rather than reading one, so it does not belong to
     * a frame. Call it when the look begins and again when it ends.
     *
     * @param window The window to capture the pointer inside.
     * @param on True to hold the pointer, false to release it.
     */
    void set_relative_mouse(const Window& window, bool on);

    /**
     * @brief Turns frames of device state into actions a caller reads by name.
     *
     * An action is a name bound to one or more keys or mouse buttons. The caller
     * asks for the name, so a rebind changes one call to bind() and nothing
     * else. A script names the action and never the key.
     *
     * @warning update() must run once for each frame, and exactly once. The
     * edges come from comparing this frame against the one before, so a second
     * call in one frame reports every edge as finished.
     */
    class Input {
    public:
        /**
         * @brief Binds a key to an action.
         *
         * An action may carry several bindings. It is held when any one of them
         * is held. Binding the same key to one action twice does nothing.
         *
         * @param action The name a caller reads. It is copied.
         * @param key The key to bind.
         */
        void bind(std::string_view action, Key key);

        /**
         * @brief Binds a mouse button to an action.
         *
         * @param action The name a caller reads. It is copied.
         * @param button The button to bind.
         */
        void bind(std::string_view action, MouseButton button);

        /// @brief Forgets every binding. The frame state is left alone.
        void clear_bindings();

        /**
         * @brief Every key bound to one action.
         *
         * This is the reverse of bind(), and it exists so that a replay can hold
         * down whatever a game bound rather than naming a key of its own.
         * `runtime --key` uses it, which is what keeps the runtime from naming a
         * key that belongs to the game. See `DESIGN.md` section 9.
         *
         * @param action The name bind() was given.
         * @return The keys, or an empty span when nothing bound that name. The
         *         span is valid until the next bind() or clear_bindings().
         */
        [[nodiscard]] std::span<const Key> keys_of(std::string_view action) const;

        /**
         * @brief Takes the frame and works out the edges.
         *
         * Call this once for each frame. An offscreen run has no devices, so it
         * passes a default InputFrame and every action reads false.
         *
         * @param frame The device state for this frame, usually from sample().
         */
        void update(const InputFrame& frame);

        /**
         * @brief Whether the action is down now.
         *
         * @param action The name bind() was given. An unbound name is false.
         * @return True while any bound key or button is down.
         */
        [[nodiscard]] bool held(std::string_view action) const;

        /**
         * @brief Whether the action went down on this frame.
         *
         * The edge and not the state. A throw bound to a held key would fire on
         * every frame, which is never what a caller means.
         *
         * @param action The name bind() was given. An unbound name is false.
         * @return True only on the frame the action goes down.
         */
        [[nodiscard]] bool pressed(std::string_view action) const;

        /**
         * @brief Whether the action came up on this frame.
         *
         * @param action The name bind() was given. An unbound name is false.
         * @return True only on the frame the action comes up.
         */
        [[nodiscard]] bool released(std::string_view action) const;

        /// @brief How far the mouse moved on this frame, in pixels.
        /// @return The movement since the frame before.
        [[nodiscard]] Vec2 mouse_delta() const { return current_.mouse_delta; }

        /// @brief Where the pointer was on this frame, in window pixels.
        /// @return The position from the top left of the window.
        [[nodiscard]] Vec2 mouse_position() const { return current_.mouse_position; }

        /// @brief Whether the window held the keyboard focus on this frame.
        /// @return True while the window has focus.
        [[nodiscard]] bool focused() const { return current_.focused; }

        /**
         * @brief The frame this update() was given.
         *
         * A replay writes this out and feeds it back to update(). See
         * `DESIGN.md` §9.
         *
         * @return The current frame.
         */
        [[nodiscard]] const InputFrame& frame() const { return current_; }

    private:
        /// One action name and everything bound to it.
        struct Action {
            std::string name;                 ///< The name a caller reads.
            std::vector<Key> keys;            ///< Every key bound to it.
            std::vector<MouseButton> buttons; ///< Every mouse button bound to it.
        };

        [[nodiscard]] const Action* find(std::string_view action) const;
        [[nodiscard]] Action& lookup(std::string_view action);
        [[nodiscard]] static bool down(const InputFrame& frame, const Action& action);

        std::vector<Action> actions_;
        InputFrame current_;
        InputFrame previous_;
    };

} // namespace engine::platform
