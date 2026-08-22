// Issue #268. The one check on the Key to SDL scancode table.
//
// `scancode_of()` in src/platform/input.cpp is a switch of about fifty entries
// and nothing checked one of them. A wrong entry compiled, linked, passed every
// test, and bound the wrong key. The failure a person met was "that key does
// nothing", with no message anywhere.
//
// tests/test_input.cpp cannot reach it. That file drives Input::update() with
// frames written by hand, which is what lets it run with no window, and the
// mapping lives on the other side of that seam in sample().
//
// **This is two statements of one intention, and they have to agree.** The
// table below says what each Key is, in SDL's own words. platform::scancode_name
// asks SDL what the scancode the switch returned is called. A typo in either one
// is caught by the other, and neither needs a window, a keyboard or a video
// driver: SDL_GetScancodeName reads a static table.
//
// It cannot prove the table is right. Both halves could be wrong in the same
// way. What it catches is the failure that actually happens: one entry that
// says the wrong thing, and a swapped pair.

#include "check.h"
#include "platform/input.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace {

    using engine::platform::Key;
    using engine::platform::kKeyCount;
    using engine::platform::scancode_name;
    using test::check;

    /// What SDL calls each key, written out rather than derived.
    struct Expected {
        Key key;
        const char* name;
    };

    // Every entry of Key, in order. SDL names the letters and digits after
    // themselves, and the specials after what is printed on them, which is why
    // Enter is "Return" and the modifiers carry a side.
    constexpr std::array<Expected, 46> kExpected{ {
        { Key::A, "A" },
        { Key::B, "B" },
        { Key::C, "C" },
        { Key::D, "D" },
        { Key::E, "E" },
        { Key::F, "F" },
        { Key::G, "G" },
        { Key::H, "H" },
        { Key::I, "I" },
        { Key::J, "J" },
        { Key::K, "K" },
        { Key::L, "L" },
        { Key::M, "M" },
        { Key::N, "N" },
        { Key::O, "O" },
        { Key::P, "P" },
        { Key::Q, "Q" },
        { Key::R, "R" },
        { Key::S, "S" },
        { Key::T, "T" },
        { Key::U, "U" },
        { Key::V, "V" },
        { Key::W, "W" },
        { Key::X, "X" },
        { Key::Y, "Y" },
        { Key::Z, "Z" },
        { Key::Num0, "0" },
        { Key::Num1, "1" },
        { Key::Num2, "2" },
        { Key::Num3, "3" },
        { Key::Num4, "4" },
        { Key::Num5, "5" },
        { Key::Num6, "6" },
        { Key::Num7, "7" },
        { Key::Num8, "8" },
        { Key::Num9, "9" },
        { Key::Space, "Space" },
        { Key::Enter, "Return" },
        { Key::Escape, "Escape" },
        { Key::Tab, "Tab" },
        { Key::LeftShift, "Left Shift" },
        { Key::RightShift, "Right Shift" },
        { Key::LeftControl, "Left Ctrl" },
        { Key::RightControl, "Right Ctrl" },
        { Key::LeftAlt, "Left Alt" },
        { Key::RightAlt, "Right Alt" },
    } };

    /// Every key the table above names maps to the scancode SDL calls that.
    void every_key_maps_to_the_scancode_it_says() {
        for (const Expected& entry : kExpected) {
            const std::string got = scancode_name(entry.key);
            check(got == entry.name,
                  (std::string{ "Key " } + std::to_string(static_cast<int>(entry.key)) + " is '" +
                   got + "', and the table says '" + entry.name + "'")
                      .c_str());
        }
    }

    /**
     * The arrow keys, checked apart from the rest.
     *
     * SDL names these "Up", "Down", "Left" and "Right", which are also words a
     * reader could take for something else. They are listed here so that the
     * table above stays a list of things nobody would misread.
     */
    void the_arrows_are_the_arrows() {
        check(std::string{ scancode_name(Key::Left) } == "Left", "Left is the left arrow");
        check(std::string{ scancode_name(Key::Right) } == "Right", "Right is the right arrow");
        check(std::string{ scancode_name(Key::Up) } == "Up", "Up is the up arrow");
        check(std::string{ scancode_name(Key::Down) } == "Down", "Down is the down arrow");
    }

    /**
     * Every key in the enum is covered, and no two share a scancode.
     *
     * The first half is what stops a key added later from slipping past this
     * file. The second is what catches a copied case that was not edited, which
     * is the shape a swap takes: two entries returning one scancode.
     */
    void the_enum_is_covered_and_nothing_is_mapped_twice() {
        check(kExpected.size() + 4 == kKeyCount,
              "the table and the four arrows cover every key in the enum");

        std::vector<std::string> seen;
        seen.reserve(kKeyCount);
        bool duplicated = false;
        bool empty = false;
        for (std::size_t i = 0; i < kKeyCount; ++i) {
            const std::string name = scancode_name(static_cast<Key>(i));
            if (name.empty()) {
                empty = true;
                continue;
            }
            if (std::find(seen.begin(), seen.end(), name) != seen.end()) {
                std::printf("  note  two keys both map to '%s'\n", name.c_str());
                duplicated = true;
            }
            seen.push_back(name);
        }
        check(!empty, "every key in the enum maps to a scancode SDL knows");
        check(!duplicated, "and no two keys map to the same one");
    }

} // namespace

int main() {
    test::section("the Key to scancode table");
    every_key_maps_to_the_scancode_it_says();
    the_arrows_are_the_arrows();
    the_enum_is_covered_and_nothing_is_mapped_twice();
    return test::report();
}
