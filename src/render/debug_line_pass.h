#pragma once

/**
 * @file
 * @brief Draws world-space lines, which today means the physics wireframe.
 */

#include "assets/asset_source.h"
#include "gfx/device.h"
#include "math/conventions.h"
#include "physics/debug_draw.h"

#include <array>
#include <cstddef>
#include <span>

namespace engine::render {

    /**
     * @brief Draws a list of world-space lines over the tonemapped frame.
     *
     * @note **This draws after the curve and tests no depth.** Both are
     * deliberate. Drawing before the curve would let ACES move the colors, and
     * a wireframe whose color says what state a body is in has to keep it.
     * Testing depth would hide a collider that sits inside geometry, which is
     * the case somebody is usually hunting when they turn this on.
     *
     * It costs nothing when nothing is drawn. A frame with no lines uploads
     * nothing and records no draw, so the toggle is the only thing a frame that
     * does not want it pays for.
     *
     * @code
     * pass.draw(commands, clip_from_world, lines);
     * @endcode
     */
    class DebugLinePass {
    public:
        /**
         * @brief Builds the line pipeline.
         *
         * @param device The device to build on. Held, not owned.
         * @param content The engine content tree, which holds the debug_line
         *                shaders.
         * @return False when the pipeline did not build.
         */
        [[nodiscard]] bool create(gfx::Device* device, const assets::AssetSource& content);

        /// @brief Releases everything create() built. Safe to call twice.
        void destroy();

        /**
         * @brief Uploads the lines and draws them.
         *
         * Call this inside the scope that the tonemap pass drew in, because the
         * pipeline is built for a target with no depth attachment and Vulkan
         * calls a mismatch undefined rather than an error.
         *
         * @param commands The command list from begin_frame().
         * @param clip_from_world The camera matrix, which is the only thing the
         *                        vertex stage reads.
         * @param lines The lines to draw. An empty span draws nothing.
         */
        void draw(gfx::CommandList* commands, const Mat4& clip_from_world,
                  std::span<const physics::DebugLine> lines);

        /// @brief How many lines the last draw() recorded.
        /// @return The count, which is zero when the pass drew nothing.
        [[nodiscard]] std::size_t line_count() const { return line_count_; }

    private:
        /// One end of a line, as the vertex stage reads it.
        struct Vertex {
            Vec3 position{ 0.0F, 0.0F, 0.0F };
            Vec3 color{ 1.0F, 1.0F, 1.0F };
        };

        gfx::Device* device_ = nullptr;
        gfx::PipelineHandle pipeline_{};

        /// One buffer for each slot, so a buffer a frame in flight still reads
        /// is never destroyed. The same three UiPass keeps, for the same reason:
        /// two frames are in flight, and a third leaves a margin rather than
        /// relying on that number staying two.
        static constexpr std::size_t kSlots = 3;
        std::array<gfx::BufferHandle, kSlots> vertices_{};
        std::size_t slot_ = 0;
        std::size_t line_count_ = 0;
    };

} // namespace engine::render
