#pragma once

/**
 * @file
 * @brief A pass that draws one triangle, to prove the pipeline path works.
 *
 * This is the layer above `gfx::`. It holds no Vulkan type, and rule 4.1 in
 * DESIGN.md keeps it that way. M5 replaces this with the render graph.
 */

#include "gfx/device.h"

/// @brief Render passes and, from M5, the render graph.
namespace engine::render {

    /**
     * @brief Owns the triangle pipeline and records the draw.
     *
     * The shaders are compiled at build time and embedded, so this needs no file
     * access. See cmake/Shaders.cmake.
     */
    class TrianglePass {
    public:
        TrianglePass() = default;
        ~TrianglePass();

        TrianglePass(const TrianglePass&) = delete;
        TrianglePass& operator=(const TrianglePass&) = delete;
        TrianglePass(TrianglePass&&) = delete;
        TrianglePass& operator=(TrianglePass&&) = delete;

        /**
         * @brief Builds the pipeline from the embedded shader modules.
         * @param device The device that owns the pipeline.
         * @return True on success. On failure the reason is logged.
         */
        [[nodiscard]] bool create(gfx::Device* device);

        /// @brief Releases the pipeline. Safe to call twice.
        void destroy();

        /**
         * @brief Records the draw into an open rendering scope.
         *
         * Call this between gfx::cmd_begin_rendering() and gfx::cmd_end_rendering().
         *
         * @param commands The command list from gfx::begin_frame().
         */
        void draw(gfx::CommandList* commands) const;

    private:
        gfx::Device* device_ = nullptr;
        gfx::PipelineHandle pipeline_;
    };

} // namespace engine::render
