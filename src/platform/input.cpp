#include "platform/input.h"

#include "platform/window.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>

namespace engine::platform {

    namespace {

        /**
         * The one place a Key becomes an SDL scancode.
         *
         * A switch rather than a table indexed by Key. A table has to hold the
         * same order as the enum, and nothing checks that. Here the compiler
         * reports a missing enumerator through -Wswitch.
         */
        [[nodiscard]] SDL_Scancode scancode_of(Key key) {
            switch (key) {
            case Key::A:
                return SDL_SCANCODE_A;
            case Key::B:
                return SDL_SCANCODE_B;
            case Key::C:
                return SDL_SCANCODE_C;
            case Key::D:
                return SDL_SCANCODE_D;
            case Key::E:
                return SDL_SCANCODE_E;
            case Key::F:
                return SDL_SCANCODE_F;
            case Key::G:
                return SDL_SCANCODE_G;
            case Key::H:
                return SDL_SCANCODE_H;
            case Key::I:
                return SDL_SCANCODE_I;
            case Key::J:
                return SDL_SCANCODE_J;
            case Key::K:
                return SDL_SCANCODE_K;
            case Key::L:
                return SDL_SCANCODE_L;
            case Key::M:
                return SDL_SCANCODE_M;
            case Key::N:
                return SDL_SCANCODE_N;
            case Key::O:
                return SDL_SCANCODE_O;
            case Key::P:
                return SDL_SCANCODE_P;
            case Key::Q:
                return SDL_SCANCODE_Q;
            case Key::R:
                return SDL_SCANCODE_R;
            case Key::S:
                return SDL_SCANCODE_S;
            case Key::T:
                return SDL_SCANCODE_T;
            case Key::U:
                return SDL_SCANCODE_U;
            case Key::V:
                return SDL_SCANCODE_V;
            case Key::W:
                return SDL_SCANCODE_W;
            case Key::X:
                return SDL_SCANCODE_X;
            case Key::Y:
                return SDL_SCANCODE_Y;
            case Key::Z:
                return SDL_SCANCODE_Z;
            case Key::Num0:
                return SDL_SCANCODE_0;
            case Key::Num1:
                return SDL_SCANCODE_1;
            case Key::Num2:
                return SDL_SCANCODE_2;
            case Key::Num3:
                return SDL_SCANCODE_3;
            case Key::Num4:
                return SDL_SCANCODE_4;
            case Key::Num5:
                return SDL_SCANCODE_5;
            case Key::Num6:
                return SDL_SCANCODE_6;
            case Key::Num7:
                return SDL_SCANCODE_7;
            case Key::Num8:
                return SDL_SCANCODE_8;
            case Key::Num9:
                return SDL_SCANCODE_9;
            case Key::Space:
                return SDL_SCANCODE_SPACE;
            case Key::Enter:
                return SDL_SCANCODE_RETURN;
            case Key::Escape:
                return SDL_SCANCODE_ESCAPE;
            case Key::Tab:
                return SDL_SCANCODE_TAB;
            case Key::LeftShift:
                return SDL_SCANCODE_LSHIFT;
            case Key::RightShift:
                return SDL_SCANCODE_RSHIFT;
            case Key::LeftControl:
                return SDL_SCANCODE_LCTRL;
            case Key::RightControl:
                return SDL_SCANCODE_RCTRL;
            case Key::LeftAlt:
                return SDL_SCANCODE_LALT;
            case Key::RightAlt:
                return SDL_SCANCODE_RALT;
            case Key::Left:
                return SDL_SCANCODE_LEFT;
            case Key::Right:
                return SDL_SCANCODE_RIGHT;
            case Key::Up:
                return SDL_SCANCODE_UP;
            case Key::Down:
                return SDL_SCANCODE_DOWN;
            case Key::Count:
                break;
            }
            return SDL_SCANCODE_UNKNOWN;
        }

        /// The one place a MouseButton becomes an SDL button mask.
        [[nodiscard]] SDL_MouseButtonFlags mask_of(MouseButton button) {
            switch (button) {
            case MouseButton::Left:
                return SDL_BUTTON_LMASK;
            case MouseButton::Right:
                return SDL_BUTTON_RMASK;
            case MouseButton::Middle:
                return SDL_BUTTON_MMASK;
            case MouseButton::Count:
                break;
            }
            return 0;
        }

    } // namespace

    InputFrame sample(const Window& window, InputConsumed consumed) {
        InputFrame frame;

        // The relative state is drained whatever happens with it. SDL adds up
        // the movement since the last call and clears it, so a frame that skips
        // this call hands its movement to the next one. The camera would then
        // jump by everything the pointer did while an ImGui panel had it.
        float mouse_x = 0.0F;
        float mouse_y = 0.0F;
        const SDL_MouseButtonFlags buttons = SDL_GetRelativeMouseState(&mouse_x, &mouse_y);

        SDL_Window* native = window.native();
        frame.focused = native != nullptr && SDL_GetKeyboardFocus() == native;

        if (!consumed.mouse) {
            frame.mouse_delta = Vec2{ mouse_x, mouse_y };

            // The absolute position, which the relative state above does not
            // carry. A UI hit test needs to know what is under the pointer, and
            // a delta cannot say. SDL reports it in window pixels from the top
            // left, which is the space a moth_ui screen rect is already in.
            float position_x = 0.0F;
            float position_y = 0.0F;
            SDL_GetMouseState(&position_x, &position_y);
            frame.mouse_position = Vec2{ position_x, position_y };

            for (std::size_t i = 0; i < kMouseButtonCount; ++i) {
                const SDL_MouseButtonFlags mask = mask_of(static_cast<MouseButton>(i));
                frame.mouse_buttons.at(i) = (buttons & mask) != 0;
            }
        }

        // A key counts only while this window holds the focus. Without that a
        // person typing in another program drives the game.
        if (consumed.keyboard || !frame.focused) {
            return frame;
        }

        const bool* keys = SDL_GetKeyboardState(nullptr);
        if (keys == nullptr) {
            return frame;
        }

        for (std::size_t i = 0; i < kKeyCount; ++i) {
            const SDL_Scancode code = scancode_of(static_cast<Key>(i));
            if (code == SDL_SCANCODE_UNKNOWN) {
                continue;
            }
            // SDL indexes this array by scancode and sizes it by
            // SDL_SCANCODE_COUNT, so every code the switch returns is in range.
            frame.keys.at(i) = keys[static_cast<std::size_t>(code)];
        }

        return frame;
    }

    void set_relative_mouse(const Window& window, bool on) {
        SDL_Window* native = window.native();
        if (native == nullptr) {
            return;
        }
        SDL_SetWindowRelativeMouseMode(native, on);
    }

    void Input::bind(std::string_view action, Key key) {
        Action& entry = lookup(action);
        if (std::ranges::find(entry.keys, key) == entry.keys.end()) {
            entry.keys.push_back(key);
        }
    }

    void Input::bind(std::string_view action, MouseButton button) {
        Action& entry = lookup(action);
        if (std::ranges::find(entry.buttons, button) == entry.buttons.end()) {
            entry.buttons.push_back(button);
        }
    }

    void Input::clear_bindings() { actions_.clear(); }

    std::span<const Key> Input::keys_of(std::string_view action) const {
        const Action* found = find(action);
        return found == nullptr ? std::span<const Key>{} : std::span<const Key>{ found->keys };
    }

    void Input::update(const InputFrame& frame) {
        previous_ = current_;
        current_ = frame;
    }

    bool Input::held(std::string_view action) const {
        const Action* entry = find(action);
        return entry != nullptr && down(current_, *entry);
    }

    bool Input::pressed(std::string_view action) const {
        const Action* entry = find(action);
        return entry != nullptr && down(current_, *entry) && !down(previous_, *entry);
    }

    bool Input::released(std::string_view action) const {
        const Action* entry = find(action);
        return entry != nullptr && !down(current_, *entry) && down(previous_, *entry);
    }

    const Input::Action* Input::find(std::string_view action) const {
        const auto it = std::ranges::find(actions_, action, &Action::name);
        return it == actions_.end() ? nullptr : &*it;
    }

    Input::Action& Input::lookup(std::string_view action) {
        const auto it = std::ranges::find(actions_, action, &Action::name);
        if (it != actions_.end()) {
            return *it;
        }
        actions_.push_back(Action{ std::string{ action }, {}, {} });
        return actions_.back();
    }

    bool Input::down(const InputFrame& frame, const Action& action) {
        // Any one binding is enough. An action bound to both shift keys is held
        // while either is down, which is what a caller means by "sprint".
        const bool key_down = std::ranges::any_of(action.keys, [&frame](Key key) {
            return frame.keys.at(static_cast<std::size_t>(key));
        });
        if (key_down) {
            return true;
        }
        return std::ranges::any_of(action.buttons, [&frame](MouseButton button) {
            return frame.mouse_buttons.at(static_cast<std::size_t>(button));
        });
    }

} // namespace engine::platform
