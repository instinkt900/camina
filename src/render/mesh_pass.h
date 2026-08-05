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
#include <string_view>
#include <vector>

namespace engine::render {

    /**
     * @brief How many compiled forms of `mesh.frag` the pass builds.
     *
     * The shader compiles out two things: the normal map and the occlusion map.
     * Every other map is read with no branch, because a slot the material left
     * empty binds a white texel that costs the same to sample. Two toggles give
     * four forms.
     *
     * A cross product of all five maps would be thirty-two, and most of those
     * combinations no material in the sandbox asks for. See
     * `src/assets/meta.h` for why the variant list is written and not computed.
     */
    inline constexpr std::size_t kMeshVariantCount = 4;

    /**
     * @brief Which compiled form of `mesh.frag` a material has to draw with.
     *
     * Bit 0 of the result is the normal map and bit 1 is the occlusion map, so
     * the index matches the order the variants appear in `mesh.frag.meta`. That
     * order is for a person reading the file. The pass matches by what a module
     * declares, not by where it sits.
     *
     * @param has_maps A mask of MaterialMap bits, from material_maps().
     * @return An index below kMeshVariantCount.
     */
    [[nodiscard]] std::size_t mesh_variant_index(std::uint32_t has_maps);

    /**
     * @brief The defines one compiled form must have been built with.
     * @param variant An index below kMeshVariantCount.
     * @return The defines. The storage is static. Empty for the base form.
     */
    [[nodiscard]] std::span<const std::string_view> mesh_variant_defines(std::size_t variant);

    /**
     * @brief Finds the cooked form built with exactly @p defines.
     *
     * A cooked module records what it was compiled with, so this matches on
     * that rather than on the order the manifest lists the outputs in. A
     * variant added to the sidecar in the middle then moves nothing.
     *
     * The match is exact in both directions. A form built with more defines
     * than asked for would shade differently, so it is not a substitute.
     *
     * @param forms Every cooked form of one source.
     * @param defines What the caller needs, from mesh_variant_defines().
     * @return The form, or nullptr when no form matches.
     */
    [[nodiscard]] const assets::Shader* pick_shader_variant(
        std::span<const assets::Shader> forms, std::span<const std::string_view> defines);

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

        /// @brief How many pipeline changes the last draw() made.
        /// @return The count, over the opaque draws and the blended ones.
        [[nodiscard]] std::size_t pipeline_switch_count() const { return pipeline_switches_; }

    private:
        /// One blended submesh, waiting for the sort.
        struct BlendedDraw {
            Mat4 model{ 1.0F };            ///< The model matrix to push.
            gfx::BufferHandle vertices;    ///< The stream the submesh reads.
            gfx::BufferHandle indices;     ///< The indices the submesh reads.
            gfx::DescriptorSetHandle set;  ///< The material set to bind.
            std::uint32_t index_count = 0; ///< How many indices to draw.
            std::uint32_t first_index = 0; ///< Where the submesh starts.
            std::size_t variant = 0;       ///< Which compiled form it needs.
            bool double_sided = false;     ///< Whether the material wants both faces.
            float depth = 0.0F;            ///< Distance to the camera, for the sort.
        };

        /**
         * Every pipeline the pass draws with.
         *
         * One for each compiled form of the fragment shader, and then the same
         * set again for the blended draws. They all declare the same
         * descriptors, so one material set binds with any of them.
         */
        struct PipelineSet {
            std::array<gfx::PipelineHandle, kMeshVariantCount> opaque; ///< Writes depth.
            std::array<gfx::PipelineHandle, kMeshVariantCount> blend;  ///< Blends, no depth write.
        };

        /// Builds every pipeline from the shaders in @p content, into @p out.
        /// Frees whatever it built when any one of them fails.
        [[nodiscard]] bool build_pipelines(const assets::Content& content, PipelineSet& out);

        /// Builds one pipeline from two already-read modules.
        /// @param blend True for the pipeline that blends and does not write depth.
        [[nodiscard]] bool build_pipeline(const assets::Shader& vertex,
                                          const assets::Shader& fragment,
                                          const std::vector<gfx::DescriptorBinding>& bindings,
                                          bool blend, gfx::PipelineHandle& out);

        /// Frees every pipeline in @p set and clears the handles.
        void destroy_pipelines(PipelineSet& set);

        /// The pipeline every descriptor set is allocated against. See the note
        /// on PipelineSet about why any of them would serve.
        [[nodiscard]] gfx::PipelineHandle layout_pipeline() const { return pipelines_.opaque[0]; }

        /// Draws what collect() gathered, back to front.
        void draw_blended(gfx::CommandList* commands);

        /// Builds the per-frame blocks and the sets that bind them.
        [[nodiscard]] bool build_frame_sets();

        /// Frees the per-frame sets, which belong to a pipeline layout.
        void destroy_frame_sets();

        /// Resolves the cubemap the world names, and rebuilds the frame sets
        /// when it is not the one they already bind.
        void update_environment(const scene::World& world, const assets::Content& content);

        gfx::Device* device_ = nullptr;
        /**
         * @brief Every compiled form, opaque and blended.
         *
         * They all declare the same descriptors, so their set layouts are
         * compatible and one material set binds with any of them. build_pipelines()
         * checks that rather than trusting it, because a declaration moved inside
         * an `#ifdef` would break it with no error until a draw read the wrong
         * texture.
         */
        PipelineSet pipelines_;
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
        /**
         * @brief True while the world holds more than one Environment component.
         *
         * The first one wins. draw() warns when this turns on and stays quiet
         * after that, the same way it treats a light overflow.
         */
        bool environments_overflowed_ = false;
        /// @brief The cubemap the frame sets bind. Null until the first draw().
        gfx::TextureHandle environment_;
        /// @brief Which cubemap ::environment_ came from, so a change is visible.
        Guid environment_guid_;
        MeshCache meshes_;
        TextureCache textures_;
        MaterialCache materials_;
        /// @brief The blended submeshes of the current frame. Kept to reuse its storage.
        std::vector<BlendedDraw> blended_;
        std::size_t draw_count_ = 0;
        /// @brief How many times the last draw() changed pipeline. See #105.
        std::size_t pipeline_switches_ = 0;
    };

} // namespace engine::render
