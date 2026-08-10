// M6.4 tests for the font: the atlas, the metrics, the shaping, and the
// wrapping.
//
// Every one of these runs with no device, the way test_frustum.cpp does. That
// is the point of splitting load() from upload(): rasterization, packing,
// measurement and line breaking are the parts most worth testing, and none of
// them needs a GPU. A wrong advance or a wrong bearing draws text that looks
// almost right, which is the hardest kind of error to see in a screenshot.
//
// The font is the one the sandbox ships. See sandbox/content/ui/fonts/.

#include "check.h"
#include "ui/font.h"

#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

namespace {

    using engine::ui::Font;
    using engine::ui::FontLibrary;
    using engine::ui::Glyph;
    using engine::ui::ShapedGlyph;
    using engine::ui::WrappedLine;
    using test::check;
    using test::section;

    /// The size everything here loads at. Large enough that a rounded advance
    /// is not the whole measurement, and small enough to pack quickly.
    constexpr int kSize = 32;

    /// CMake passes the path of the sandbox font, so this test does not guess
    /// where the build put anything.
    const char* font_path() {
        return ENGINE_TEST_FONT_PATH;
    }

    /**
     * A face loads, and its metrics are the ones a face has.
     *
     * The ordering is what this checks rather than the numbers, because the
     * numbers belong to the font file. An ascent above the baseline and a
     * descent below it is the convention every caller depends on. Getting the
     * sign of the descent wrong puts every line one descent out of place.
     */
    void loads_a_face(const FontLibrary& library) {
        section("loading");

        Font font;
        check(font.load(library, font_path(), kSize), "the sandbox font loads");
        check(font.glyph_count() > 90, "the atlas holds the printable ASCII set at least");
        check(font.line_height() > 0, "the line height is positive");
        check(font.ascent() > 0, "the ascent reaches above the baseline");
        check(font.descent() < 0, "the descent reaches below the baseline");
        check(font.ascent() - font.descent() <= font.line_height() + kSize,
              "the line height is near the ascent plus the descent");

        // The atlas is square and a power of two, and it is RGBA.
        check(font.atlas_width() == font.atlas_height(), "the atlas is square");
        check(font.atlas_width() >= 128, "the atlas is at least the smallest size");
        check(font.atlas_pixels().size() ==
                  static_cast<std::size_t>(font.atlas_width()) *
                      static_cast<std::size_t>(font.atlas_height()) * 4,
              "the atlas holds four bytes for each texel");

        font.destroy(nullptr);
    }

    /**
     * A missing file and a bad size are refused rather than half loaded.
     *
     * A font that reports success and then measures everything at zero draws
     * nothing and says nothing, which is worse than a refusal.
     */
    void refuses_what_it_cannot_load(const FontLibrary& library) {
        section("refusals");

        Font missing;
        check(!missing.load(library, "no/such/font.ttf", kSize), "a missing file is refused");

        Font zero;
        check(!zero.load(library, font_path(), 0), "a size of zero is refused");

        Font negative;
        check(!negative.load(library, font_path(), -8), "a negative size is refused");

        FontLibrary unopened;
        Font orphan;
        check(!orphan.load(unopened, font_path(), kSize),
              "a library that never started is refused");
    }

    /**
     * The atlas carries white color and the coverage in alpha.
     *
     * This is what lets a glyph draw through the image pipeline. If the color
     * channels were not white, the fragment shader would tint every glyph by
     * whatever they held, and if the coverage were in a color channel instead
     * the text would draw as opaque boxes.
     */
    void atlas_is_white_with_coverage_in_alpha(const FontLibrary& library) {
        section("atlas format");

        Font font;
        check(font.load(library, font_path(), kSize), "the font loads");

        const std::vector<std::uint8_t>& pixels = font.atlas_pixels();
        bool color_all_white = true;
        bool any_alpha_set = true;
        int opaque = 0;
        for (std::size_t i = 0; i < pixels.size(); i += 4) {
            if (pixels[i] != 0xFF || pixels[i + 1] != 0xFF || pixels[i + 2] != 0xFF) {
                color_all_white = false;
                break;
            }
            if (pixels[i + 3] != 0) {
                ++opaque;
            }
        }
        any_alpha_set = opaque > 0;

        check(color_all_white, "every color channel in the atlas is white");
        check(any_alpha_set, "some texels carry coverage in alpha");
        check(static_cast<std::size_t>(opaque) < pixels.size() / 4,
              "the atlas is not entirely covered, so the packing left space");

        font.destroy(nullptr);
    }

    /**
     * A shaped string gives one glyph for each letter, and every glyph is in
     * the atlas.
     *
     * A -1 here would mean the atlas and the shaper disagree about which glyph
     * index means what, which draws the wrong letters rather than none.
     */
    void shapes_latin_text(const FontLibrary& library) {
        section("shaping");

        Font font;
        check(font.load(library, font_path(), kSize), "the font loads");

        const std::vector<ShapedGlyph> shaped = font.shape("Hello");
        check(shaped.size() == 5, "five letters shape to five glyphs");

        bool all_found = true;
        bool all_advance = true;
        for (const ShapedGlyph& glyph : shaped) {
            if (glyph.glyph < 0) {
                all_found = false;
            } else if (font.glyph(glyph.glyph).advance_x <= 0) {
                all_advance = false;
            }
            if (glyph.advance_x <= 0) {
                all_advance = false;
            }
        }
        check(all_found, "every shaped glyph is in the atlas");
        check(all_advance, "every shaped glyph moves the pen");

        // Shaping answers with a face glyph index and the atlas is keyed on
        // one, so the two have to agree about which index means which letter.
        // Checking only that a lookup succeeded does not test that: almost
        // every index in the covered range is present, so a mapping shifted by
        // one still finds an entry and draws the wrong letters. Comparing the
        // shapes of letters that differ is what ties the index to the glyph.
        const Glyph& narrow = font.glyph(font.shape("i")[0].glyph);
        const Glyph& broad = font.glyph(font.shape("W")[0].glyph);
        check(broad.width > narrow.width * 2, "W maps to a far wider glyph than i");
        check(broad.advance_x > narrow.advance_x, "and it advances the pen further");

        const Glyph& space = font.glyph(font.shape(" ")[0].glyph);
        check(space.width == 0, "a space maps to a glyph that draws nothing");
        check(space.advance_x > 0, "and it still moves the pen");

        check(font.shape("").empty(), "an empty string shapes to nothing");

        // A codepoint outside the covered set has no atlas entry. The renderer
        // skips it rather than drawing glyph zero, which is the empty box.
        const std::vector<ShapedGlyph> outside = font.shape("\xE4\xB8\xAD");
        bool any_missing = false;
        for (const ShapedGlyph& glyph : outside) {
            if (glyph.glyph < 0) {
                any_missing = true;
            }
        }
        check(any_missing, "a codepoint past the covered set reports no glyph");

        font.destroy(nullptr);
    }

    /**
     * Measuring is monotonic, and it matches the sum of the advances.
     *
     * The width of a string is the one number both alignments divide by, so an
     * error here moves centered text by half of it and right aligned text by
     * all of it.
     */
    void measures_text(const FontLibrary& library) {
        section("measuring");

        Font font;
        check(font.load(library, font_path(), kSize), "the font loads");

        const int one = font.measure_width("i");
        const int two = font.measure_width("ii");
        const int wide = font.measure_width("WW");

        check(one > 0, "a letter has a width");
        check(two > one, "two letters are wider than one");
        check(wide > two, "two wide letters are wider than two narrow ones");
        check(font.measure_width("") == 0, "an empty string has no width");

        int summed = 0;
        for (const ShapedGlyph& glyph : font.shape("Hello world")) {
            summed += glyph.advance_x;
        }
        check(summed == font.measure_width("Hello world"),
              "the measured width is the sum of the shaped advances");

        font.destroy(nullptr);
    }

    /**
     * Wrapping breaks at a newline always and at a space when a word will not
     * fit.
     *
     * The reference walks the string once and backtracks the loop counter.
     * This is a rewrite, so it is the part of the port most worth driving hard.
     */
    void wraps_text(const FontLibrary& library) {
        section("wrapping");

        Font font;
        check(font.load(library, font_path(), kSize), "the font loads");

        // A newline breaks whatever the width is.
        const std::string two_lines = "one\ntwo";
        const std::vector<WrappedLine> newline = font.wrap(two_lines, 10000);
        check(newline.size() == 2, "a newline breaks a line that would otherwise fit");
        check(newline.size() == 2 && newline[0].text == "one", "the first line is the first part");
        check(newline.size() == 2 && newline[1].text == "two", "the second line is the second part");

        // A width that fits everything gives one line.
        const std::string sentence = "the quick brown fox";
        check(font.wrap(sentence, 10000).size() == 1, "a wide limit gives one line");

        // A width that fits two words gives more than one line, and no line is
        // wider than the limit unless one word is.
        const int limit = font.measure_width("the quick");
        const std::vector<WrappedLine> wrapped = font.wrap(sentence, limit);
        check(wrapped.size() > 1, "a narrow limit breaks the sentence");
        bool within = true;
        bool nonempty = true;
        for (const WrappedLine& line : wrapped) {
            if (line.width > limit) {
                within = false;
            }
            if (line.text.empty()) {
                nonempty = false;
            }
        }
        check(within, "no wrapped line is wider than the limit");
        check(nonempty, "no wrapped line is empty");

        // A word longer than the limit gets a line of its own rather than
        // being cut in half.
        const std::string long_word = "antidisestablishmentarianism";
        const std::vector<WrappedLine> single = font.wrap(long_word, 10);
        check(single.size() == 1, "a word wider than the limit stays whole");
        check(single.size() == 1 && single[0].text == long_word, "and it keeps every letter");

        // The reported width is the width of the line it names.
        bool widths_match = true;
        for (const WrappedLine& line : wrapped) {
            if (line.width != font.measure_width(line.text)) {
                widths_match = false;
            }
        }
        check(widths_match, "each line reports the width of its own text");

        // Runs of spaces and blank lines do not produce empty lines.
        const std::string spaced = "  a   b  \n\n  c  ";
        const std::vector<WrappedLine> trimmed = font.wrap(spaced, 10000);
        check(trimmed.size() == 2, "a blank line is dropped");
        check(!trimmed.empty() && trimmed[0].text == "a   b",
              "the outer spaces come off and the inner ones stay");

        check(font.wrap("", 100).empty(), "an empty string wraps to nothing");
        check(font.wrap("   ", 100).empty(), "a string of spaces wraps to nothing");

        font.destroy(nullptr);
    }

    /**
     * A glyph carries texture coordinates inside the atlas, and a size.
     *
     * A zero-area glyph rectangle draws nothing, and coordinates outside 0 to 1
     * read another glyph. Both draw text that is wrong rather than absent.
     */
    void glyphs_carry_atlas_coordinates(const FontLibrary& library) {
        section("glyph rectangles");

        Font font;
        check(font.load(library, font_path(), kSize), "the font loads");

        const std::vector<ShapedGlyph> shaped = font.shape("Ag");
        check(shaped.size() == 2, "two letters shape to two glyphs");

        bool inside = true;
        bool ordered = true;
        bool sized = true;
        for (const ShapedGlyph& entry : shaped) {
            if (entry.glyph < 0) {
                continue;
            }
            const Glyph& glyph = font.glyph(entry.glyph);
            if (glyph.u0 < 0.0F || glyph.v0 < 0.0F || glyph.u1 > 1.0F || glyph.v1 > 1.0F) {
                inside = false;
            }
            if (glyph.u1 <= glyph.u0 || glyph.v1 <= glyph.v0) {
                ordered = false;
            }
            if (glyph.width <= 0 || glyph.height <= 0) {
                sized = false;
            }
        }
        check(inside, "every glyph rectangle is inside the atlas");
        check(ordered, "every glyph rectangle runs top left to bottom right");
        check(sized, "every drawn glyph has an area");

        // Each glyph is packed with a transparent ring around it. Without one,
        // a linear sampler reaching just past an edge picks up the glyph packed
        // beside it, and a letter draws with a sliver of another one on its
        // side. That is invisible in a still picture at the right size and
        // obvious once anything scales, so a test has to hold it.
        const Glyph& capital_a = font.glyph(font.shape("A")[0].glyph);
        const int left = static_cast<int>(capital_a.u0 * static_cast<float>(font.atlas_width()));
        const int top = static_cast<int>(capital_a.v0 * static_cast<float>(font.atlas_height()));
        const auto alpha_at = [&font](int x, int y) {
            const std::size_t texel =
                (static_cast<std::size_t>(y) * static_cast<std::size_t>(font.atlas_width())) +
                static_cast<std::size_t>(x);
            return font.atlas_pixels()[(texel * 4) + 3];
        };
        bool ring_clear = true;
        for (int y = top - 1; y <= top + capital_a.height; ++y) {
            if (alpha_at(left - 1, y) != 0 || alpha_at(left + capital_a.width, y) != 0) {
                ring_clear = false;
            }
        }
        for (int x = left - 1; x <= left + capital_a.width; ++x) {
            if (alpha_at(x, top - 1) != 0 || alpha_at(x, top + capital_a.height) != 0) {
                ring_clear = false;
            }
        }
        check(ring_clear, "a transparent ring separates a glyph from its neighbours");

        // 'g' descends below the baseline and 'A' does not, so their bearings
        // differ. A bearing that was always zero would pass every other check
        // here and draw every letter sitting on the same line.
        const Glyph& capital = font.glyph(font.shape("A")[0].glyph);
        const Glyph& descender = font.glyph(font.shape("g")[0].glyph);
        check(capital.bearing_y < 0, "a capital is drawn above the baseline");
        check(descender.bearing_y + descender.height > capital.bearing_y + capital.height,
              "a descender reaches lower than a capital");

        font.destroy(nullptr);
    }

}

int main() {
    FontLibrary library;
    if (!library.create()) {
        std::printf("  FAIL  FreeType would not start\n");
        return EXIT_FAILURE;
    }

    loads_a_face(library);
    refuses_what_it_cannot_load(library);
    atlas_is_white_with_coverage_in_alpha(library);
    shapes_latin_text(library);
    measures_text(library);
    wraps_text(library);
    glyphs_carry_atlas_coordinates(library);

    std::printf("%d failure(s)\n", test::g_failures);
    return test::g_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
