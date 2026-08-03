#pragma once

/**
 * @file
 * @brief Draws every entity that names a mesh.
 *
 * This is the pass that replaces CubePass. CubePass draws one shape that lives
 * in its own source file. This one draws whatever the scene says, which is what
 * makes the asset pipeline visible.
 *
 * The shading is a placeholder. A submesh carries a material GUID and it is
 * null until M4.4b writes one, so every surface here takes the same neutral
 * shade. The geometry, the normals, and the winding are real, and those are
 * what a first look at a new importer has to check.
 */

#include "assets/content.h"
#include "gfx/device.h"
#include "math/conventions.h"
#include "render/mesh_cache.h"
#include "scene/world.h"

namespace engine::render {

    /**
     * @brief Draws the meshes a world names.
     *
     * @code
     * engine::render::MeshPass pass;
     * pass.create(device, engine_content);
     * pass.draw(commands, world, game_content, view_projection);
     * @endcode
     */
    class MeshPass {
    public:
        /// @brief Frees the pipeline and every uploaded mesh.
        ~MeshPass();

        MeshPass() = default;
        MeshPass(const MeshPass&) = delete;
        MeshPass& operator=(const MeshPass&) = delete;
        MeshPass(MeshPass&&) = delete;
        MeshPass& operator=(MeshPass&&) = delete;

        /**
         * @brief Builds the pipeline.
         * @param device The device to draw with.
         * @param content The engine content tree, which holds the shaders.
         * @return True when the pipeline built.
         */
        [[nodiscard]] bool create(gfx::Device* device, const assets::Content& content);

        /// @brief Frees the pipeline and every uploaded mesh.
        void destroy();

        /**
         * @brief Draws every entity that has a MeshRenderer and a WorldTransform.
         *
         * A mesh loads the first time an entity asks for it, so the first frame
         * that shows a new mesh pays for the upload. M4.5 moves that off the
         * frame, when hot reload needs a load that does not stall.
         *
         * @param commands The open command list.
         * @param world The world to read.
         * @param content The game content tree, which holds the meshes.
         * @param view_projection The camera, without any model matrix.
         */
        void draw(gfx::CommandList* commands, const scene::World& world,
                  const assets::Content& content, const Mat4& view_projection);

        /// @brief How many meshes are uploaded.
        /// @return The count.
        [[nodiscard]] std::size_t mesh_count() const { return meshes_.size(); }

        /// @brief How many draw calls the last draw() made.
        /// @return The count, one for each submesh of each entity.
        [[nodiscard]] std::size_t draw_count() const { return draw_count_; }

    private:
        gfx::Device* device_ = nullptr;
        gfx::PipelineHandle pipeline_;
        MeshCache meshes_;
        std::size_t draw_count_ = 0;
    };

} // namespace engine::render
