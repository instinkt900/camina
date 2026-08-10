#include "ui/renderer.h"

#include "core/assert.h"

#include <algorithm>
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

    void Renderer::add_quad(float x0, float y0, float x1, float y1,
                            const moth_ui::Color& top_left, const moth_ui::Color& top_right,
                            const moth_ui::Color& bottom_right,
                            const moth_ui::Color& bottom_left) {
        const moth_ui::FloatMat4x4& transform = m_transform.back();
        const moth_ui::Color& tint = m_color.back();

        const auto base = static_cast<std::uint32_t>(m_vertices.size());

        const moth_ui::FloatVec2 corners[4] = {
            transform.TransformPoint({ x0, y0 }),
            transform.TransformPoint({ x1, y0 }),
            transform.TransformPoint({ x1, y1 }),
            transform.TransformPoint({ x0, y1 }),
        };
        const moth_ui::Color colors[4] = { top_left, top_right, bottom_right, bottom_left };

        for (int i = 0; i < 4; ++i) {
            // The tint composes here as well as on the stack, because a colour
            // a caller passed to RenderGradientRect never went through
            // PushColor.
            const moth_ui::Color c = colors[i] * tint;
            m_vertices.push_back(Vertex{ .x = corners[i].x,
                                         .y = corners[i].y,
                                         .r = srgb_to_linear(c.r),
                                         .g = srgb_to_linear(c.g),
                                         .b = srgb_to_linear(c.b),
                                         .a = c.a });
        }

        const std::uint32_t order[6] = { 0, 1, 2, 0, 2, 3 };
        for (const std::uint32_t offset : order) {
            m_indices.push_back(base + offset);
        }
        m_batches.back().index_count += 6;
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
        m_filter.push_back(filter);
    }

    void Renderer::PopTextureFilter() {
        if (m_filter.size() > 1) {
            m_filter.pop_back();
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
        const moth_ui::Color white{ 1.0F, 1.0F, 1.0F, 1.0F };

        add_quad(x0, y0, x1, y0 + kBorder, white, white, white, white);
        add_quad(x0, y1 - kBorder, x1, y1, white, white, white, white);
        add_quad(x0, y0 + kBorder, x0 + kBorder, y1 - kBorder, white, white, white, white);
        add_quad(x1 - kBorder, y0 + kBorder, x1, y1 - kBorder, white, white, white, white);
    }

    void Renderer::RenderFilledRect(const moth_ui::IntRect& rect) {
        const moth_ui::Color white{ 1.0F, 1.0F, 1.0F, 1.0F };
        add_quad(static_cast<float>(rect.topLeft.x), static_cast<float>(rect.topLeft.y),
                 static_cast<float>(rect.bottomRight.x), static_cast<float>(rect.bottomRight.y),
                 white, white, white, white);
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

        add_quad(x0, y0, x1, y1, color_at(x0, y0), color_at(x1, y0), color_at(x1, y1),
                 color_at(x0, y1));
    }

    void Renderer::RenderImage(const moth_ui::IImage& image, const moth_ui::IntRect& source_rect,
                               const moth_ui::IntRect& dest_rect,
                               moth_ui::ImageScaleType scale_type, float scale) {
        // Issue #198. This increment records shapes only, and the shader that
        // draws them declares no sampler.
        (void)image;
        (void)source_rect;
        (void)dest_rect;
        (void)scale_type;
        (void)scale;
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
