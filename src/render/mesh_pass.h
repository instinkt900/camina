#pragma once

/**
 * @file
 * @brief Draws every entity that names a mesh.
 *
 * This is the pass that replaces CubePass. CubePass draws one shape that lives
 * in its own source file. This one draws whatever the scene says, which is what
 * makes the asset pipeline visible.
 *
 * A submesh names a material, and the material names its base color texture.
 * Both arrive by GUID, so this pass binds one texture for each submesh and the
 * scene decides what that texture is.
 *
 * The shading over that color is still a placeholder. The cooked material also
 * names a normal map, a metallic-roughness map, and an occlusion map, and M5 is
 * the milestone that reads them.
 */

#include "assets/content.h"
#include "gfx/device.h"
#include "math/conventions.h"
#include "render/material_cache.h"
#include "render/mesh_cache.h"
#include "render/texture_cache.h"
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
        /// @brief Frees the pipeline and everything the caches uploaded.
        ~MeshPass();

        MeshPass() = default;
        MeshPass(const MeshPass&) = delete;
        MeshPass& operator=(const MeshPass&) = delete;
        MeshPass(MeshPass&&) = delete;
        MeshPass& operator=(MeshPass&&) = delete;

        /**
         * @brief Builds the pipeline and the fallback texture.
         * @param device The device to draw with.
         * @param content The engine content tree, which holds the shaders.
         * @return True when the pipeline built.
         */
        [[nodiscard]] bool create(gfx::Device* device, const assets::Content& content);

        /// @brief Frees the pipeline and everything the caches uploaded.
        void destroy();

        /**
         * @brief Draws every entity that has a MeshRenderer and a WorldTransform.
         *
         * A mesh, a material, and a texture each load the first time something
         * asks for it, so the first frame that shows a new mesh pays for the
         * uploads. M4.5 moves that off the frame, when hot reload needs a load
         * that does not stall.
         *
         * @param commands The open command list.
         * @param world The world to read.
         * @param content The game content tree, which holds the meshes, the
         * materials, and the textures.
         * @param view_projection The camera, without any model matrix.
         */
        void draw(gfx::CommandList* commands, const scene::World& world,
                  const assets::Content& content, const Mat4& view_projection);

        /// @brief How many meshes are uploaded.
        /// @return The count.
        [[nodiscard]] std::size_t mesh_count() const { return meshes_.size(); }

        /// @brief How many textures are uploaded.
        /// @return The count, the fallback texel not included.
        [[nodiscard]] std::size_t texture_count() const { return textures_.size(); }

        /// @brief How many draw calls the last draw() made.
        /// @return The count, one for each submesh of each entity.
        [[nodiscard]] std::size_t draw_count() const { return draw_count_; }

    private:
        gfx::Device* device_ = nullptr;
        gfx::PipelineHandle pipeline_;
        MeshCache meshes_;
        TextureCache textures_;
        MaterialCache materials_;
        std::size_t draw_count_ = 0;
    };

} // namespace engine::render
