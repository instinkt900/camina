// M8.0 tests for the input module.
//
// Every test here drives Input::update() with a frame it wrote by hand. That is
// the whole reason sample() is a separate function: reading a device needs a
// window and a person at the keyboard, and neither one exists in CI.
//
// So this file opens no window, starts no SDL subsystem, and names no scancode.
// What it cannot cover is the mapping inside sample(), which is a table from
// Key to SDL_Scancode. A wrong entry there is a wrong key, and only a person
// pressing that key finds it.

#include "check.h"
#include "platform/input.h"

#include <cstddef>

namespace {

    using test::check;
    using test::section;
    namespace pf = engine::platform;

    /// A frame with one key down and nothing else.
    [[nodiscard]] pf::InputFrame with_key(pf::Key key) {
        pf::InputFrame frame;
        frame.focused = true;
        frame.keys.at(static_cast<std::size_t>(key)) = true;
        return frame;
    }

    /// A frame with one mouse button down and nothing else.
    [[nodiscard]] pf::InputFrame with_button(pf::MouseButton button) {
        pf::InputFrame frame;
        frame.focused = true;
        frame.mouse_buttons.at(static_cast<std::size_t>(button)) = true;
        return frame;
    }

    /// A frame with nothing down.
    [[nodiscard]] pf::InputFrame idle() {
        pf::InputFrame frame;
        frame.focused = true;
        return frame;
    }

    void test_held() {
        section("An action reports the state of every key bound to it");

        pf::Input input;
        input.bind("forward", pf::Key::W);

        input.update(idle());
        check(!input.held("forward"), "an action with nothing down is not held");

        input.update(with_key(pf::Key::W));
        check(input.held("forward"), "and it is held while its key is down");

        input.update(idle());
        check(!input.held("forward"), "and it stops when the key comes up");

        check(!input.held("nothing"), "a name nobody bound is never held");
        check(!input.pressed("nothing"), "and it is never pressed");
        check(!input.released("nothing"), "and it is never released");
    }

    void test_edges() {
        section("An edge is the frame the action changed, and only that frame");

        pf::Input input;
        input.bind("throw", pf::Key::F);

        input.update(with_key(pf::Key::F));
        check(input.pressed("throw"), "the press lands on the frame the key goes down");
        check(!input.released("throw"), "and that frame is not a release");

        // This is the bug the old throw_pressed() carried a static bool to
        // avoid. A held key that reported a press on every frame would fill the
        // room with crates in one second.
        input.update(with_key(pf::Key::F));
        check(!input.pressed("throw"), "a held key does not press again");
        check(input.held("throw"), "but it stays held");

        input.update(idle());
        check(input.released("throw"), "the release lands on the frame the key comes up");
        check(!input.pressed("throw"), "and that frame is not a press");

        input.update(idle());
        check(!input.released("throw"), "and the release does not repeat either");
    }

    void test_several_bindings() {
        section("One action takes several bindings and any one of them holds it");

        pf::Input input;
        input.bind("sprint", pf::Key::LeftShift);
        input.bind("sprint", pf::Key::RightShift);

        input.update(with_key(pf::Key::LeftShift));
        check(input.held("sprint"), "the left key holds it");

        input.update(with_key(pf::Key::RightShift));
        check(input.held("sprint"), "and so does the right one");

        // The action never came up between those two frames, so this is not a
        // new press. A caller that swaps hands mid-sprint gets one press.
        check(!input.pressed("sprint"), "swapping between two bound keys is not a new press");

        input.update(idle());
        check(!input.held("sprint"), "and it comes up when both are up");
    }

    void test_mouse() {
        section("A mouse button binds like a key, and the delta comes through");

        pf::Input input;
        input.bind("look", pf::MouseButton::Right);

        input.update(with_button(pf::MouseButton::Right));
        check(input.held("look"), "a bound mouse button holds its action");
        check(input.pressed("look"), "and it presses on the frame it goes down");

        input.update(with_button(pf::MouseButton::Left));
        check(!input.held("look"), "another button does not hold it");

        pf::InputFrame moved = idle();
        moved.mouse_delta = engine::Vec2{ 3.0F, -4.0F };
        input.update(moved);
        check(input.mouse_delta().x == 3.0F && input.mouse_delta().y == -4.0F,
              "the delta reaches the reader unchanged");
    }

    void test_mixed_bindings() {
        section("One action takes a key and a button together");

        pf::Input input;
        input.bind("fire", pf::Key::Space);
        input.bind("fire", pf::MouseButton::Left);

        input.update(with_key(pf::Key::Space));
        check(input.held("fire"), "the key holds it");

        input.update(with_button(pf::MouseButton::Left));
        check(input.held("fire"), "and the button holds it too");
    }

    void test_rebinding() {
        section("Binding is idempotent, and clearing forgets everything");

        pf::Input input;
        input.bind("jump", pf::Key::Space);
        input.bind("jump", pf::Key::Space);

        input.update(with_key(pf::Key::Space));
        check(input.held("jump"), "binding the same key twice still works");

        input.clear_bindings();
        input.update(with_key(pf::Key::Space));
        check(!input.held("jump"), "and nothing is bound after clear_bindings()");
    }

    void test_frame_is_plain() {
        section("The frame is plain data, so a replay can write it and read it back");

        // DESIGN.md section 9 asks for input sampled into a plain serializable
        // struct. This test is what holds that promise: the struct copies by
        // value, and a copy fed back to another Input gives the same answers.
        pf::Input recorded;
        recorded.bind("forward", pf::Key::W);
        recorded.update(with_key(pf::Key::W));

        const pf::InputFrame saved = recorded.frame();

        pf::Input replayed;
        replayed.bind("forward", pf::Key::W);
        replayed.update(saved);

        check(replayed.held("forward"), "a copied frame drives a second reader");
        check(replayed.pressed("forward"), "and it gives the same edge");
        check(saved.focused, "and it carries the focus flag");
    }

    void test_focus_is_reported() {
        section("The frame says whether the window had focus");

        pf::Input input;
        input.update(idle());
        check(input.focused(), "a frame sampled with focus reports it");

        input.update(pf::InputFrame{});
        check(!input.focused(), "and a default frame reports none");
    }

    void test_offscreen_run() {
        section("A default frame is what an offscreen run feeds, and nothing fires");

        pf::Input input;
        input.bind("throw", pf::Key::F);

        for (int i = 0; i < 3; ++i) {
            input.update(pf::InputFrame{});
        }

        check(!input.held("throw"), "nothing is held with no devices");
        check(!input.pressed("throw"), "and nothing presses");
        check(input.mouse_delta() == engine::Vec2{ 0.0F, 0.0F }, "and the mouse never moves");
    }

} // namespace

int main() {
    test_held();
    test_edges();
    test_several_bindings();
    test_mouse();
    test_mixed_bindings();
    test_rebinding();
    test_frame_is_plain();
    test_focus_is_reported();
    test_offscreen_run();

    if (test::g_failures != 0) {
        std::printf("\n%d check(s) failed.\n", test::g_failures);
        return 1;
    }
    std::printf("\nAll checks passed.\n");
    return 0;
}
