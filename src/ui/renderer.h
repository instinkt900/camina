/**
 * @file renderer.h
 * @brief The moth_ui drawing surface, recorded as batches and drawn through gfx::.
 */

#pragma once

#include "gfx/types.h"

#include <moth_ui/graphics/irenderer.h>

#include <cstdint>
#include <vector>

namespace engine::ui {

    /// @brief One vertex of a recorded quad, in screen space and linear colour.
    struct Vertex {
        float x = 0.0F; ///< Screen x in pixels, origin at the top left.
        float y = 0.0F; ///< Screen y in pixels, y down.
        float u = 0.0F; ///< Texture x, normalized. Zero for a shape with no image.
        float v = 0.0F; ///< Texture y, normalized. Zero for a shape with no image.
        float r = 0.0F; ///< Red, linear.
        float g = 0.0F; ///< Green, linear.
        float b = 0.0F; ///< Blue, linear.
        float a = 0.0F; ///< Alpha, which carries no curve.
    };

    /**
     * @brief One run of indices that share every piece of pipeline state.
     *
     * A batch ends when something changes that the GPU cannot carry inside a
     * draw. The transform is not one of those, because Renderer applies it to
     * each corner as it records.
     */
    struct Batch {
        std::uint32_t first_index = 0; ///< Where this run starts in the index buffer.
        std::uint32_t index_count = 0; ///< How many indices the run holds.
        /// @brief The blend mode the run was recorded under.
        moth_ui::BlendMode blend = moth_ui::BlendMode::Replace;
        /**
         * @brief The image the run draws, or a null handle for a plain shape.
         *
         * One draw reads one texture, so a run ends where the image changes.
         * A null handle means the run drew rectangles, gradients or an outline,
         * and `UiPass` binds a single white texel for it. That keeps one
         * pipeline for both kinds, because white is the identity of the
         * multiply the fragment stage does.
         */
        gfx::TextureHandle texture;
        /**
         * @brief The texture filter the run was recorded under.
         *
         * @warning Nothing reads this yet. A `gfx::` sampler belongs to the
         *          texture it was uploaded with, so a filter cannot be chosen
         *          at bind time. See issue #209. The recorder carries it and
         *          breaks on it, so the day gfx grows a sampler the batch
         *          already says which one it wanted.
         */
        moth_ui::TextureFilter filter = moth_ui::TextureFilter::Linear;
        bool clipped = false;          ///< Whether @c clip names a scissor rectangle.
        std::int32_t clip_x = 0;       ///< Scissor left edge in pixels.
        std::int32_t clip_y = 0;       ///< Scissor top edge in pixels.
        std::uint32_t clip_width = 0;  ///< Scissor width in pixels.
        std::uint32_t clip_height = 0; ///< Scissor height in pixels.
    };

    /**
     * @brief Records moth_ui drawing calls into vertex and index buffers.
     *
     * moth_ui drives this through its `IRenderer` interface. Nothing here talks
     * to a device. `begin()` clears the recording, moth_ui draws its node tree,
     * and `UiPass` uploads what came out and issues one draw for each batch.
     *
     * The state stack follows moth_ui rather than this engine:
     * - `PushColor` **composes** with the colour under it, so a parent tint
     *   reaches every child.
     * - `PushTransform` **replaces**, because the node tree already composed the
     *   parent chain.
     * - `PushClip` **intersects** with the clip under it.
     * - A pop never empties a stack that carries a default.
     *
     * @warning No file in this namespace may include a Vulkan header. Hard rule
     *          4.1 keeps Vulkan inside `src/gfx/vulkan/`, and CI greps for it.
     */
    class Renderer final : public moth_ui::IRenderer {
    public:
        Renderer();

        Renderer(const Renderer&) = delete;
        Renderer& operator=(const Renderer&) = delete;
        Renderer(Renderer&&) = delete;
        Renderer& operator=(Renderer&&) = delete;
        ~Renderer() final = default;

        /**
         * @brief Drops the last recording and resets every stack to its default.
         *
         * Call this once for each frame, before moth_ui draws.
         *
         * @param width Target width in pixels.
         * @param height Target height in pixels.
         */
        void begin(std::uint32_t width, std::uint32_t height);

        /// @brief Closes the batch still open, so the recording can be read.
        void end();

        /**
         * @brief The recorded vertices. Valid until the next begin().
         *
         * @return The recorded vertices. Valid until the next begin().
         */
        [[nodiscard]] const std::vector<Vertex>& vertices() const { return m_vertices; }

        /**
         * @brief The recorded indices. Valid until the next begin().
         *
         * @return The recorded indices. Valid until the next begin().
         */
        [[nodiscard]] const std::vector<std::uint32_t>& indices() const { return m_indices; }

        /**
         * @brief The recorded batches, in the order they must be drawn.
         *
         * @return The recorded batches, in the order they must be drawn.
         */
        [[nodiscard]] const std::vector<Batch>& batches() const { return m_batches; }

        /**
         * @brief The logical width the recording was made against.
         *
         * @return The logical width the recording was made against.
         */
        [[nodiscard]] std::uint32_t logical_width() const { return m_width; }

        /**
         * @brief The logical height the recording was made against.
         *
         * @return The logical height the recording was made against.
         */
        [[nodiscard]] std::uint32_t logical_height() const { return m_height; }

        /// @cond
        // These implement moth_ui::IRenderer, and moth_ui documents the
        // contract. Doxygen reads src/ only, so it cannot see that base class
        // and reports every override as undocumented. The rules that are not
        // obvious are on the class above, because they are the ones a reader
        // here needs.
        void PushBlendMode(moth_ui::BlendMode mode) override;
        void PopBlendMode() override;
        void PushColor(const moth_ui::Color& color) override;
        void PopColor() override;
        void PushTransform(const moth_ui::FloatMat4x4& transform) override;
        void PopTransform() override;
        void PushClip(const moth_ui::IntRect& rect) override;
        void PopClip() override;
        void PushTextureFilter(moth_ui::TextureFilter filter) override;
        void PopTextureFilter() override;

        void RenderRect(const moth_ui::IntRect& rect) override;
        void RenderFilledRect(const moth_ui::IntRect& rect) override;
        void RenderGradientRect(const moth_ui::IntRect& rect,
                                const moth_ui::LinearGradient& gradient) override;
        void RenderImage(const moth_ui::IImage& image, const moth_ui::IntRect& source_rect,
                         const moth_ui::IntRect& dest_rect, moth_ui::ImageScaleType scale_type,
                         float scale) override;
        void RenderText(std::string_view text, moth_ui::IFont& font,
                        moth_ui::TextHorizAlignment horizontal,
                        moth_ui::TextVertAlignment vertical,
                        const moth_ui::IntRect& dest_rect) override;

        void SetRendererLogicalSize(const moth_ui::IntVec2& size) override;
        /// @endcond

    private:
        /// @brief One rectangle in local space, with the texture it reads.
        struct Quad {
            float x0 = 0.0F; ///< Left edge in local space.
            float y0 = 0.0F; ///< Top edge in local space.
            float x1 = 0.0F; ///< Right edge in local space.
            float y1 = 0.0F; ///< Bottom edge in local space.
            float u0 = 0.0F; ///< Texture x at the left edge, normalized.
            float v0 = 0.0F; ///< Texture y at the top edge, normalized.
            float u1 = 0.0F; ///< Texture x at the right edge, normalized.
            float v1 = 0.0F; ///< Texture y at the bottom edge, normalized.
        };

        /// @brief The colour of each corner, in the order add_quad writes them.
        struct QuadColors {
            moth_ui::Color top_left;     ///< The colour at (x0, y0).
            moth_ui::Color top_right;    ///< The colour at (x1, y0).
            moth_ui::Color bottom_right; ///< The colour at (x1, y1).
            moth_ui::Color bottom_left;  ///< The colour at (x0, y1).
        };

        /// @brief The four white corners a shape with no image of its own uses.
        [[nodiscard]] static QuadColors plain_white();

        /// @brief Ends the open batch and starts one with the current state.
        void break_batch();

        /**
         * @brief Makes the open batch draw @p texture, breaking it when it must.
         *
         * A draw call reads one texture, so a run that already recorded a quad
         * against another one has to end here.
         */
        void want_texture(gfx::TextureHandle texture);

        /**
         * @brief Adds one transformed quad with a colour at each corner.
         *
         * The corners arrive in local space and leave in screen space, because
         * the current transform is applied here.
         */
        void add_quad(const Quad& quad, const QuadColors& colors);

        std::vector<Vertex> m_vertices;
        std::vector<std::uint32_t> m_indices;
        std::vector<Batch> m_batches;

        std::vector<moth_ui::Color> m_color;
        std::vector<moth_ui::BlendMode> m_blend;
        std::vector<moth_ui::FloatMat4x4> m_transform;
        std::vector<moth_ui::IntRect> m_clip;
        std::vector<moth_ui::TextureFilter> m_filter;

        std::uint32_t m_width = 1;
        std::uint32_t m_height = 1;
    };

}
