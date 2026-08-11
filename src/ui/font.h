#pragma once

/**
 * @file
 * @brief A font, rasterized by FreeType and shaped by HarfBuzz.
 *
 * `moth_ui::IFont` declares no methods at all. So a backend that implements
 * `IRenderer::RenderText` owns glyph rasterization, atlas packing, measurement,
 * line breaking, and both alignments. This file is all of that except the
 * alignment, which belongs to the one call that knows the destination
 * rectangle.
 *
 * The design comes from `moth_graphics/src/graphics/vulkan/vulkan_font.cpp`,
 * which solves the same problem for the moth_ui editor. Matching it is
 * deliberate: `DESIGN.md` section 8.3 records that stb_truetype was the other
 * option and why the shaping difference decided against it.
 *
 * Nothing here opens a device. `load()` builds the atlas into a byte buffer and
 * `upload()` turns that buffer into a texture, so every measurement this file
 * makes is testable with no GPU.
 */

#include "gfx/device.h"

#include <moth_ui/graphics/ifont.h>

#include <cstdint>
#include <filesystem>
#include <map>
#include <string_view>
#include <vector>

// FreeType and HarfBuzz types, forward-declared so that their headers stay in
// font.cpp. FT_Library and FT_Face are pointers to these in freetype.h, and
// hb_font_t and hb_buffer_t are opaque structs in hb.h.
/// @cond
struct FT_LibraryRec_;
struct FT_FaceRec_;
struct hb_font_t;
struct hb_buffer_t;
/// @endcond

namespace engine::ui {

    /**
     * @brief The first codepoint engine::ui::Font packs into its atlas.
     *
     * Space. Everything below it is a control character that no face draws.
     */
    inline constexpr char32_t kCoverageFirst = 0x20;

    /**
     * @brief The last codepoint engine::ui::Font packs into its atlas.
     *
     * The end of the Latin-1 supplement. That is about 190 glyphs, which packs
     * into a small atlas and loads quickly.
     *
     * **This is a limit, and a string outside it loses those glyphs.** The
     * reference walks every glyph in the face instead. That cannot work in
     * general: a CJK face carries more than 20000 glyphs, and one atlas of them
     * at a readable size is tens of megabytes. A real answer packs on demand,
     * and issue #213 holds it.
     */
    inline constexpr char32_t kCoverageLast = 0xFF;

    /**
     * @brief One glyph in the atlas.
     *
     * The bearing moves the glyph away from the pen position. `bearing_y` is
     * negative upward, because the pen sits on the baseline and a glyph is
     * drawn above it, and a screen rectangle grows downward.
     */
    struct Glyph {
        int width = 0;     ///< Drawn width in texels, without the border.
        int height = 0;    ///< Drawn height in texels, without the border.
        int bearing_x = 0; ///< Pen offset to the left edge, in pixels.
        int bearing_y = 0; ///< Pen offset to the top edge, in pixels. Negative is up.
        int advance_x = 0; ///< What this glyph moves the pen by, in pixels.
        int advance_y = 0; ///< Vertical pen movement. Zero for every horizontal script.
        float u0 = 0.0F;   ///< Left texture coordinate in the atlas.
        float v0 = 0.0F;   ///< Top texture coordinate in the atlas.
        float u1 = 0.0F;   ///< Right texture coordinate in the atlas.
        float v1 = 0.0F;   ///< Bottom texture coordinate in the atlas.
    };

    /**
     * @brief One glyph that HarfBuzz placed, in the order it draws.
     *
     * Shaping is not a character-to-glyph mapping. HarfBuzz applies kerning and
     * ligatures, so the count here does not have to match the character count
     * of the string, and the order can differ from it.
     */
    struct ShapedGlyph {
        int glyph = -1;    ///< Index into engine::ui::Font::glyph(), or -1 when the atlas has none.
        int advance_x = 0; ///< Pen movement after this glyph, in pixels.
        int advance_y = 0; ///< Vertical pen movement, in pixels.
        int offset_x = 0;  ///< Shaping offset applied to this glyph alone.
        int offset_y = 0;  ///< Shaping offset applied to this glyph alone.
    };

    /**
     * @brief One line that engine::ui::Font::wrap() produced.
     *
     * @warning `text` points into the string passed to `wrap()`. It does not
     *          own the characters, so it stops being valid when that string
     *          does.
     */
    struct WrappedLine {
        int width = 0;         ///< Width of this line in pixels, from the shaped advances.
        std::string_view text; ///< The characters of this line, with the outer spaces removed.
    };

    /**
     * @brief The FreeType library object every face is loaded through.
     *
     * FreeType keeps its allocator and its module list here, so one of these
     * has to outlive every `engine::ui::Font` loaded from it. It is a separate
     * object rather than a global, because a global would be destroyed in an
     * order nothing controls.
     *
     * @code
     * engine::ui::FontLibrary library;
     * if (library.create()) {
     *     engine::ui::Font font;
     *     font.load(library, "ui/fonts/LiberationSans-Regular.ttf", 32);
     * }
     * @endcode
     */
    class FontLibrary {
    public:
        FontLibrary() = default;

        FontLibrary(const FontLibrary&) = delete;
        FontLibrary& operator=(const FontLibrary&) = delete;
        FontLibrary(FontLibrary&&) = delete;
        FontLibrary& operator=(FontLibrary&&) = delete;
        /// @brief Closes the library, the way destroy() does.
        ~FontLibrary();

        /**
         * @brief Starts FreeType.
         *
         * @return False when FreeType would not start. It reports why.
         */
        [[nodiscard]] bool create();

        /// @brief Closes the library. Safe to call twice.
        void destroy();

        /**
         * @brief The FreeType library, for engine::ui::Font::load().
         *
         * @return The library, or nullptr when create() has not run or failed.
         */
        [[nodiscard]] FT_LibraryRec_* handle() const { return handle_; }

    private:
        FT_LibraryRec_* handle_ = nullptr;
    };

    /**
     * @brief One face at one pixel size, with its glyph atlas.
     *
     * A size is baked in, because the atlas holds rasterized glyphs. Two sizes
     * of one file are two of these, and each packs the whole covered set. Issue
     * #214 holds what that costs.
     *
     * The atlas is RGBA8 with every color channel set to white and the glyph
     * coverage in alpha. That costs four times the memory of a coverage-only
     * texture and buys one thing: a glyph is an ordinary textured quad, so text
     * draws through the same pipeline as an image and a plain shape. It is also
     * correct under an sRGB swapchain, because Vulkan applies the sRGB transfer
     * function to the color channels only and leaves alpha linear.
     *
     * @warning Call destroy() before the device goes away. The atlas texture is
     *          a device resource and nothing else frees it.
     */
    class Font final : public moth_ui::IFont {
    public:
        Font() = default;

        Font(const Font&) = delete;
        Font& operator=(const Font&) = delete;
        Font(Font&&) = delete;
        Font& operator=(Font&&) = delete;
        /// @brief Closes the face and frees the shaping state.
        ~Font() final;

        /**
         * @brief Reads a face, rasterizes its glyphs, and packs the atlas.
         *
         * This opens no device. It leaves the atlas in a byte buffer that
         * upload() then turns into a texture.
         *
         * @param library The started FreeType library. It must outlive this.
         * @param path The font file to read.
         * @param pixel_size The height to rasterize at, in pixels. Above zero.
         * @return False when the file will not open or the glyphs will not
         * pack. It reports which.
         */
        [[nodiscard]] bool load(const FontLibrary& library, const std::filesystem::path& path,
                                int pixel_size);

        /**
         * @brief Uploads the packed atlas as a texture.
         *
         * @param device The device that holds the texture. Held, not owned.
         * @return False when the texture would not be created, or when load()
         * has not run.
         */
        [[nodiscard]] bool upload(gfx::Device* device);

        /**
         * @brief Frees the atlas texture. Safe to call twice.
         *
         * @param device The device the texture was uploaded to.
         */
        void destroy(gfx::Device* device);

        /// @brief The distance from one baseline to the next.
        /// @return The height in pixels, at the size this loaded.
        [[nodiscard]] int line_height() const { return line_height_; }

        /// @brief How far the face reaches above the baseline.
        /// @return The ascent in pixels. Positive.
        [[nodiscard]] int ascent() const { return ascent_; }

        /// @brief How far the face reaches below the baseline.
        /// @return The descent in pixels. Negative, because down is positive.
        [[nodiscard]] int descent() const { return descent_; }

        /// @brief Where the face puts an underline.
        /// @return The offset from the baseline in pixels.
        [[nodiscard]] int underline() const { return underline_; }

        /**
         * @brief One packed glyph.
         *
         * @param index An index from engine::ui::ShapedGlyph::glyph.
         * @return The glyph. Asserts when the index is out of range.
         */
        [[nodiscard]] const Glyph& glyph(int index) const;

        /// @brief How many glyphs the atlas holds.
        /// @return The count, which is the covered set the face carries.
        [[nodiscard]] std::size_t glyph_count() const { return glyphs_.size(); }

        /**
         * @brief Runs the text through HarfBuzz.
         *
         * @param text The characters, as UTF-8.
         * @return The glyphs in draw order. Empty when load() has not run.
         *
         * @warning **This is const and it is not safe to call on one Font from
         *          two threads.** It fills a HarfBuzz buffer that the object
         *          owns and reuses, so two calls at once write the same buffer
         *          and read each other's glyphs. measure_width() and wrap()
         *          call this, so the same holds for them. A parallel layout
         *          pass needs one Font for each thread, or a buffer for each
         *          call.
         */
        [[nodiscard]] std::vector<ShapedGlyph> shape(std::string_view text) const;

        /**
         * @brief How wide the text draws on one line.
         *
         * This shapes the text, so it costs what shape() costs.
         *
         * @param text The characters, as UTF-8.
         * @return The sum of the shaped advances, in pixels.
         */
        [[nodiscard]] int measure_width(std::string_view text) const;

        /**
         * @brief Breaks the text into lines that fit a width.
         *
         * It breaks at a newline always, and at a space when the next word
         * would not fit. A single word wider than the limit gets a line of its
         * own rather than being cut.
         *
         * **Every newline-separated part gives at least one line, even when it
         * holds nothing.** A blank line is a line the author typed, so dropping
         * it would collapse a paragraph break and move everything below it up.
         * Empty text is the one exception, and it gives no line at all.
         *
         * @param text The characters, as UTF-8. The result points into it.
         * @param width The limit in pixels. A width of zero or less puts every
         * word on a line of its own.
         * @return One entry for each line, in order. The spaces around a line
         * are removed, and the ones inside it stay.
         */
        [[nodiscard]] std::vector<WrappedLine> wrap(std::string_view text, int width) const;

        /// @brief The texture a glyph quad samples.
        /// @return The handle, which is null until upload() succeeds.
        [[nodiscard]] gfx::TextureHandle texture() const { return texture_; }

        /**
         * @brief Points the atlas at a texture this did not create.
         *
         * upload() is the way in for anything that draws. This exists so that
         * `tests/test_ui_renderer.cpp` can drive `RenderText` with no device:
         * the recorder refuses a font whose atlas handle is null, and it
         * compares the handle without ever reading the texture.
         *
         * destroy() frees only what upload() made, so a handle that arrives
         * here is never freed by this object.
         *
         * @param texture The handle to report. Pass a null handle to clear it.
         */
        void borrow_texture(gfx::TextureHandle texture);

        /// @brief How wide the packed atlas is.
        /// @return The width in texels. Zero before load().
        [[nodiscard]] int atlas_width() const { return atlas_width_; }

        /// @brief How tall the packed atlas is.
        /// @return The height in texels. It matches the width, because the
        /// atlas is square.
        [[nodiscard]] int atlas_height() const { return atlas_height_; }

        /**
         * @brief The packed atlas bytes, four per texel.
         *
         * This is here so that a test can check the coverage with no device.
         *
         * @return The bytes, row by row from the top. Empty before load().
         */
        [[nodiscard]] const std::vector<std::uint8_t>& atlas_pixels() const { return pixels_; }

    private:
        // Packs every covered codepoint and rasterizes it, at whatever size
        // load() already set on the face. Reports false when the glyphs will
        // not fit any atlas this is willing to build.
        [[nodiscard]] bool build_atlas();

        // Breaks one newline-free run into lines and appends them. wrap() is
        // the newline split, and this is the word wrap under it.
        void wrap_segment(std::string_view segment, int width,
                          std::vector<WrappedLine>& lines) const;

        // Closes the face, the shaping state, and every measurement that came
        // from them. Every failure path in load() calls this, and so does
        // load() itself before it opens anything, so a reload starts clean.
        void release_face();

        FT_FaceRec_* face_ = nullptr;
        hb_font_t* hb_font_ = nullptr;
        // Reused between calls, so that shaping a string does not allocate a
        // buffer every time. shape() is const and this is its scratch space.
        mutable hb_buffer_t* hb_buffer_ = nullptr;

        std::vector<Glyph> glyphs_;
        // HarfBuzz answers in face glyph indices, and the atlas holds only the
        // covered ones. This turns the first into the second.
        std::map<std::uint32_t, int> glyph_index_to_atlas_;

        std::vector<std::uint8_t> pixels_;
        int atlas_width_ = 0;
        int atlas_height_ = 0;

        int line_height_ = 0;
        int ascent_ = 0;
        int descent_ = 0;
        int underline_ = 0;

        gfx::TextureHandle texture_;
        // Whether upload() made texture_. borrow_texture() leaves this false,
        // so destroy() cannot free a handle this did not create.
        bool owns_texture_ = false;
    };

}
