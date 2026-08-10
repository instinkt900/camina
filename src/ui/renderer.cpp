#include "ui/renderer.h"

#include "core/assert.h"
#include "core/log.h"
#include "ui/image.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cmath>

namespace engine::ui {

    namespace {

        // The swapchain is B8G8R8A8_SRGB, so the hardware encodes linear to
        // sRGB when it writes. A moth_ui colour is authored as sRGB, so it has
        // to come out of that curve here or the round trip brightens it.
        //
        // Alpha carries no curve and passes through untouched.
        float srgb_to_linear(float value) {
            constexpr float kKnee = 0.04045F;
            constexpr float kLowScale = 12.92F;
            constexpr float kOffset = 0.055F;
            constexpr float kGamma = 2.4F;

            if (value <= kKnee) {
                return value / kLowScale;
            }
            return std::pow((value + kOffset) / (1.0F + kOffset), kGamma);
        }

        moth_ui::IntRect intersect(const moth_ui::IntRect& outer, const moth_ui::IntRect& inner) {
            moth_ui::IntRect result;
            result.topLeft.x = std::max(outer.topLeft.x, inner.topLeft.x);
            result.topLeft.y = std::max(outer.topLeft.y, inner.topLeft.y);
            result.bottomRight.x = std::min(outer.bottomRight.x, inner.bottomRight.x);
            result.bottomRight.y = std::min(outer.bottomRight.y, inner.bottomRight.y);
            // An empty intersection must stay empty rather than invert. An
            // inverted rectangle reaches a scissor as an enormous unsigned
            // extent, so a clip that hides everything would show everything.
            //
            // break_batch() clamps as well, and deleting either guard alone
            // fails no test because the other still holds the invariant.
            // Deleting both fails one. Two guards for one invariant is on
            // purpose here, because the failure is silent and severe.
            result.bottomRight.x = std::max(result.topLeft.x, result.bottomRight.x);
            result.bottomRight.y = std::max(result.topLeft.y, result.bottomRight.y);
            return result;
        }

        moth_ui::Color lerp(const moth_ui::Color& from, const moth_ui::Color& to, float t) {
            return moth_ui::Color{ from.r + ((to.r - from.r) * t), from.g + ((to.g - from.g) * t),
                                   from.b + ((to.b - from.b) * t), from.a + ((to.a - from.a) * t) };
        }

        /// The width of a rectangle measured along a direction, which is what
        /// "transitionLength = 1.0 fills the rect" means.
        float support_width(float width, float height, float cos_angle, float sin_angle) {
            return (std::abs(width * cos_angle)) + (std::abs(height * sin_angle));
        }

    }

    Renderer::Renderer() {
        // Every stack carries a default that a pop must never remove. moth_ui
        // pops more than it pushes in places, and the reference backend guards
        // the same way.
        m_color.emplace_back(1.0F, 1.0F, 1.0F, 1.0F);
        m_blend.push_back(moth_ui::BlendMode::Replace);
        m_transform.push_back(moth_ui::FloatMat4x4::Identity());
        m_filter.push_back(moth_ui::TextureFilter::Linear);
    }

    void Renderer::begin(std::uint32_t width, std::uint32_t height) {
        m_vertices.clear();
        m_indices.clear();
        m_batches.clear();
        m_clip.clear();

        m_color.assign(1, moth_ui::Color{ 1.0F, 1.0F, 1.0F, 1.0F });
        m_blend.assign(1, moth_ui::BlendMode::Replace);
        m_transform.assign(1, moth_ui::FloatMat4x4::Identity());
        m_filter.assign(1, moth_ui::TextureFilter::Linear);

        m_width = std::max(width, 1U);
        m_height = std::max(height, 1U);

        // The first batch opens now, so a draw needs no special case for it.
        m_batches.push_back(Batch{ .first_index = 0,
                                   .index_count = 0,
                                   .blend = m_blend.back(),
                                   .texture = gfx::TextureHandle{},
                                   .filter = m_filter.back(),
                                   .clipped = false,
                                   .clip_x = 0,
                                   .clip_y = 0,
                                   .clip_width = 0,
                                   .clip_height = 0 });
    }

    void Renderer::end() {
        // A trailing batch that recorded nothing would cost a draw call for no
        // pixels, so drop it.
        if (!m_batches.empty() && m_batches.back().index_count == 0) {
            m_batches.pop_back();
        }
    }

    void Renderer::break_batch() {
        ENGINE_ASSERT(!m_batches.empty(), "A batch must be open before a break.");

        Batch next{};
        next.first_index = static_cast<std::uint32_t>(m_indices.size());
        next.index_count = 0;
        next.blend = m_blend.back();
        next.filter = m_filter.back();
        // The texture is not part of the current state. A push says what the
        // next quad may read, and only RenderImage knows which image that is,
        // so want_texture() sets it on the batch that is open by then.
        next.texture = m_batches.back().texture;
        if (!m_clip.empty()) {
            const moth_ui::IntRect& rect = m_clip.back();
            next.clipped = true;
            next.clip_x = rect.topLeft.x;
            next.clip_y = rect.topLeft.y;
            next.clip_width = static_cast<std::uint32_t>(std::max(0, rect.w()));
            next.clip_height = static_cast<std::uint32_t>(std::max(0, rect.h()));
        }

        // Reuse the open batch when it drew nothing, rather than leaving an
        // empty run behind. Several pushes in a row are common in a node tree.
        if (m_batches.back().index_count == 0) {
            m_batches.back() = next;
            return;
        }
        m_batches.push_back(next);
    }

    Renderer::QuadColors Renderer::plain_white() {
        const moth_ui::Color white{ 1.0F, 1.0F, 1.0F, 1.0F };
        return QuadColors{ .top_left = white,
                           .top_right = white,
                           .bottom_right = white,
                           .bottom_left = white };
    }

    void Renderer::want_texture(gfx::TextureHandle texture) {
        if (m_batches.empty()) {
            return;
        }
        Batch& open = m_batches.back();
        if (open.texture.value == texture.value) {
            return;
        }
        // A run that drew nothing has no geometry to keep, so it takes the new
        // texture rather than costing a draw call.
        if (open.index_count == 0) {
            open.texture = texture;
            return;
        }
        break_batch();
        m_batches.back().texture = texture;
    }

    void Renderer::add_quad(const Quad& quad, const QuadColors& colors) {
        // moth_ui decides the call order, not this engine, so a draw can
        // arrive before begin() or after end(). Both leave no open batch, and
        // m_batches.back() would then read an empty vector. ENGINE_ASSERT
        // compiles out of a Release build, so this needs a real return.
        if (m_batches.empty()) {
            return;
        }

        const moth_ui::FloatMat4x4& transform = m_transform.back();
        const moth_ui::Color& tint = m_color.back();

        const auto base = static_cast<std::uint32_t>(m_vertices.size());

        const std::array<moth_ui::FloatVec2, 4> corners{ {
            transform.TransformPoint({ quad.x0, quad.y0 }),
            transform.TransformPoint({ quad.x1, quad.y0 }),
            transform.TransformPoint({ quad.x1, quad.y1 }),
            transform.TransformPoint({ quad.x0, quad.y1 }),
        } };
        // The texture coordinates take the same corner order, and no transform.
        // A node transform moves the quad across the screen and leaves the part
        // of the image it shows alone.
        const std::array<moth_ui::FloatVec2, 4> texcoords{ {
            { quad.u0, quad.v0 },
            { quad.u1, quad.v0 },
            { quad.u1, quad.v1 },
            { quad.u0, quad.v1 },
        } };
        const std::array<moth_ui::Color, 4> corner_colors{ colors.top_left, colors.top_right,
                                                           colors.bottom_right,
                                                           colors.bottom_left };

        for (std::size_t i = 0; i < corners.size(); ++i) {
            // The tint composes here as well as on the stack, because a colour
            // a caller passed to RenderGradientRect never went through
            // PushColor.
            const moth_ui::Color c = corner_colors[i] * tint;
            m_vertices.push_back(Vertex{ .x = corners[i].x,
                                         .y = corners[i].y,
                                         .u = texcoords[i].x,
                                         .v = texcoords[i].y,
                                         .r = srgb_to_linear(c.r),
                                         .g = srgb_to_linear(c.g),
                                         .b = srgb_to_linear(c.b),
                                         .a = c.a });
        }

        // Two triangles that share the diagonal from the top left corner.
        static constexpr std::array<std::uint32_t, 6> kQuadOrder{ 0, 1, 2, 0, 2, 3 };
        for (const std::uint32_t offset : kQuadOrder) {
            m_indices.push_back(base + offset);
        }
        m_batches.back().index_count += static_cast<std::uint32_t>(kQuadOrder.size());
    }

    void Renderer::PushBlendMode(moth_ui::BlendMode mode) {
        m_blend.push_back(mode);
        break_batch();
    }

    void Renderer::PopBlendMode() {
        if (m_blend.size() > 1) {
            m_blend.pop_back();
            break_batch();
        }
    }

    void Renderer::PushColor(const moth_ui::Color& color) {
        // Compose, so a tint on a parent reaches every child. This is what the
        // reference backend does, and it is not what "push" suggests.
        m_color.push_back(m_color.back() * color);
    }

    void Renderer::PopColor() {
        if (m_color.size() > 1) {
            m_color.pop_back();
        }
    }

    void Renderer::PushTransform(const moth_ui::FloatMat4x4& transform) {
        // Replace rather than compose. Node::GetWorldTransform already walked
        // the parent chain, which the moth_ui header states plainly.
        //
        // This costs no batch break, because add_quad applies the matrix to
        // each corner as it records. That is what lets a whole layout of nodes
        // collapse into one draw.
        m_transform.push_back(transform);
    }

    void Renderer::PopTransform() {
        if (m_transform.size() > 1) {
            m_transform.pop_back();
        }
    }

    void Renderer::PushClip(const moth_ui::IntRect& rect) {
        m_clip.push_back(m_clip.empty() ? rect : intersect(m_clip.back(), rect));
        break_batch();
    }

    void Renderer::PopClip() {
        if (m_clip.empty()) {
            return;
        }
        m_clip.pop_back();
        break_batch();
    }

    void Renderer::PushTextureFilter(moth_ui::TextureFilter filter) {
        // Invalid is the moth_ui sentinel for "nothing was set", and a layout
        // that saved no filter loads it. Keep the one already in force rather
        // than reading a sentinel as a filter.
        const moth_ui::TextureFilter wanted =
            filter == moth_ui::TextureFilter::Invalid ? m_filter.back() : filter;
        const bool changed = wanted != m_filter.back();
        m_filter.push_back(wanted);
        // Only a change breaks the run. NodeImage pushes a filter around every
        // image it draws, so breaking on the push itself would give two images
        // of one texture two draws for no reason.
        if (changed) {
            break_batch();
        }
    }

    void Renderer::PopTextureFilter() {
        if (m_filter.size() > 1) {
            const moth_ui::TextureFilter old = m_filter.back();
            m_filter.pop_back();
            if (old != m_filter.back()) {
                break_batch();
            }
        }
    }

    void Renderer::RenderRect(const moth_ui::IntRect& rect) {
        // An outline of four one-pixel bars. moth_ui has no line primitive, and
        // a bar is a quad the batch already knows how to carry.
        constexpr float kBorder = 1.0F;
        const auto x0 = static_cast<float>(rect.topLeft.x);
        const auto y0 = static_cast<float>(rect.topLeft.y);
        const auto x1 = static_cast<float>(rect.bottomRight.x);
        const auto y1 = static_cast<float>(rect.bottomRight.y);
        if (x1 <= x0 || y1 <= y0) {
            return;
        }
        want_texture(gfx::TextureHandle{});
        const QuadColors white = plain_white();

        add_quad(Quad{ .x0 = x0, .y0 = y0, .x1 = x1, .y1 = y0 + kBorder }, white);
        add_quad(Quad{ .x0 = x0, .y0 = y1 - kBorder, .x1 = x1, .y1 = y1 }, white);
        add_quad(Quad{ .x0 = x0, .y0 = y0 + kBorder, .x1 = x0 + kBorder, .y1 = y1 - kBorder },
                 white);
        add_quad(Quad{ .x0 = x1 - kBorder, .y0 = y0 + kBorder, .x1 = x1, .y1 = y1 - kBorder },
                 white);
    }

    void Renderer::RenderFilledRect(const moth_ui::IntRect& rect) {
        want_texture(gfx::TextureHandle{});
        add_quad(Quad{ .x0 = static_cast<float>(rect.topLeft.x),
                       .y0 = static_cast<float>(rect.topLeft.y),
                       .x1 = static_cast<float>(rect.bottomRight.x),
                       .y1 = static_cast<float>(rect.bottomRight.y) },
                 plain_white());
    }

    void Renderer::RenderGradientRect(const moth_ui::IntRect& rect,
                                      const moth_ui::LinearGradient& gradient) {
        const auto x0 = static_cast<float>(rect.topLeft.x);
        const auto y0 = static_cast<float>(rect.topLeft.y);
        const auto x1 = static_cast<float>(rect.bottomRight.x);
        const auto y1 = static_cast<float>(rect.bottomRight.y);
        const float width = x1 - x0;
        const float height = y1 - y0;

        const float cos_angle = std::cos(gradient.angle);
        const float sin_angle = std::sin(gradient.angle);

        // Where t crosses 0.5, as a point rather than a fraction.
        const float mid_x = x0 + (width * gradient.midpoint.x);
        const float mid_y = y0 + (height * gradient.midpoint.y);

        // transitionLength scales the run of the lerp along the axis, and 1.0
        // is defined as filling the rect.
        const float axis = support_width(width, height, cos_angle, sin_angle) *
                           gradient.transitionLength;

        // t is an affine function of position, and a triangle interpolates an
        // affine function exactly. So evaluating it at the four corners
        // reproduces the gradient for any angle, with no extra geometry.
        //
        // The clamp is the one part that is not affine. Where the lerp ends
        // inside the rect, the corner colours understate the flat region and
        // this is an approximation. transitionLength below 1.0 is what does
        // that, and a sharp step is its limit.
        const auto color_at = [&](float x, float y) {
            constexpr float kFlat = 1.0e-6F;
            if (axis <= kFlat) {
                // A zero run is a step at the midpoint rather than a divide by
                // zero.
                const float side = ((x - mid_x) * cos_angle) + ((y - mid_y) * sin_angle);
                return side < 0.0F ? gradient.startColor : gradient.endColor;
            }
            const float along = ((x - mid_x) * cos_angle) + ((y - mid_y) * sin_angle);
            const float t = std::clamp(0.5F + (along / axis), 0.0F, 1.0F);
            return lerp(gradient.startColor, gradient.endColor, t);
        };

        want_texture(gfx::TextureHandle{});
        add_quad(Quad{ .x0 = x0, .y0 = y0, .x1 = x1, .y1 = y1 },
                 QuadColors{ .top_left = color_at(x0, y0),
                             .top_right = color_at(x1, y0),
                             .bottom_right = color_at(x1, y1),
                             .bottom_left = color_at(x0, y1) });
    }

    void Renderer::RenderImage(const moth_ui::IImage& image, const moth_ui::IntRect& source_rect,
                               const moth_ui::IntRect& dest_rect,
                               moth_ui::ImageScaleType scale_type, float scale) {
        // Another backend's image would carry another backend's texture. The
        // reference backend casts the same way and returns on a miss.
        const auto* ours = dynamic_cast<const Image*>(&image);
        if (ours == nullptr) {
            ENGINE_LOG_ERROR("A layout drew an image this renderer did not make.");
            return;
        }

        const auto width = static_cast<float>(ours->GetWidth());
        const auto height = static_cast<float>(ours->GetHeight());
        if (width <= 0.0F || height <= 0.0F) {
            return;
        }
        if (dest_rect.w() <= 0 || dest_rect.h() <= 0) {
            return;
        }

        const auto x0 = static_cast<float>(dest_rect.topLeft.x);
        const auto y0 = static_cast<float>(dest_rect.topLeft.y);
        const auto x1 = static_cast<float>(dest_rect.bottomRight.x);
        const auto y1 = static_cast<float>(dest_rect.bottomRight.y);

        // The source rectangle is in texels of the whole image, so this is the
        // one place the size of the texture is needed.
        const float u0 = static_cast<float>(source_rect.topLeft.x) / width;
        const float v0 = static_cast<float>(source_rect.topLeft.y) / height;
        float u1 = static_cast<float>(source_rect.bottomRight.x) / width;
        float v1 = static_cast<float>(source_rect.bottomRight.y) / height;

        switch (scale_type) {
        case moth_ui::ImageScaleType::Stretch:
            // The source region covers the destination once, whatever the two
            // sizes are.
            break;

        case moth_ui::ImageScaleType::Tile: {
            // One tile is the source region at `scale`, and the coordinates run
            // past 1 for as many tiles as the destination holds. The sampler
            // repeats, so this is still one quad.
            //
            // Repeat wraps the whole texture rather than the source region, so
            // this is right only when the region is the whole image. Every
            // engine::ui::Image is one cooked texture and never an atlas page,
            // and NodeImage fills an unset source rectangle with the full size.
            const bool whole_image = source_rect.topLeft.x == 0 &&
                                     source_rect.topLeft.y == 0 &&
                                     static_cast<float>(source_rect.bottomRight.x) == width &&
                                     static_cast<float>(source_rect.bottomRight.y) == height;
            ENGINE_ASSERT(whole_image,
                          "Tile draws a part of an image wrongly, because the sampler repeats "
                          "the whole texture. Cook the region as its own image.");
            ENGINE_ASSERT(scale > 0.0F, "Tile needs a scale above zero.");
            if (!whole_image || scale <= 0.0F) {
                return;
            }
            u1 = u0 + ((x1 - x0) / (width * scale));
            v1 = v0 + ((y1 - y0) / (height * scale));
            break;
        }

        case moth_ui::ImageScaleType::NineSlice:
            // moth_ui cuts a nine-slice into nine Stretch calls in
            // NodeImage::DrawInternal, so this backend never sees one. Reaching
            // here means a new caller, and drawing it stretched would show a
            // distorted border rather than nothing.
            ENGINE_ASSERT(false, "RenderImage has no NineSlice. moth_ui NodeImage cuts one into "
                                 "nine Stretch calls before it reaches a backend.");
            return;
        }

        want_texture(ours->texture());
        add_quad(Quad{ .x0 = x0, .y0 = y0, .x1 = x1, .y1 = y1, .u0 = u0, .v0 = v0, .u1 = u1, .v1 = v1 },
                 plain_white());
    }

    void Renderer::RenderText(std::string_view text, moth_ui::IFont& font,
                              moth_ui::TextHorizAlignment horizontal,
                              moth_ui::TextVertAlignment vertical,
                              const moth_ui::IntRect& dest_rect) {
        // Issue #199, and the heaviest part of the milestone. moth_ui::IFont
        // declares no method at all, so the engine owns rasterization, the
        // atlas, measurement and both alignments.
        (void)text;
        (void)font;
        (void)horizontal;
        (void)vertical;
        (void)dest_rect;
    }

    void Renderer::SetRendererLogicalSize(const moth_ui::IntVec2& size) {
        m_width = static_cast<std::uint32_t>(std::max(1, size.x));
        m_height = static_cast<std::uint32_t>(std::max(1, size.y));
    }

}
