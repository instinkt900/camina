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

        /// @brief The recorded vertices. Valid until the next begin().
        [[nodiscard]] const std::vector<Vertex>& vertices() const { return m_vertices; }

        /// @brief The recorded indices. Valid until the next begin().
        [[nodiscard]] const std::vector<std::uint32_t>& indices() const { return m_indices; }

        /// @brief The recorded batches, in the order they must be drawn.
        [[nodiscard]] const std::vector<Batch>& batches() const { return m_batches; }

        /// @brief The logical width the recording was made against.
        [[nodiscard]] std::uint32_t logical_width() const { return m_width; }

        /// @brief The logical height the recording was made against.
        [[nodiscard]] std::uint32_t logical_height() const { return m_height; }

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

    private:
        /// @brief Ends the open batch and starts one with the current state.
        void break_batch();

        /**
         * @brief Adds one transformed quad with a colour at each corner.
         *
         * The corners arrive in local space and leave in screen space, because
         * the current transform is applied here.
         */
        void add_quad(float x0, float y0, float x1, float y1,
                      const moth_ui::Color& top_left, const moth_ui::Color& top_right,
                      const moth_ui::Color& bottom_right, const moth_ui::Color& bottom_left);

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
