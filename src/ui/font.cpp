#include "ui/font.h"

#include "core/assert.h"
#include "core/log.h"

#include <ft2build.h>
#include FT_FREETYPE_H

#include <hb-ft.h>
#include <hb.h>
#include <stb_rect_pack.h>

#include <cctype>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>

namespace engine::ui {

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

        /**
         * Turns a 26.6 fixed point value into whole pixels, rounding to nearest.
         *
         * Truncating instead would bias every advance the same way, and
         * measure_width() sums them. A 60-character line then measures about a
         * pixel short, and wrap() breaks on that width. Integer division also
         * truncates toward zero, so it would round a negative advance the
         * opposite way from a positive one.
         */
        int round_fixed(int fixed) {
            const int half = kFixedToPixels / 2;
            return (fixed >= 0) ? ((fixed + half) / kFixedToPixels)
                                : -((-fixed + half) / kFixedToPixels);
        }

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
         * Rasterizes one glyph of the face and keeps its bitmap.
         *
         * One glyph rather than the whole covered set, because the atlas packs
         * on demand now. The caller decides which glyphs it wants and when.
         *
         * @return The glyph, or nothing when the face will not render it. A
         * refusal is reported where it needs saying and quiet where it does
         * not: a face with no glyph for a codepoint is normal.
         */
        std::optional<PendingGlyph> rasterize_one(FT_Face face, FT_UInt glyph_index) {
            if (FT_Load_Glyph(face, glyph_index, FT_LOAD_RENDER) != 0) {
                return std::nullopt;
            }

            const FT_GlyphSlot slot = face->glyph;

            // FT_LOAD_RENDER picks the hinting target, not the bitmap format. A
            // bitmap-only face can answer with one bit for each pixel, and
            // reading that as one byte for each pixel turns a glyph into noise
            // eight times too wide. Refuse it rather than draw it wrongly. An
            // empty bitmap is fine and normal: a space has an advance and no
            // coverage.
            if (slot->bitmap.width != 0 && slot->bitmap.rows != 0 &&
                slot->bitmap.pixel_mode != FT_PIXEL_MODE_GRAY) {
                ENGINE_LOG_WARN("The face answered glyph {} in pixel mode {}, and this "
                                "reads 8-bit gray only. The glyph is skipped.",
                                glyph_index, static_cast<int>(slot->bitmap.pixel_mode));
                return std::nullopt;
            }

            PendingGlyph glyph;
            glyph.index = glyph_index;
            glyph.width = static_cast<int>(slot->bitmap.width);
            glyph.height = static_cast<int>(slot->bitmap.rows);
            glyph.bearing_x = static_cast<int>(slot->metrics.horiBearingX / kFixedToPixels);
            // Negative, because the pen sits on the baseline and a screen
            // rectangle grows downward.
            glyph.bearing_y = -static_cast<int>(slot->metrics.horiBearingY / kFixedToPixels);
            glyph.advance_x = static_cast<int>(slot->advance.x / kFixedToPixels);
            glyph.advance_y = static_cast<int>(slot->advance.y / kFixedToPixels);

            glyph.coverage.resize(static_cast<std::size_t>(glyph.width) *
                                  static_cast<std::size_t>(glyph.height));

            // The sign of the pitch is the row order. A positive pitch puts the
            // top row first, and a negative one puts the bottom row first and
            // counts backward. Taking the absolute value alone would read the
            // right bytes in the wrong order and draw every glyph upside down,
            // so the start row moves with the sign.
            const int pitch = slot->bitmap.pitch;
            const std::uint8_t* first_row = slot->bitmap.buffer;
            if (pitch < 0) {
                first_row += static_cast<std::ptrdiff_t>(pitch) * (glyph.height - 1);
            }
            const int stride = (pitch < 0) ? -pitch : pitch;
            for (int y = 0; y < glyph.height; ++y) {
                for (int x = 0; x < glyph.width; ++x) {
                    glyph.coverage[(static_cast<std::size_t>(y) *
                                    static_cast<std::size_t>(glyph.width)) +
                                   static_cast<std::size_t>(x)] = first_row[x + (y * stride)];
                }
            }
            return glyph;
        }

        /**
         * Every face glyph index the preload range names, in codepoint order.
         *
         * Several codepoints can share one glyph, and the atlas holds it once.
         */
        std::vector<FT_UInt> preload_indices(FT_Face face) {
            std::vector<FT_UInt> wanted;
            std::set<FT_UInt> seen;
            for (char32_t codepoint = kCoverageFirst; codepoint <= kCoverageLast; ++codepoint) {
                const FT_UInt glyph_index = FT_Get_Char_Index(face, codepoint);
                if (glyph_index == 0) {
                    // The face carries no glyph for this codepoint.
                    continue;
                }
                if (seen.insert(glyph_index).second) {
                    wanted.push_back(glyph_index);
                }
            }
            return wanted;
        }

    } // namespace

    /**
     * The live rectangle packer, which the atlas keeps between glyphs.
     *
     * `stbrp_context` holds pointers into its node array, so the two travel
     * together and neither one may be copied. Font holds this behind a pointer
     * for that reason, and so that font.h names no stb type.
     */
    struct Font::Packer {
        stbrp_context context{};
        std::vector<stbrp_node> nodes;

        /// Starts a packer over an empty square of the given size.
        void reset(int size) {
            nodes.assign(static_cast<std::size_t>(size), stbrp_node{});
            stbrp_init_target(&context, size, size, nodes.data(),
                              static_cast<int>(nodes.size()));
        }

        /// Finds room for one rectangle. Reports false when the square is full.
        [[nodiscard]] bool place(int width, int height, int& out_x, int& out_y) {
            stbrp_rect rect{};
            rect.w = static_cast<stbrp_coord>(width);
            rect.h = static_cast<stbrp_coord>(height);
            if (stbrp_pack_rects(&context, &rect, 1) == 0 || rect.was_packed == 0) {
                return false;
            }
            out_x = rect.x;
            out_y = rect.y;
            return true;
        }
    };

    Font::Font() = default;

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

    void Font::release_face() {
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

        // The measurements go with the face. Leaving them behind would let a
        // Font that failed to load still answer a width, which reads as a
        // working font that draws nothing.
        glyphs_.clear();
        glyph_index_to_atlas_.clear();
        uploaded_glyphs_.clear();
        uploaded_index_.clear();
        wanted_.clear();
        packed_order_.clear();
        pixels_.clear();
        packer_.reset();
        atlas_changed_ = false;
        atlas_width_ = 0;
        atlas_height_ = 0;
        line_height_ = 0;
        ascent_ = 0;
        descent_ = 0;
        underline_ = 0;
    }

    Font::~Font() {
        release_face();
        // Only an atlas this uploaded. The assert is here to catch a device
        // resource that nothing freed, and a handle that came through
        // borrow_texture() is not one: it belongs to somebody else and this
        // never had anything to free.
        ENGINE_ASSERT(!owns_texture_,
                      "A ui::Font still holds the atlas texture it uploaded. Call destroy() "
                      "first.");
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

        // A reload starts clean. Without this, loading a second file over the
        // first leaks the first face and its HarfBuzz font.
        release_face();

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
            release_face();
            return false;
        }

        hb_font_ = hb_ft_font_create(face_, nullptr);
        if (hb_font_ == nullptr) {
            ENGINE_LOG_ERROR("HarfBuzz would not take the font {}.", path_text);
            release_face();
            return false;
        }

        // The shaping buffer is made once here rather than on the first shape().
        // That keeps the lazy branch out of the measuring path, and it makes
        // shape() touch nothing that a failed load left behind.
        hb_buffer_ = hb_buffer_create();

        if (!build_atlas()) {
            ENGINE_LOG_ERROR("The glyphs of {} at {} pixels would not pack into an atlas of "
                             "{} texels square.",
                             path_text, pixel_size, kMaxAtlasSize);
            // Every other failure path releases the face, and this one used to
            // return with it still open. That leaked on a retry and left the
            // object able to shape and measure but not to draw.
            release_face();
            return false;
        }

        line_height_ = static_cast<int>(face_->size->metrics.height / kFixedToPixels);
        ascent_ = static_cast<int>(face_->size->metrics.ascender / kFixedToPixels);
        descent_ = static_cast<int>(face_->size->metrics.descender / kFixedToPixels);
        underline_ = static_cast<int>(
            FT_MulFix(face_->underline_position, face_->size->metrics.y_scale) / kFixedToPixels);

        // The preload set is readable straight away, so measure_width() and
        // wrap() work with no device the way they always have. upload() then
        // uploads exactly these bytes. See issue #213.
        publish();

        ENGINE_LOG_INFO("Font {} at {} px: {} glyphs in a {}x{} atlas, line height {}.",
                        path.filename().string(), pixel_size, glyphs_.size(), atlas_width_,
                        atlas_height_, line_height_);
        return true;
    }

    void Font::reset_atlas(int size) {
        if (packer_ == nullptr) {
            packer_ = std::make_unique<Packer>();
        }
        atlas_width_ = size;
        atlas_height_ = size;

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

        packer_->reset(size);
    }

    void Font::blit(const PendingGlyph& source, int x0, int y0, Glyph& out) {
        const std::size_t stride =
            static_cast<std::size_t>(atlas_width_) * static_cast<std::size_t>(kBytesPerTexel);
        for (int y = 0; y < source.height; ++y) {
            for (int x = 0; x < source.width; ++x) {
                const std::size_t target = (static_cast<std::size_t>(y0 + y) * stride) +
                                           (static_cast<std::size_t>(x0 + x) * kBytesPerTexel) +
                                           3;
                pixels_[target] = source.coverage[(static_cast<std::size_t>(y) *
                                                   static_cast<std::size_t>(source.width)) +
                                                  static_cast<std::size_t>(x)];
            }
        }

        const auto atlas_width_f = static_cast<float>(atlas_width_);
        const auto atlas_height_f = static_cast<float>(atlas_height_);

        out.width = source.width;
        out.height = source.height;
        out.bearing_x = source.bearing_x;
        out.bearing_y = source.bearing_y;
        out.advance_x = source.advance_x;
        out.advance_y = source.advance_y;
        // The coordinates cover the glyph and not its border, so a sampler
        // never reads the transparent ring on purpose.
        out.u0 = static_cast<float>(x0) / atlas_width_f;
        out.v0 = static_cast<float>(y0) / atlas_height_f;
        out.u1 = static_cast<float>(x0 + source.width) / atlas_width_f;
        out.v1 = static_cast<float>(y0 + source.height) / atlas_height_f;
    }

    bool Font::grow_atlas() {
        if (atlas_width_ >= kMaxAtlasSize) {
            ENGINE_LOG_ERROR("The font atlas is full at {} texels square, which is the largest "
                             "a Vulkan 1.3 device is guaranteed to accept. {} glyphs are "
                             "packed. A further glyph will not draw.",
                             atlas_width_, glyphs_.size());
            return false;
        }

        // Everything is packed again from nothing rather than moved. The
        // packer has no way to relocate a rectangle it already placed, and a
        // glyph the face can rasterize once it can rasterize twice.
        const std::vector<std::uint32_t> again = packed_order_;
        const int size = atlas_width_ * 2;

        glyphs_.clear();
        glyph_index_to_atlas_.clear();
        packed_order_.clear();
        reset_atlas(size);

        for (const std::uint32_t glyph_index : again) {
            if (pack_glyph(glyph_index) < 0) {
                // A set that fitted the smaller square and not the larger one
                // is a packer fault rather than a full atlas, and carrying on
                // would lose glyphs silently.
                ENGINE_LOG_ERROR("A glyph that was packed at {} texels would not pack at {}.",
                                 size / 2, size);
                return false;
            }
        }
        return true;
    }

    std::uint32_t Font::glyph_index_for(char32_t codepoint) const {
        if (face_ == nullptr) {
            return 0;
        }
        return FT_Get_Char_Index(face_, codepoint);
    }

    int Font::pack_glyph(std::uint32_t glyph_index) {
        if (face_ == nullptr) {
            return -1;
        }
        const auto found = glyph_index_to_atlas_.find(glyph_index);
        if (found != glyph_index_to_atlas_.end()) {
            return found->second;
        }

        const std::optional<PendingGlyph> source =
            rasterize_one(face_, static_cast<FT_UInt>(glyph_index));
        if (!source) {
            return -1;
        }

        int x0 = 0;
        int y0 = 0;
        const int wanted_width = source->width + (kBorderTexels * 2);
        const int wanted_height = source->height + (kBorderTexels * 2);
        if (!packer_->place(wanted_width, wanted_height, x0, y0)) {
            // grow_atlas() packs every glyph again, this one included when it
            // is already in packed_order_. It is not, so it is packed here
            // after the growth, against the fresh packer.
            if (!grow_atlas()) {
                return -1;
            }
            if (!packer_->place(wanted_width, wanted_height, x0, y0)) {
                ENGINE_LOG_ERROR("One glyph does not fit an atlas of {} texels square.",
                                 atlas_width_);
                return -1;
            }
        }

        const auto index = static_cast<int>(glyphs_.size());
        glyphs_.emplace_back();
        blit(*source, x0 + kBorderTexels, y0 + kBorderTexels, glyphs_.back());
        glyph_index_to_atlas_[glyph_index] = index;
        packed_order_.push_back(glyph_index);
        atlas_changed_ = true;
        return index;
    }

    bool Font::build_atlas() {
        reset_atlas(kMinAtlasSize);

        const std::vector<FT_UInt> wanted = preload_indices(face_);
        if (wanted.empty()) {
            ENGINE_LOG_ERROR("The face carries no glyph between U+{:04X} and U+{:04X}.",
                             static_cast<std::uint32_t>(kCoverageFirst),
                             static_cast<std::uint32_t>(kCoverageLast));
            return false;
        }

        for (const FT_UInt glyph_index : wanted) {
            // A glyph the face refuses is skipped, the way it always was. A
            // full atlas is not, because that loses the rest of the range.
            if (pack_glyph(glyph_index) < 0 && atlas_width_ >= kMaxAtlasSize) {
                return false;
            }
        }
        return !glyphs_.empty();
    }

    bool Font::upload(gfx::Device* device) {
        if (device == nullptr) {
            // create_texture() traps on a null device rather than reporting
            // one, and a caller that never opened a device deserves a message
            // instead of a stopped process.
            ENGINE_LOG_ERROR("A font atlas was uploaded with no device.");
            return false;
        }
        if (pixels_.empty()) {
            ENGINE_LOG_ERROR("A font atlas was uploaded before it was loaded.");
            return false;
        }
        if (texture_.valid()) {
            // Already uploaded. Anything packed since then is in pixels_ and
            // not in the texture, and atlas_changed() still says so. Replacing
            // the texture is the second half of issue #213, because freeing one
            // a frame in flight is still reading is a real error.
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
        owns_texture_ = true;
        publish();
        atlas_changed_ = false;
        return true;
    }

    void Font::publish() {
        // What a caller may read. glyph() and shape() answer from these, so
        // they move only here: at the end of load(), where the preload set is
        // ready to measure, and after each upload, where the bytes they
        // describe have just reached the device.
        uploaded_glyphs_ = glyphs_;
        uploaded_index_ = glyph_index_to_atlas_;
    }

    bool Font::refresh(gfx::Device* device, gfx::TextureHandle& out_retired) {
        out_retired = gfx::TextureHandle{};
        if (device == nullptr || face_ == nullptr) {
            return false;
        }

        // Whatever shaping could not find. Taken first, because pack_glyph()
        // may grow the atlas and a growth repacks everything.
        const std::set<std::uint32_t> asked = std::move(wanted_);
        wanted_.clear();
        for (const std::uint32_t glyph_index : asked) {
            // A refusal is remembered by the map staying empty for it, so the
            // next shape() asks again. That is one wasted rasterize attempt for
            // each frame that draws a glyph the face does not carry, and the
            // alternative is a second map of failures for a case a real font
            // does not have.
            (void)pack_glyph(glyph_index);
        }

        if (!atlas_changed_) {
            return false;
        }
        if (!texture_.valid()) {
            // Nothing has been uploaded yet, so there is nothing to replace.
            return upload(device);
        }
        if (!owns_texture_) {
            // borrow_texture() handed this one over. Replacing it would free
            // somebody else's texture, and a test that borrows never draws.
            return false;
        }

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
        gfx::TextureHandle fresh;
        if (!gfx::succeeded(gfx::create_texture(device, desc, &fresh))) {
            ENGINE_LOG_ERROR("The font atlas texture was not replaced, so {} glyphs will not "
                             "draw until it is.",
                             glyphs_.size() - uploaded_glyphs_.size());
            return false;
        }

        // The caller frees this behind the frames in flight. A command list
        // recorded this frame still names it.
        out_retired = texture_;
        texture_ = fresh;
        publish();
        atlas_changed_ = false;
        return true;
    }

    void Font::destroy(gfx::Device* device) {
        // Only what upload() made. A handle that came through borrow_texture()
        // belongs to somebody else.
        if (texture_.valid() && owns_texture_) {
            gfx::destroy_texture(device, texture_);
        }
        texture_ = gfx::TextureHandle{};
        owns_texture_ = false;
    }

    void Font::borrow_texture(gfx::TextureHandle texture) {
        ENGINE_ASSERT(!owns_texture_,
                      "borrow_texture() would drop an atlas this font uploaded. Call destroy() "
                      "first.");
        texture_ = texture;
        // Borrowing says "this handle holds the atlas as it stands", so the
        // working atlas becomes the uploaded one. Without this, glyph() and
        // shape() would answer for a texture nobody claimed and every glyph
        // would report -1. See issue #213.
        if (texture.valid()) {
            publish();
        }
    }

    const Glyph& Font::glyph(int index) const {
        // Two assertions rather than one condition. A shaped glyph that the
        // atlas does not hold arrives as -1, so the two failures have different
        // causes and deserve different messages.
        ENGINE_ASSERT(index >= 0, "A negative glyph index reached ui::Font::glyph(). A shaped "
                                  "glyph the atlas does not hold reports -1, and the caller "
                                  "has to skip it.");
        const auto slot = static_cast<std::size_t>(index);
        ENGINE_ASSERT(slot < uploaded_glyphs_.size(),
                      "A glyph index reached ui::Font::glyph() that the atlas does not hold.");
        return uploaded_glyphs_[slot];
    }

    std::vector<ShapedGlyph> Font::shape(std::string_view text) const {
        std::vector<ShapedGlyph> result;
        if (hb_font_ == nullptr || text.empty()) {
            return result;
        }

        hb_buffer_clear_contents(hb_buffer_);

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
            const auto found = uploaded_index_.find(infos[i].codepoint);
            ShapedGlyph shaped;
            if (found == uploaded_index_.end()) {
                // The texture does not hold it. Remember that somebody wanted
                // it, so the next refresh() packs and uploads it, and report -1
                // so this frame skips it rather than drawing a glyph from
                // wherever that atlas region happens to be. See issue #213.
                shaped.glyph = -1;
                wanted_.insert(infos[i].codepoint);
            } else {
                shaped.glyph = found->second;
            }
            shaped.advance_x = round_fixed(positions[i].x_advance);
            shaped.advance_y = round_fixed(positions[i].y_advance);
            shaped.offset_x = round_fixed(positions[i].x_offset);
            shaped.offset_y = round_fixed(positions[i].y_offset);
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

        if (line_begin == std::string_view::npos) {
            // The segment carried no word. It still gets a line, because a
            // newline the author typed is a line the reader expects to see.
            // Dropping it collapses a paragraph break and moves every line
            // after it up by one line height.
            lines.push_back(WrappedLine{ 0, segment.substr(0, 0) });
            return;
        }
        emit(segment.substr(line_begin, line_end - line_begin));
    }

    std::vector<WrappedLine> Font::wrap(std::string_view text, int width) const {
        std::vector<WrappedLine> lines;

        // Empty text is no lines at all, rather than one empty line. Every
        // other input gives at least one line for each newline-separated part.
        if (text.empty()) {
            return lines;
        }

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
