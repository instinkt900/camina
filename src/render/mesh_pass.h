#pragma once

/**
 * @file
 * @brief Draws every entity that names a mesh.
 *
 * This is the pass that replaces CubePass. CubePass draws one shape that lives
 * in its own source file. This one draws whatever the scene says, which is what
 * makes the asset pipeline visible.
 *
 * A submesh names a material, and the material names five textures. All of them
 * arrive by GUID, so this pass binds one descriptor set for each submesh and the
 * scene decides what is in it.
 *
 * The shading is Cook-Torrance metallic-roughness since M5.2, and it reads every
 * map and every factor the cooked material carries. Two things over it are still
 * constants in the shader: the one directional light, which becomes a scene
 * component, and the environment, which M5.4 replaces with a cooked one.
 */

#include "assets/content.h"
#include "core/guid.h"
#include "gfx/device.h"
#include "math/conventions.h"
#include "render/material_cache.h"
#include "render/mesh_cache.h"
#include "render/texture_cache.h"
#include "scene/world.h"

#include <array>
#include <cstdint>
#include <span>

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
         * @brief Lets go of the assets that changed, so the next draw reloads them.
         *
         * This is the render half of hot reload. `assets::Content::reload()`
         * says what moved and this frees it. An identity the caches never
         * loaded costs nothing, so the caller passes the whole list.
         *
         * @param changed The identities that changed, from the content.
         *
         * @warning Call this between frames and never while a command list is
         * open. It waits for the GPU, which cannot happen mid-frame.
         */
        void reload(std::span<const Guid> changed);

        /**
         * @brief Builds the pipeline again from the shaders on disk.
         *
         * The other half of hot reload. A mesh and a texture swap in behind a
         * handle, and a shader cannot: the SPIR-V is built into the pipeline,
         * so a changed shader means a new pipeline.
         *
         * A shader that will not build leaves the pipeline that is drawing
         * alone. Somebody editing a shader breaks it often, and losing the
         * picture on every typo would make the loop useless.
         *
         * @param content The engine content tree, which holds the shaders.
         * @return True when a new pipeline was built and swapped in.
         *
         * @warning Call this between frames and never while a command list is
         * open. It waits for the GPU, which cannot happen mid-frame.
         */
        [[nodiscard]] bool reload_shaders(const assets::Content& content);

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
         * @param camera_position Where the camera is, in world space. The
         * shading needs it for the view vector that every specular term uses.
         */
        void draw(gfx::CommandList* commands, const scene::World& world,
                  const assets::Content& content, const Mat4& view_projection,
                  const Vec3& camera_position);

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
        /// Builds a pipeline from the shaders in @p content, into @p out.
        [[nodiscard]] bool build_pipeline(const assets::Content& content,
                                          gfx::PipelineHandle& out);

        /// Builds the per-frame blocks and the sets that bind them.
        [[nodiscard]] bool build_frame_sets();

        /// Frees the per-frame sets, which belong to a pipeline layout.
        void destroy_frame_sets();

        gfx::Device* device_ = nullptr;
        gfx::PipelineHandle pipeline_;
        /**
         * @brief One per-frame block for each frame in flight.
         *
         * A single block would be written at the top of a frame while the GPU
         * may still be reading it for the frame before. One for each slot in the
         * ring means a write never touches what a live frame reads.
         */
        std::array<gfx::BufferHandle, gfx::kFramesInFlight> frame_uniforms_;
        /// @brief The set that binds each block above.
        std::array<gfx::DescriptorSetHandle, gfx::kFramesInFlight> frame_sets_;
        /// @brief Which slot of the ring the next draw uses.
        std::uint32_t frame_slot_ = 0;
        /**
         * @brief True while the world holds more lights than one frame carries.
         *
         * draw() warns when this turns on and stays quiet after that. Deleting a
         * light until the world fits again clears it, so a later overflow warns
         * once more.
         */
        bool lights_overflowed_ = false;
        MeshCache meshes_;
        TextureCache textures_;
        MaterialCache materials_;
        std::size_t draw_count_ = 0;
    };

} // namespace engine::render
