#pragma once

/**
 * @file
 * @brief Draws the environment cubemap where no geometry covers a pixel.
 */

#include "assets/asset_source.h"
#include "gfx/device.h"
#include "math/conventions.h"

#include <cstdint>

namespace engine::render {

    /**
     * @brief Fills the pixels geometry left empty with the environment.
     *
     * `scene::Environment` names a cubemap and MeshPass samples it to light
     * every surface. Nothing ever drew it, so a pixel no geometry covered kept
     * the clear color the caller gave the scene target. In a closed room that is
     * invisible. In an open scene the flat fill reads as a wall, and a smooth
     * surface reflects a sky the viewer cannot see.
     *
     * This is one full-screen triangle at the far plane. It costs only the
     * pixels nothing else covered, because the depth test rejects the rest.
     *
     * @warning This is not a pass of the render graph, and that is deliberate.
     * It draws inside the rendering scope MeshPass already opened, so it touches
     * the same two attachments in the same two states that MeshPass::declare()
     * names. Declaring it separately would derive a barrier, because
     * derive_barriers() orders every write against what came before it, and a
     * barrier inside a rendering scope is invalid.
     */
    class SkyPass {
    public:
        /**
         * @brief Builds the pipeline that draws the sky.
         *
         * The descriptor set is not built here. It names a cubemap, and which
         * cubemap that is comes from the world rather than from the content
         * tree, so draw() builds it on the first frame that has one.
         *
         * @param device The device to build on. Held, not owned.
         * @param content The engine content tree, which holds the sky shaders.
         * @return False when the pipeline did not build.
         */
        [[nodiscard]] bool create(gfx::Device* device, const assets::AssetSource& content);

        /// @brief Releases everything create() and draw() built. Safe twice.
        void destroy();

        /**
         * @brief Builds a new pipeline from the cooked shaders and swaps it in.
         *
         * The old pipeline stays when the new one will not build, the same way
         * every other pass answers a broken shader.
         *
         * @param content The engine content tree.
         * @return True when the pipeline was replaced.
         */
        [[nodiscard]] bool reload_shaders(const assets::AssetSource& content);

        /**
         * @brief Draws the sky, when the scene named an environment.
         *
         * @param commands The open command list.
         * @param environment The cubemap to show. A null handle draws nothing.
         * @param world_from_clip The inverse of the matrix the frame draws
         * with, which turns a pixel back into a world-space ray.
         * @param camera_position Where that ray starts.
         *
         * @warning Call this inside the gfx::cmd_begin_color_rendering() scope
         * that MeshPass drew in, and after MeshPass::draw(). Before the opaque
         * draws it would still be correct and would shade every pixel of the
         * frame, which is the whole cost this pass exists to avoid.
         */
        void draw(gfx::CommandList* commands, gfx::TextureHandle environment,
                  const Mat4& world_from_clip, const Vec3& camera_position);

        /// @brief How many times draw() issued its triangle.
        /// @return The count since create(). Nothing resets it, and zero means
        /// no frame ever had an environment to draw.
        [[nodiscard]] std::uint64_t draw_count() const { return draw_count_; }

    private:
        /// Builds a pipeline from the cooked shaders into @p out.
        [[nodiscard]] bool build_pipeline(const assets::AssetSource& content,
                                          gfx::PipelineHandle& out);
        /**
         * Builds the set that binds @p environment into @p out.
         *
         * @param pipeline The pipeline whose layout the set is allocated against.
         * @param environment The cubemap the set names.
         * @param out Receives the set. Untouched on failure.
         * @return False when the set could not be built.
         */
        [[nodiscard]] bool build_set(gfx::PipelineHandle pipeline, gfx::TextureHandle environment,
                                     gfx::DescriptorSetHandle& out);

        gfx::Device* device_ = nullptr;
        gfx::PipelineHandle pipeline_;
        gfx::DescriptorSetHandle set_;
        /// @brief Which cubemap set_ names, so a scene that changes is noticed.
        gfx::TextureHandle bound_;
        std::uint64_t draw_count_ = 0;
    };

} // namespace engine::render
