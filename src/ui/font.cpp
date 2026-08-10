#include "ui/font.h"

#include "core/assert.h"
#include "core/log.h"

#include <ft2build.h>
#include FT_FREETYPE_H

#include <hb-ft.h>
#include <hb.h>
#include <stb_rect_pack.h>

#include <cctype>
#include <set>
#include <string>
#include <utility>

namespace engine::ui {

    namespace {

        // FreeType reports most metrics in 26.6 fixed point, which is pixels
        // times 64. Everything this file stores is whole pixels.
        constexpr int kFixedToPixels = 64;

        // One transparent texel around each glyph. Without it a linear sampler
        // reaching just past a glyph edge picks up the neighbour packed beside
        // it, and a letter draws with a sliver of another one on its side.
        constexpr int kBorderTexels = 1;

        // The atlas grows by doubling from the first size to the last. 128 is
        // large enough for the covered set of a small face, and 4096 is the
        // largest a Vulkan 1.3 device is guaranteed to accept.
        constexpr int kMinAtlasSize = 128;
        constexpr int kMaxAtlasSize = 4096;

        constexpr int kBytesPerTexel = 4;

        /// A fully opaque or fully white channel.
        constexpr std::uint8_t kFull = 0xFF;

        /// One glyph, rasterized and waiting to be packed.
        struct PendingGlyph {
            std::uint32_t index = 0; ///< The glyph index in the face.
            int width = 0;           ///< Bitmap width in texels.
            int height = 0;          ///< Bitmap height in texels.
            int bearing_x = 0;
            int bearing_y = 0;
            int advance_x = 0;
            int advance_y = 0;
            std::vector<std::uint8_t> coverage; ///< width times height bytes.
        };

        bool is_space(char character) {
            return std::isspace(static_cast<unsigned char>(character)) != 0;
        }

        /// One run of non-space characters, as a half-open range.
        struct WordSpan {
            std::size_t begin = 0;
            std::size_t end = 0;
        };

        /// Splits a line into its words and drops every run of spaces.
        std::vector<WordSpan> split_words(std::string_view segment) {
            std::vector<WordSpan> words;
            std::size_t index = 0;
            while (index < segment.size()) {
                while (index < segment.size() && is_space(segment[index])) {
                    ++index;
                }
                const std::size_t begin = index;
                while (index < segment.size() && !is_space(segment[index])) {
                    ++index;
                }
                if (index > begin) {
                    words.push_back(WordSpan{ begin, index });
                }
            }
            return words;
        }

        /**
         * Rasterizes every codepoint the atlas covers and keeps each bitmap.
         *
         * The reference renders every glyph twice, once to measure it for the
         * packer and once to blit it. Holding the coverage here costs a few
         * hundred kilobytes and halves the rasterization.
         */
        std::vector<PendingGlyph> rasterize_covered(FT_Face face) {
            std::vector<PendingGlyph> pending;
            std::set<FT_UInt> seen;

            for (char32_t codepoint = kCoverageFirst; codepoint <= kCoverageLast; ++codepoint) {
                const FT_UInt glyph_index = FT_Get_Char_Index(face, codepoint);
                if (glyph_index == 0) {
                    // The face carries no glyph for this codepoint.
                    continue;
                }
                // Several codepoints can share one glyph, and the atlas holds
                // it once.
                if (!seen.insert(glyph_index).second) {
                    continue;
                }
                if (FT_Load_Glyph(face, glyph_index, FT_LOAD_RENDER) != 0) {
                    continue;
                }

                const FT_GlyphSlot slot = face->glyph;
                PendingGlyph glyph;
                glyph.index = glyph_index;
                glyph.width = static_cast<int>(slot->bitmap.width);
                glyph.height = static_cast<int>(slot->bitmap.rows);
                glyph.bearing_x = static_cast<int>(slot->metrics.horiBearingX / kFixedToPixels);
                // Negative, because the pen sits on the baseline and a screen
                // rectangle grows downward.
                glyph.bearing_y =
                    -static_cast<int>(slot->metrics.horiBearingY / kFixedToPixels);
                glyph.advance_x = static_cast<int>(slot->advance.x / kFixedToPixels);
                glyph.advance_y = static_cast<int>(slot->advance.y / kFixedToPixels);

                glyph.coverage.resize(static_cast<std::size_t>(glyph.width) *
                                      static_cast<std::size_t>(glyph.height));
                const int pitch = slot->bitmap.pitch;
                for (int y = 0; y < glyph.height; ++y) {
                    for (int x = 0; x < glyph.width; ++x) {
                        glyph.coverage[(static_cast<std::size_t>(y) *
                                        static_cast<std::size_t>(glyph.width)) +
                                       static_cast<std::size_t>(x)] =
                            slot->bitmap.buffer[x + (y * pitch)];
                    }
                }
                pending.push_back(std::move(glyph));
            }

            return pending;
        }

        /**
         * Packs the glyphs into the smallest square that holds them.
         *
         * The reference tries every width and height pair and keeps the
         * tightest. A square that doubles is within a texel or two of that for
         * a set this size, and it packs once for each size rather than once for
         * each pair.
         *
         * @return The square size, or zero when nothing this large enough was
         * allowed.
         */
        int pack_glyphs(const std::vector<PendingGlyph>& pending,
                        std::vector<stbrp_rect>& rects) {
            rects.resize(pending.size());
            for (int size = kMinAtlasSize; size <= kMaxAtlasSize; size *= 2) {
                for (std::size_t i = 0; i < pending.size(); ++i) {
                    rects[i] = stbrp_rect{};
                    rects[i].id = static_cast<int>(i);
                    rects[i].w = static_cast<stbrp_coord>(pending[i].width + (kBorderTexels * 2));
                    rects[i].h =
                        static_cast<stbrp_coord>(pending[i].height + (kBorderTexels * 2));
                }
                std::vector<stbrp_node> nodes(static_cast<std::size_t>(size));
                stbrp_context context{};
                stbrp_init_target(&context, size, size, nodes.data(),
                                  static_cast<int>(nodes.size()));
                if (stbrp_pack_rects(&context, rects.data(), static_cast<int>(rects.size())) !=
                    0) {
                    return size;
                }
            }
            return 0;
        }

    }

    FontLibrary::~FontLibrary() {
        destroy();
    }

    bool FontLibrary::create() {
        if (handle_ != nullptr) {
            return true;
        }
        FT_Library library = nullptr;
        const FT_Error error = FT_Init_FreeType(&library);
        if (error != 0) {
            ENGINE_LOG_ERROR("FreeType would not start. Error {}.", static_cast<int>(error));
            return false;
        }
        handle_ = library;
        return true;
    }

    void FontLibrary::destroy() {
        if (handle_ == nullptr) {
            return;
        }
        FT_Done_FreeType(handle_);
        handle_ = nullptr;
    }

    Font::~Font() {
        // HarfBuzz holds the face, so it goes first. hb_ft_font_create takes no
        // reference on the face, which makes the other order a use after free.
        if (hb_buffer_ != nullptr) {
            hb_buffer_destroy(hb_buffer_);
            hb_buffer_ = nullptr;
        }
        if (hb_font_ != nullptr) {
            hb_font_destroy(hb_font_);
            hb_font_ = nullptr;
        }
        if (face_ != nullptr) {
            FT_Done_Face(face_);
            face_ = nullptr;
        }
        ENGINE_ASSERT(!texture_.valid(),
                      "A ui::Font still holds its atlas texture. Call destroy() first.");
    }

    bool Font::load(const FontLibrary& library, const std::filesystem::path& path,
                    int pixel_size) {
        if (library.handle() == nullptr) {
            ENGINE_LOG_ERROR("A font was loaded through a FontLibrary that is not started.");
            return false;
        }
        if (pixel_size <= 0) {
            ENGINE_LOG_ERROR("A font was asked for at {} pixels. A size must be above zero.",
                             pixel_size);
            return false;
        }

        const std::string path_text = path.string();
        if (FT_New_Face(library.handle(), path_text.c_str(), 0, &face_) != 0) {
            ENGINE_LOG_ERROR("The font file {} would not open.", path_text);
            face_ = nullptr;
            return false;
        }
        if (FT_Set_Pixel_Sizes(face_, 0, static_cast<FT_UInt>(pixel_size)) != 0) {
            ENGINE_LOG_ERROR("The font {} does not carry the size {}. A bitmap-only face "
                             "carries a fixed set of sizes.",
                             path_text, pixel_size);
            FT_Done_Face(face_);
            face_ = nullptr;
            return false;
        }

        hb_font_ = hb_ft_font_create(face_, nullptr);
        if (hb_font_ == nullptr) {
            ENGINE_LOG_ERROR("HarfBuzz would not take the font {}.", path_text);
            FT_Done_Face(face_);
            face_ = nullptr;
            return false;
        }

        if (!build_atlas()) {
            ENGINE_LOG_ERROR("The glyphs of {} at {} pixels would not pack into an atlas of "
                             "{} texels square.",
                             path_text, pixel_size, kMaxAtlasSize);
            return false;
        }

        line_height_ = static_cast<int>(face_->size->metrics.height / kFixedToPixels);
        ascent_ = static_cast<int>(face_->size->metrics.ascender / kFixedToPixels);
        descent_ = static_cast<int>(face_->size->metrics.descender / kFixedToPixels);
        underline_ = static_cast<int>(
            FT_MulFix(face_->underline_position, face_->size->metrics.y_scale) / kFixedToPixels);

        ENGINE_LOG_INFO("Font {} at {} px: {} glyphs in a {}x{} atlas, line height {}.",
                        path.filename().string(), pixel_size, glyphs_.size(), atlas_width_,
                        atlas_height_, line_height_);
        return true;
    }

    bool Font::build_atlas() {
        const std::vector<PendingGlyph> pending = rasterize_covered(face_);
        if (pending.empty()) {
            ENGINE_LOG_ERROR("The face carries no glyph between U+{:04X} and U+{:04X}.",
                             static_cast<std::uint32_t>(kCoverageFirst),
                             static_cast<std::uint32_t>(kCoverageLast));
            return false;
        }

        std::vector<stbrp_rect> rects;
        const int packed_size = pack_glyphs(pending, rects);
        if (packed_size == 0) {
            return false;
        }

        atlas_width_ = packed_size;
        atlas_height_ = packed_size;

        // White everywhere, with the coverage in alpha. A glyph is then an
        // ordinary textured quad and text shares the one UI pipeline. See the
        // note on the class.
        const std::size_t stride =
            static_cast<std::size_t>(atlas_width_) * static_cast<std::size_t>(kBytesPerTexel);
        pixels_.assign(stride * static_cast<std::size_t>(atlas_height_), 0);
        for (std::size_t i = 0; i < pixels_.size(); i += kBytesPerTexel) {
            pixels_[i + 0] = kFull;
            pixels_[i + 1] = kFull;
            pixels_[i + 2] = kFull;
        }

        const auto atlas_width_f = static_cast<float>(atlas_width_);
        const auto atlas_height_f = static_cast<float>(atlas_height_);

        glyphs_.resize(pending.size());
        for (const stbrp_rect& rect : rects) {
            const PendingGlyph& source = pending[static_cast<std::size_t>(rect.id)];
            const int x0 = rect.x + kBorderTexels;
            const int y0 = rect.y + kBorderTexels;

            for (int y = 0; y < source.height; ++y) {
                for (int x = 0; x < source.width; ++x) {
                    const std::size_t target =
                        (static_cast<std::size_t>(y0 + y) * stride) +
                        (static_cast<std::size_t>(x0 + x) * kBytesPerTexel) + 3;
                    pixels_[target] =
                        source.coverage[(static_cast<std::size_t>(y) *
                                         static_cast<std::size_t>(source.width)) +
                                        static_cast<std::size_t>(x)];
                }
            }

            Glyph& glyph = glyphs_[static_cast<std::size_t>(rect.id)];
            glyph.width = source.width;
            glyph.height = source.height;
            glyph.bearing_x = source.bearing_x;
            glyph.bearing_y = source.bearing_y;
            glyph.advance_x = source.advance_x;
            glyph.advance_y = source.advance_y;
            // The coordinates cover the glyph and not its border, so a sampler
            // never reads the transparent ring on purpose.
            glyph.u0 = static_cast<float>(x0) / atlas_width_f;
            glyph.v0 = static_cast<float>(y0) / atlas_height_f;
            glyph.u1 = static_cast<float>(x0 + source.width) / atlas_width_f;
            glyph.v1 = static_cast<float>(y0 + source.height) / atlas_height_f;

            glyph_index_to_atlas_[source.index] = rect.id;
        }

        return true;
    }

    bool Font::upload(gfx::Device* device) {
        if (pixels_.empty()) {
            ENGINE_LOG_ERROR("A font atlas was uploaded before it was loaded.");
            return false;
        }
        if (texture_.valid()) {
            return true;
        }

        // sRGB to match every other UI texture, so one descriptor set layout
        // serves all of them. The choice changes nothing here: the color
        // channels are 255, which decodes to 1.0 under either format, and
        // Vulkan leaves alpha linear in an sRGB format, which is what a
        // coverage value needs.
        const gfx::TextureDesc desc{
            .pixels = pixels_.data(),
            .size = pixels_.size(),
            .width = static_cast<std::uint32_t>(atlas_width_),
            .height = static_cast<std::uint32_t>(atlas_height_),
            .mip_count = 1,
            .format = gfx::TextureFormat::RGBA8Srgb,
            .sampler = { .filter = gfx::Filter::Linear,
                         .address = gfx::AddressMode::ClampToEdge },
        };
        if (!gfx::succeeded(gfx::create_texture(device, desc, &texture_))) {
            ENGINE_LOG_ERROR("The font atlas texture was not created.");
            texture_ = gfx::TextureHandle{};
            return false;
        }
        return true;
    }

    void Font::destroy(gfx::Device* device) {
        if (!texture_.valid()) {
            return;
        }
        gfx::destroy_texture(device, texture_);
        texture_ = gfx::TextureHandle{};
    }

    const Glyph& Font::glyph(int index) const {
        // Two assertions rather than one condition. A shaped glyph that the
        // atlas does not hold arrives as -1, so the two failures have different
        // causes and deserve different messages.
        ENGINE_ASSERT(index >= 0, "A negative glyph index reached ui::Font::glyph(). A shaped "
                                  "glyph the atlas does not hold reports -1, and the caller "
                                  "has to skip it.");
        const auto slot = static_cast<std::size_t>(index);
        ENGINE_ASSERT(slot < glyphs_.size(),
                      "A glyph index reached ui::Font::glyph() that the atlas does not hold.");
        return glyphs_[slot];
    }

    std::vector<ShapedGlyph> Font::shape(std::string_view text) const {
        std::vector<ShapedGlyph> result;
        if (hb_font_ == nullptr || text.empty()) {
            return result;
        }

        if (hb_buffer_ == nullptr) {
            hb_buffer_ = hb_buffer_create();
        } else {
            hb_buffer_clear_contents(hb_buffer_);
        }

        hb_buffer_add_utf8(hb_buffer_, text.data(), static_cast<int>(text.length()), 0, -1);
        // Latin, left to right, unless the text says otherwise. This is what
        // makes kerning and ligatures work at all, and it is also the line that
        // a script beyond Latin would have to change.
        hb_buffer_set_direction(hb_buffer_, HB_DIRECTION_LTR);
        hb_buffer_set_script(hb_buffer_, HB_SCRIPT_LATIN);
        hb_buffer_set_language(hb_buffer_, hb_language_from_string("en", -1));
        hb_buffer_guess_segment_properties(hb_buffer_);

        hb_shape(hb_font_, hb_buffer_, nullptr, 0);

        unsigned int count = 0;
        const hb_glyph_info_t* infos = hb_buffer_get_glyph_infos(hb_buffer_, &count);
        const hb_glyph_position_t* positions = hb_buffer_get_glyph_positions(hb_buffer_, &count);
        if (infos == nullptr || positions == nullptr) {
            return result;
        }

        result.reserve(count);
        for (unsigned int i = 0; i < count; ++i) {
            // HarfBuzz calls this field a codepoint, and after shaping it holds
            // a glyph index in the face rather than a character.
            const auto found = glyph_index_to_atlas_.find(infos[i].codepoint);
            ShapedGlyph shaped;
            shaped.glyph = (found == glyph_index_to_atlas_.end()) ? -1 : found->second;
            shaped.advance_x = positions[i].x_advance / kFixedToPixels;
            shaped.advance_y = positions[i].y_advance / kFixedToPixels;
            shaped.offset_x = positions[i].x_offset / kFixedToPixels;
            shaped.offset_y = positions[i].y_offset / kFixedToPixels;
            result.push_back(shaped);
        }
        return result;
    }

    int Font::measure_width(std::string_view text) const {
        int width = 0;
        for (const ShapedGlyph& shaped : shape(text)) {
            width += shaped.advance_x;
        }
        return width;
    }

    void Font::wrap_segment(std::string_view segment, int width,
                            std::vector<WrappedLine>& lines) const {
        const auto emit = [this, &lines](std::string_view line) {
            if (!line.empty()) {
                lines.push_back(WrappedLine{ measure_width(line), line });
            }
        };

        // Greedily fill a line with whole words. The candidate is measured as
        // one string rather than as a sum of word widths, because shaping
        // across a space can kern.
        std::size_t line_begin = std::string_view::npos;
        std::size_t line_end = 0;

        for (const WordSpan& word : split_words(segment)) {
            if (line_begin == std::string_view::npos) {
                line_begin = word.begin;
                line_end = word.end;
                continue;
            }

            const std::string_view candidate = segment.substr(line_begin, word.end - line_begin);
            if (width > 0 && measure_width(candidate) > width) {
                // This word does not fit beside the ones before it, so it
                // starts the next line. A word wider than the limit gets a line
                // to itself rather than being cut.
                emit(segment.substr(line_begin, line_end - line_begin));
                line_begin = word.begin;
            }
            line_end = word.end;
        }

        if (line_begin != std::string_view::npos) {
            emit(segment.substr(line_begin, line_end - line_begin));
        }
    }

    std::vector<WrappedLine> Font::wrap(std::string_view text, int width) const {
        std::vector<WrappedLine> lines;

        // This is a rewrite rather than a port. The reference walks the string
        // once and backtracks the loop counter to the last break it passed.
        // Splitting the words out first says the same thing without the
        // backtracking, and it cannot run an index past the start of a line.
        std::size_t segment_begin = 0;
        while (segment_begin <= text.size()) {
            std::size_t newline = text.find('\n', segment_begin);
            if (newline == std::string_view::npos) {
                newline = text.size();
            }
            wrap_segment(text.substr(segment_begin, newline - segment_begin), width, lines);
            segment_begin = newline + 1;
        }

        return lines;
    }

}
