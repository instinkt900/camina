#include "ui/input_bridge.h"

#include "core/assert.h"

#include <moth_ui/nodes/group.h>

#include <cstddef>
#include <memory>

namespace engine::ui {

    namespace {

        /// Turns a pixel position into the integer space a moth_ui event carries.
        [[nodiscard]] moth_ui::IntVec2 point_of(Vec2 position) {
            return moth_ui::IntVec2{ static_cast<int>(position.x),
                                     static_cast<int>(position.y) };
        }

        /// Whether the pointer is somewhere else than it was.
        [[nodiscard]] bool moved(const platform::InputFrame& now,
                                 const platform::InputFrame& before) {
            return point_of(now.mouse_position) != point_of(before.mouse_position);
        }

    } // namespace

    bool LayoutListener::OnEvent(const moth_ui::Event& event) {
        // Read through the owner on every event. The pointer a reload leaves
        // behind is the trap this class exists to stop repeating.
        if (root_ == nullptr) {
            return false;
        }
        const std::shared_ptr<moth_ui::Node>& root = *root_;
        if (!root) {
            return false;
        }

        // A captured node first, then the whole tree. This is what
        // moth_ui::flow::TransitioningLayer does, and a widget in an exclusive
        // input mode loses to its siblings without it. A capture holder that
        // declines an event sees it a second time through the broadcast, which
        // moth_ui documents and its widgets expect.
        if (auto* group = dynamic_cast<moth_ui::Group*>(root.get())) {
            if (const std::shared_ptr<moth_ui::Node> captured = group->GetCapturedNode()) {
                if (captured->OnEvent(event)) {
                    return true;
                }
            }
        }

        return root->Broadcast(event);
    }

    bool InputBridge::take(platform::InputFrame& frame, moth_ui::IEventListener* listener) {
        // What the devices reported, kept before anything is taken out of it.
        // The next frame compares against this and never against what is left,
        // because a key the UI owns reads as up in the frame the caller keeps.
        // Comparing against that would report an up the person never made.
        const platform::InputFrame device = frame;

        if (listener == nullptr) {
            // No layout means the UI owns nothing. Holding an old claim would
            // leave the game unable to read a button until somebody pressed it
            // again.
            owned_keys_.fill(false);
            owned_buttons_.fill(false);
            previous_ = device;
            return false;
        }

        // The move first, so a widget knows what is under the pointer before it
        // is asked about a press there.
        if (moved(device, previous_)) {
            const moth_ui::EventMouseMove event{
                point_of(device.mouse_position),
                moth_ui::FloatVec2{ device.mouse_delta.x, device.mouse_delta.y }
            };
            // A move is never taken. moth_ui declines one, because a hover
            // changes what a widget looks like and belongs to nobody.
            (void)listener->OnEvent(event);
        }

        for (std::size_t i = 0; i < platform::kMouseButtonCount; ++i) {
            const bool down = device.mouse_buttons.at(i);
            if (down == previous_.mouse_buttons.at(i)) {
                continue;
            }
            const moth_ui::MouseButton button =
                moth_button(static_cast<platform::MouseButton>(i));
            const moth_ui::IntVec2 point = point_of(device.mouse_position);
            const bool consumed =
                down ? listener->OnEvent(moth_ui::EventMouseDown{ button, point })
                     : listener->OnEvent(moth_ui::EventMouseUp{ button, point });
            if (down && consumed) {
                owned_buttons_.at(i) = true;
            }
        }

        const int mods = moth_mods(device);
        for (std::size_t i = 0; i < platform::kKeyCount; ++i) {
            const bool down = device.keys.at(i);
            if (down == previous_.keys.at(i)) {
                continue;
            }
            const moth_ui::EventKey event{
                down ? moth_ui::KeyAction::Down : moth_ui::KeyAction::Up,
                moth_key(static_cast<platform::Key>(i)), mods
            };
            if (listener->OnEvent(event) && down) {
                owned_keys_.at(i) = true;
            }
        }

        // A claim ends when the key or the button comes up. The release itself
        // has already gone to the UI above, because a press taken on a menu owns
        // its release too: that release is what activates a button.
        bool took = false;
        for (std::size_t i = 0; i < platform::kKeyCount; ++i) {
            if (!device.keys.at(i)) {
                owned_keys_.at(i) = false;
            }
            if (owned_keys_.at(i)) {
                frame.keys.at(i) = false;
                took = true;
            }
        }
        for (std::size_t i = 0; i < platform::kMouseButtonCount; ++i) {
            if (!device.mouse_buttons.at(i)) {
                owned_buttons_.at(i) = false;
            }
            if (owned_buttons_.at(i)) {
                frame.mouse_buttons.at(i) = false;
                took = true;
            }
        }

        previous_ = device;
        return took;
    }

    void InputBridge::forget() {
        owned_keys_.fill(false);
        owned_buttons_.fill(false);
    }

    bool InputBridge::owns(platform::Key key) const {
        ENGINE_ASSERT(key != platform::Key::Count, "Key::Count is not a key.");
        return owned_keys_.at(static_cast<std::size_t>(key));
    }

    bool InputBridge::owns(platform::MouseButton button) const {
        ENGINE_ASSERT(button != platform::MouseButton::Count,
                      "MouseButton::Count is not a button.");
        return owned_buttons_.at(static_cast<std::size_t>(button));
    }

    // A switch rather than a table indexed by Key, for the reason
    // platform/input.cpp gives for scancode_of: a table has to hold the same
    // order as the enum and nothing checks that, while a switch reports a
    // missing enumerator through -Wswitch.
    moth_ui::Key moth_key(platform::Key key) {
        switch (key) {
        case platform::Key::A:
            return moth_ui::Key::A;
        case platform::Key::B:
            return moth_ui::Key::B;
        case platform::Key::C:
            return moth_ui::Key::C;
        case platform::Key::D:
            return moth_ui::Key::D;
        case platform::Key::E:
            return moth_ui::Key::E;
        case platform::Key::F:
            return moth_ui::Key::F;
        case platform::Key::G:
            return moth_ui::Key::G;
        case platform::Key::H:
            return moth_ui::Key::H;
        case platform::Key::I:
            return moth_ui::Key::I;
        case platform::Key::J:
            return moth_ui::Key::J;
        case platform::Key::K:
            return moth_ui::Key::K;
        case platform::Key::L:
            return moth_ui::Key::L;
        case platform::Key::M:
            return moth_ui::Key::M;
        case platform::Key::N:
            return moth_ui::Key::N;
        case platform::Key::O:
            return moth_ui::Key::O;
        case platform::Key::P:
            return moth_ui::Key::P;
        case platform::Key::Q:
            return moth_ui::Key::Q;
        case platform::Key::R:
            return moth_ui::Key::R;
        case platform::Key::S:
            return moth_ui::Key::S;
        case platform::Key::T:
            return moth_ui::Key::T;
        case platform::Key::U:
            return moth_ui::Key::U;
        case platform::Key::V:
            return moth_ui::Key::V;
        case platform::Key::W:
            return moth_ui::Key::W;
        case platform::Key::X:
            return moth_ui::Key::X;
        case platform::Key::Y:
            return moth_ui::Key::Y;
        case platform::Key::Z:
            return moth_ui::Key::Z;
        case platform::Key::Num0:
            return moth_ui::Key::N0;
        case platform::Key::Num1:
            return moth_ui::Key::N1;
        case platform::Key::Num2:
            return moth_ui::Key::N2;
        case platform::Key::Num3:
            return moth_ui::Key::N3;
        case platform::Key::Num4:
            return moth_ui::Key::N4;
        case platform::Key::Num5:
            return moth_ui::Key::N5;
        case platform::Key::Num6:
            return moth_ui::Key::N6;
        case platform::Key::Num7:
            return moth_ui::Key::N7;
        case platform::Key::Num8:
            return moth_ui::Key::N8;
        case platform::Key::Num9:
            return moth_ui::Key::N9;
        case platform::Key::Space:
            return moth_ui::Key::Space;
        case platform::Key::Enter:
            return moth_ui::Key::Return;
        case platform::Key::Escape:
            return moth_ui::Key::Escape;
        case platform::Key::Tab:
            return moth_ui::Key::Tab;
        case platform::Key::LeftShift:
            return moth_ui::Key::Lshift;
        case platform::Key::RightShift:
            return moth_ui::Key::Rshift;
        case platform::Key::LeftControl:
            return moth_ui::Key::Lctrl;
        case platform::Key::RightControl:
            return moth_ui::Key::Rctrl;
        case platform::Key::LeftAlt:
            return moth_ui::Key::Lalt;
        case platform::Key::RightAlt:
            return moth_ui::Key::Ralt;
        case platform::Key::Left:
            return moth_ui::Key::Left;
        case platform::Key::Right:
            return moth_ui::Key::Right;
        case platform::Key::Up:
            return moth_ui::Key::Up;
        case platform::Key::Down:
            return moth_ui::Key::Down;
        case platform::Key::Count:
            break;
        }
        return moth_ui::Key::Unknown;
    }

    moth_ui::MouseButton moth_button(platform::MouseButton button) {
        switch (button) {
        case platform::MouseButton::Left:
            return moth_ui::MouseButton::Left;
        case platform::MouseButton::Right:
            return moth_ui::MouseButton::Right;
        case platform::MouseButton::Middle:
            return moth_ui::MouseButton::Middle;
        case platform::MouseButton::Count:
            break;
        }
        return moth_ui::MouseButton::Unknown;
    }

    int moth_mods(const platform::InputFrame& frame) {
        const auto down = [&frame](platform::Key key) {
            return frame.keys.at(static_cast<std::size_t>(key));
        };

        int mods = 0;
        if (down(platform::Key::LeftShift)) {
            mods |= moth_ui::KeyMod_LeftShift;
        }
        if (down(platform::Key::RightShift)) {
            mods |= moth_ui::KeyMod_RightShift;
        }
        if (down(platform::Key::LeftControl)) {
            mods |= moth_ui::KeyMod_LeftCtrl;
        }
        if (down(platform::Key::RightControl)) {
            mods |= moth_ui::KeyMod_RightCtrl;
        }
        if (down(platform::Key::LeftAlt)) {
            mods |= moth_ui::KeyMod_LeftAlt;
        }
        if (down(platform::Key::RightAlt)) {
            mods |= moth_ui::KeyMod_RightAlt;
        }
        return mods;
    }

} // namespace engine::ui
