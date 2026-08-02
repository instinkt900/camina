#pragma once

/**
 * @file
 * @brief A pass that draws one textured cube.
 *
 * This is the layer above `gfx::`. It holds no Vulkan type, and rule 4.1 in
 * DESIGN.md keeps it that way. M5 replaces it with the render graph.
 */

#include "gfx/device.h"
#include "math/conventions.h"

namespace engine::render {

    /**
     * @brief Owns the cube mesh, its texture, and its pipeline.
     *
     * The mesh and the texture are built in code, because the asset pipeline
     * arrives at M4. See DESIGN.md section 10.
     */
    class CubePass {
    public:
        CubePass() = default;
        ~CubePass();

        CubePass(const CubePass&) = delete;
        CubePass& operator=(const CubePass&) = delete;
        CubePass(CubePass&&) = delete;
        CubePass& operator=(CubePass&&) = delete;

        /**
         * @brief Uploads the mesh and the texture, then builds the pipeline.
         * @param device The device that owns the resources.
         * @return True on success. On failure the reason is logged.
         */
        [[nodiscard]] bool create(gfx::Device* device);

        /// @brief Releases everything this pass owns. Safe to call twice.
        void destroy();

        /**
         * @brief Records the cube into an open rendering scope.
         *
         * Call this between gfx::cmd_begin_rendering() and gfx::cmd_end_rendering().
         *
         * @param commands The command list from gfx::begin_frame().
         * @param mvp The combined model, view, and projection matrix.
         */
        void draw(gfx::CommandList* commands, const Mat4& mvp) const;

    private:
        gfx::Device* device_ = nullptr;
        gfx::PipelineHandle pipeline_;
        gfx::BufferHandle vertices_;
        gfx::BufferHandle indices_;
        gfx::TextureHandle texture_;
    };

} // namespace engine::render
