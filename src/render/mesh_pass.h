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
 * map and every factor the cooked material carries. The environment lights it by
 * the split sum approximation since M5.4b, over three cooked parts: the
 * irradiance coefficients, the prefiltered cubemap, and the shared lookup table.
 */

#include "assets/asset_source.h"
#include "assets/irradiance.h"
#include "core/guid.h"
#include "gfx/device.h"
#include "math/conventions.h"
#include "math/frustum.h"
#include "render/material_cache.h"
#include "render/mesh_cache.h"
#include "render/render_graph.h"
// For kCascadeCount. The mesh pass reads what the shadow pass wrote, so the
// number of cascades is shared between them rather than repeated.
#include "render/shadow_pass.h"
#include "render/texture_cache.h"
#include "scene/world.h"

#include <algorithm>
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

    // The compute cull writes a cluster grid the mesh pass reads, so the
    // counts below are shared between the two halves rather than repeated.
    //
    /// @brief How many tiles across the frustum the cluster grid divides into.
    inline constexpr std::uint32_t kClusterTileCountX = 16;
    /// @brief How many tiles down the frustum the cluster grid divides into.
    inline constexpr std::uint32_t kClusterTileCountY = 12;
    /// @brief How many depth slices the cluster grid uses.
    inline constexpr std::uint32_t kClusterSliceCount = 16;
    /// @brief How many cells the cluster grid holds.
    inline constexpr std::uint32_t kClusterCellCount =
        kClusterTileCountX * kClusterTileCountY * kClusterSliceCount;
    /**
     * @brief How many light indices one cell holds before the grid grows.
     *
     * The number is measured rather than chosen. A room of 837 visible point
     * lights of 1.8 m range renders identically to a shader that loops over
     * every light at this value. At 64 the same scene lost light on 36 percent
     * of the frame, by up to 228 of 255 on a channel. Raising it from 64 cost
     * 2 ms of a 100 ms saving.
     *
     * It is the floor rather than the ceiling. The grid grows to hold every
     * visible light in every cell, so a cell overflows only past
     * kMaxLightsPerCell. See cluster_cell_capacity_for().
     */
    inline constexpr std::uint32_t kMinLightsPerCell = 256;

    /**
     * @brief The ceiling on how many light indices one cell holds.
     *
     * The grid is one count and this many indices for each of
     * kClusterCellCount cells, and there is one grid for each frame in
     * flight. So this is a memory budget written as a light count. At 2048 the
     * grid is 25 MiB, which is 50 MiB over two frames in flight.
     *
     * A scene with more visible lights than this can still overflow a cell, and
     * MeshPass::cluster_may_drop() reports when that becomes possible. Removing
     * the ceiling needs a compacted index list, which trades a second pass over
     * the lights for memory that fits the scene. See issue #175.
     */
    inline constexpr std::uint32_t kMaxLightsPerCell = 2048;

    /**
     * @brief How many light indices each cell needs to hold @p light_count.
     *
     * The capacity doubles from kMinLightsPerCell until one cell can hold
     * every visible light, and it stops at kMaxLightsPerCell. Doubling is
     * what keeps a scene that grows one light at a time from reallocating the
     * grid once for each, and it is the rule the light buffer already follows.
     *
     * A cell that can hold every visible light cannot drop one, whatever the
     * camera does. That is the whole guarantee, and it replaces counting a drop
     * that has already happened.
     *
     * @param light_count How many lights the frame can see.
     * @param ceiling The most a cell may hold. Lower than kMinLightsPerCell is
     * allowed and wins, which is how a run forces the overflow to measure it.
     * @return The per-cell capacity, at most @p ceiling.
     */
    [[nodiscard]] std::uint32_t cluster_cell_capacity_for(
        std::size_t light_count, std::uint32_t ceiling = kMaxLightsPerCell);

    /**
     * @brief What the per-cell capacity becomes on a frame with @p light_count.
     *
     * cluster_cell_capacity_for() says what a frame needs. This says what to
     * use, which is never below what the grid already holds. A scene whose light
     * count crosses a power of two every frame would otherwise wait for the
     * device and reallocate the grid on each one.
     *
     * @p ceiling still wins over @p current, so lowering it takes effect rather
     * than being held off by a grid that is already larger.
     *
     * @param current How many indices a cell holds now.
     * @param light_count How many lights the frame can see.
     * @param ceiling The most a cell may hold.
     * @return The capacity to use. Equal to @p current when nothing has to move.
     */
    [[nodiscard]] std::uint32_t grow_cluster_cell_capacity(
        std::uint32_t current, std::size_t light_count,
        std::uint32_t ceiling = kMaxLightsPerCell);

    /// @brief How many uint32 values one cell occupies at @p cell_capacity.
    /// @param cell_capacity How many light indices the cell holds.
    /// @return The stride, which is the count word plus the indices.
    [[nodiscard]] inline std::uint32_t cluster_cell_stride(std::uint32_t cell_capacity) {
        return 1 + cell_capacity;
    }

    /**
     * @brief How far in front of the camera the cluster slices reach, in meters.
     *
     * The slices grow exponentially from the near plane out to here, which puts
     * most of them where a perspective camera puts most of its pixels. Past
     * this distance every fragment falls in the last slice, and the cull
     * stretches that slice to meet them, so a far light is never dropped.
     *
     * A larger value spends slices on distance that a scene may not use. This
     * one covers the sandbox several times over.
     */
    inline constexpr float kClusterFar = 100.0F;

    /// @brief What the cluster grid needs to know about the camera.
    struct ClusterView {
        /// @brief The near plane the projection was built with, in meters.
        float z_near = kDefaultNearPlane;
        /// @brief How wide the target is, in pixels.
        float viewport_width = 0.0F;
        /// @brief How tall the target is, in pixels.
        float viewport_height = 0.0F;
    };

    /**
     * @brief Draws the meshes a world names.
     *
     * cull() runs first and outside any rendering scope, because it dispatches
     * a compute shader and because it uploads the frame block the two draw
     * halves bind.
     *
     * @code
     * engine::render::MeshPass pass;
     * pass.create(device, engine_content, shadow.map());
     *
     * const engine::render::ClusterView view{ .z_near = engine::kDefaultNearPlane,
     *                                         .viewport_width = 1280.0F,
     *                                         .viewport_height = 720.0F };
     * pass.cull(commands, world, game_content, view_projection, camera_position, view);
     * // Open the rendering scope here.
     * pass.draw_opaque(commands, world, game_content, camera_position);
     * // The sky goes here, between the two halves. See issue #435.
     * pass.draw_blended(commands);
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
         * @param shadow_map The map the shadow pass renders. Required.
         * @return True when the pipeline built.
         *
         * @warning @p shadow_map must be a real depth target from
         * ShadowPass::map(). The shader reads it as a comparison sampler and
         * there is no fallback that would serve, so this fails without one.
         */
        [[nodiscard]] bool create(gfx::Device* device, const assets::AssetSource& content,
                                  gfx::TextureHandle shadow_map);

        /**
         * @brief Tells the pass where the light looked from this frame.
         *
         * Call it after ShadowPass::draw() and before draw(), because the matrix
         * is fitted to what the scene holds and can move on any frame.
         *
         * @param light_view_projections One for each cascade, taking a world
         * position into that cascade's clip space.
         * @param splits Where each cascade ends, in front of the camera. The
         * shader picks a cascade by comparing a fragment's view depth against
         * these, which is what keeps the split out of the shader as a constant.
         * @param biases The depth bias each cascade needs in its own clip space.
         * @param casts False when the world has no directional light, which
         * makes the shader skip the shadow lookup rather than read a stale map.
         */
        void set_shadow_view(const std::array<Mat4, kCascadeCount>& light_view_projections,
                             const std::array<float, kCascadeCount>& splits,
                             const std::array<float, kCascadeCount>& biases, bool casts);

        /// @brief Frees the pipeline and everything the caches uploaded.
        void destroy();

        /**
         * @brief Lets go of the assets that changed, so the next draw reloads them.
         *
         * This is the render half of hot reload. `assets::Content::reload()`
         * says what moved and this frees it, so the list comes from whoever
         * owns the cooked tree rather than through the asset seam. An identity
         * the caches never loaded costs nothing, so the caller passes the whole
         * list.
         *
         * @param changed The identities that changed, from the content owner.
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
         * This rebuilds only the pipelines and the descriptor sets that depend
         * on their layout. The split sum lookup table is left alone, because
         * its file is in the same tree and a person editing a shader did not
         * retune the ray budget. Call reload_brdf_lut() for that.
         *
         * @param content The engine content tree, which holds the shaders.
         * @return True when a new pipeline was built and swapped in.
         *
         * @warning Call this between frames and never while a command list is
         * open. It waits for the GPU, which cannot happen mid-frame.
         */
        [[nodiscard]] bool reload_shaders(const assets::AssetSource& content);

        /**
         * @brief Drops the split sum lookup table and reads it again.
         *
         * The BRDF table lives in the engine content tree beside the shaders.
         * Saving ibl.brdf.meta to retune the ray budget calls this so the new
         * table appears without a restart. It does not touch the pipelines.
         *
         * @param content The engine content tree, which holds ibl.brdf.
         * @return True when the table was read and the frame sets were rebuilt.
         *
         * @warning Call this between frames and never while a command list is
         * open. It waits for the GPU, which cannot happen mid-frame.
         */
        [[nodiscard]] bool reload_brdf_lut(const assets::AssetSource& content);

        /**
         * @brief What this pass reads and writes, for the render graph.
         *
         * It writes the color target and the depth target, and it reads the
         * shadow map that the shadow pass wrote. That read is the producer and
         * consumer pair the graph turns into a barrier.
         *
         * The textures a material names are uploaded once and never written by
         * a pass, so they are not frame resources and the graph does not order
         * them.
         *
         * The spans point at storage with static lifetime, so the result can
         * be held for as long as the caller likes.
         *
         * @return The declaration.
         */
        [[nodiscard]] static PassDesc declare();

        /**
         * @brief What the cull pass reads and writes.
         *
         * The cull writes the cluster grid and the mesh pass reads it, which is
         * the third producer and consumer pair in the frame and the first one
         * over a buffer. It touches no image, so it declares no other resource.
         *
         * It is a declaration of its own rather than part of declare(), because
         * a compute dispatch cannot happen inside a rendering scope and the two
         * therefore run as two passes. See render_graph.h for kClusterGrid.
         *
         * @return The declaration.
         */
        [[nodiscard]] static PassDesc declare_cull();

        /**
         * @brief Gathers the lights and fills the cluster grid for this frame.
         *
         * It advances the frame slot, uploads the frame block and the light
         * list, and dispatches the compute shader that writes a light list for
         * every cell.
         *
         * @param commands The open command list.
         * @param world The world, for gathering lights.
         * @param content The game content tree.
         * @param view_projection The camera.
         * @param camera_position Where the camera stands, in world space.
         * @param view The near plane and the viewport, which the grid needs to
         * agree with the fragment shader about which cell a pixel is in.
         *
         * @warning Call this once for each frame, before draw(), and outside a
         * rendering scope. A compute dispatch cannot happen inside one. draw()
         * reports and draws nothing when this did not run, because the frame
         * block it binds would be the one the frame before it wrote.
         */
        void cull(gfx::CommandList* commands, const scene::World& world,
                  const assets::AssetSource& content, const Mat4& view_projection,
                  const Vec3& camera_position, const ClusterView& view);

        /**
         * @brief Draws the opaque half of every entity that has a MeshRenderer.
         *
         * This is the first half of the pass. It gathers every visible submesh,
         * issues the opaque draws, and holds the blended ones for
         * draw_blended(). Both halves have to run, in that order.
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
         * @param camera_position Where the camera is, in world space. The
         * shading needs it for the view vector that every specular term uses.
         *
         * An entity whose bounds miss the camera frustum issues no draw. The
         * test is per entity rather than per submesh, so a mesh that is in view
         * draws all of its parts. culled_mesh_count() reports what it dropped.
         *
         * @warning cull() must have run for this frame. The camera reaches the
         * shader through the frame block that cull() uploads, so this takes no
         * view matrix of its own. The frustum this culls against comes from
         * there too.
         */
        void draw_opaque(gfx::CommandList* commands, const scene::World& world,
                         const assets::AssetSource& content, const Vec3& camera_position);

        /**
         * @brief Draws every blended submesh draw_opaque() collected, back to front.
         *
         * This is the second half of the pass, and it is a call of its own so
         * that the caller can draw the sky between the two. A blended surface
         * reads what is already in the colour attachment, so over open sky it
         * has to run after the sky rather than before it. SkyPass is opaque and
         * tests depth for equality, and blended geometry writes no depth, so a
         * sky drawn afterwards does not tint the pane. It paints over it. That
         * was issue #435.
         *
         * @param commands The open command list.
         *
         * @warning Call this after draw_opaque() in the same frame and the same
         * rendering scope. draw_opaque() is what gathers the list, so calling
         * this alone draws nothing and reports once.
         */
        void draw_blended(gfx::CommandList* commands);

        /**
         * @brief The mesh cache, so another pass can draw the same geometry.
         *
         * The shadow pass renders depth from the meshes this one uploaded.
         * Giving it a cache of its own would put a second copy of every vertex
         * on the device to render the same triangles.
         *
         * @return The cache. It is owned here and borrowed by the caller.
         */
        [[nodiscard]] MeshCache& meshes() { return meshes_; }

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

        /// @brief How many lights the last draw() sent to the shader.
        /// @return The count, which is every directional light plus the point
        /// lights whose range reaches the view.
        [[nodiscard]] std::size_t visible_light_count() const { return visible_lights_.size(); }

        /**
         * @brief How many point lights the last draw() culled.
         *
         * This is what says the frustum test is doing anything. A scene that
         * keeps it at zero while lights sit behind the camera has a broken
         * test. The picture looks right either way, because culling a light
         * that lights nothing visible changes no pixel.
         *
         * @return The count of point lights whose range sphere missed the view.
         */
        [[nodiscard]] std::size_t culled_light_count() const { return culled_lights_; }

        /**
         * @brief How many entities the last draw() culled against the frustum.
         *
         * The same argument culled_light_count() makes. A mesh cull that works
         * changes no pixel, so the picture cannot say whether it ran. This can.
         *
         * @return The count of entities whose bounds sphere missed the view. An
         * entity counts once however many submeshes it holds.
         */
        [[nodiscard]] std::size_t culled_mesh_count() const { return culled_meshes_; }

        /// @brief How many lights the storage buffer holds without growing.
        /// @return The capacity, which grows to fit and never shrinks.
        [[nodiscard]] std::size_t light_capacity() const { return light_capacity_; }

        /// @brief How many light indices one cluster cell holds this frame.
        /// @return The capacity, which follows the visible light count up to
        /// ::kMaxLightsPerCell.
        [[nodiscard]] std::uint32_t cluster_cell_capacity() const { return cluster_capacity_; }

        /**
         * @brief The environment cubemap the world named, for a pass that draws it.
         *
         * This pass resolves the cubemap once, when the scene names another
         * one. SkyPass shows the same image, and reading it from here rather
         * than resolving a second copy is what stops the two disagreeing about
         * which environment the frame is lit by.
         *
         * @return The cubemap, or a null handle when the scene named no
         * environment at all. A named cubemap that would not load comes back as
         * the grey fallback, because the scene did ask for a sky.
         */
        [[nodiscard]] gfx::TextureHandle environment() const {
            return environment_guid_.valid() ? environment_ : gfx::TextureHandle{};
        }

        /**
         * @brief Lowers the ceiling on how many lights one cell holds.
         *
         * There to force the overflow. A cell that holds every visible light
         * cannot drop one, so a scene that fits leaves the drop path untested.
         * This makes a small scene reach it, which is how the loss it causes is
         * measured rather than argued about.
         *
         * The next cull() rebuilds the grid when the capacity this implies is
         * not the one in use, so it can be called at any time.
         *
         * @param ceiling The most a cell may hold. Zero restores
         * ::kMaxLightsPerCell, and so does anything above it. The budget wins
         * over the argument, because a grid larger than ::kMaxLightsPerCell
         * describes is memory nothing promised.
         */
        void set_cluster_cell_ceiling(std::uint32_t ceiling) {
            cluster_ceiling_ =
                ceiling == 0 ? kMaxLightsPerCell : std::min(ceiling, kMaxLightsPerCell);
        }

        /**
         * @brief Whether a crowded cell can drop a light this frame.
         *
         * True when the frame carries more visible lights than one cell holds,
         * which happens only past ::kMaxLightsPerCell. Below that the capacity
         * fits every light and no cell can drop one, so this is false and the
         * guarantee needs no measurement.
         *
         * It says a drop is possible rather than that one happened. Counting the
         * drops needs the shader to report them back, and the condition is
         * absent in every scene the capacity fits.
         *
         * @return True when the per-cell capacity is below the visible count.
         */
        [[nodiscard]] bool cluster_may_drop() const {
            return visible_lights_.size() > cluster_capacity_;
        }

    private:
        /**
         * @brief One light, as the shader reads it. Two vec4 and nothing else.
         *
         * It must match the Light struct in `mesh.frag` exactly. It lives here
         * rather than in the source file because ::visible_lights_ holds them,
         * and a member needs a complete type.
         */
        struct GpuLight {
            /// @brief xyz is the direction it points for a directional light, or
            /// where it is for a point light. w is 0 for directional, 1 for point.
            std::array<float, 4> position{};
            /// @brief rgb is the color times the intensity. a is the range in
            /// meters, which a directional light leaves at zero.
            std::array<float, 4> color{};
        };

        /**
         * @brief Collects every light the camera can see into ::visible_lights_.
         *
         * A directional light is always kept. It has no position and it lights
         * everything, so there is nothing to test it against.
         *
         * A point light is kept only when its range sphere touches the frustum.
         * The sphere rather than the centre is what matters. A lamp whose
         * centre is off screen still lights what is on screen, and culling it
         * by the centre would put a dark band along the edge of the view.
         *
         * @param world The world to read.
         * @param frustum The camera frustum, in world space.
         * @return How many point lights the frustum test dropped.
         */
        [[nodiscard]] std::size_t gather_lights(const scene::World& world, const Frustum& frustum);

        /**
         * @brief Makes sure the light buffer and the cluster grid fit @p needed.
         *
         * Both grow to fit rather than dropping what does not, so a scene is
         * never refused for carrying too many lights. Growing waits for the
         * device and rebuilds every set, because each frame in flight may be
         * reading its own buffer. The capacity doubles for that reason, so the
         * wait is rare.
         *
         * The two grow together because they answer the same number and share
         * one wait. The light buffer holds @p needed lights, and the grid holds
         * cluster_cell_capacity_for(@p needed) indices in each cell.
         *
         * @param needed How many lights this frame wants to write.
         * @return False when the buffers could not be rebuilt, which leaves the
         * pass unable to draw.
         */
        [[nodiscard]] bool ensure_capacity(std::size_t needed);

        /**
         * @brief Orders ::visible_lights_ so a crowded cell keeps the best ones.
         *
         * A cell writes the lights it overlaps in buffer order and stops when it
         * is full, so buffer order is what decides the survivors. This puts the
         * brightest and furthest-reaching first, by luminance times range.
         *
         * It runs only when cluster_may_drop() is true, which needs more visible
         * lights than ::kMaxLightsPerCell. Below that the order changes nothing,
         * and reordering would move the sum of the lights by a rounding bit for
         * no gain.
         *
         * @warning This is one order for every cell, not a choice for each. A
         * dim lamp close to a cell can still lose to a bright one far from it,
         * because the key names no cell. Ranking for each cell needs the shader
         * to carry a priority, and nothing reaches this path yet.
         */
        void order_lights_for_overflow();
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

        /// One opaque submesh, waiting to be sorted by pipeline variant.
        struct OpaqueDraw {
            Mat4 model{ 1.0F };            ///< The model matrix to push.
            gfx::BufferHandle vertices;    ///< The stream the submesh reads.
            gfx::BufferHandle indices;     ///< The indices the submesh reads.
            gfx::DescriptorSetHandle set;  ///< The material set to bind.
            std::uint32_t index_count = 0; ///< How many indices to draw.
            std::uint32_t first_index = 0; ///< Where the submesh starts.
            std::size_t variant = 0;       ///< Which compiled form it needs.
            bool double_sided = false;     ///< Whether the material wants both faces.
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
        [[nodiscard]] bool build_pipelines(const assets::AssetSource& content, PipelineSet& out);

        /// Builds one pipeline from two already-read modules.
        /// @param blend True for the pipeline that blends and does not write depth.
        [[nodiscard]] bool build_pipeline(const assets::Shader& vertex,
                                          const assets::Shader& fragment,
                                          const std::vector<gfx::DescriptorBinding>& bindings,
                                          bool blend, gfx::PipelineHandle& out);

        /**
         * Frees every pipeline in @p set and clears the handles.
         *
         * @param set The pipelines to release.
         * @param behind_the_frames True to retire them rather than free them
         * now. A reload needs that, because a submitted frame may still have
         * one bound. A half-built set has never been submitted, so it does not.
         */
        void destroy_pipelines(PipelineSet& set, bool behind_the_frames = false);

        /// The pipeline every descriptor set is allocated against. See the note
        /// on PipelineSet about why any of them would serve.
        [[nodiscard]] gfx::PipelineHandle layout_pipeline() const { return pipelines_.opaque[0]; }

        /**
         * Sorts every visible submesh into ::opaque_ and ::blended_.
         *
         * Separate from draw_opaque() because it records no command. It reads
         * the world, culls each entity against ::frustum_, and builds the two
         * lists. The two draw halves then issue them.
         *
         * @param world The world to read.
         * @param content The game content tree, for the mesh and material loads.
         * @param camera_position Where the camera is, for the blended depth sort.
         */
        void gather_draws(const scene::World& world, const assets::AssetSource& content,
                          const Vec3& camera_position);

        /// Builds the per-frame blocks and the sets that bind them.
        [[nodiscard]] bool build_frame_sets();

        /// Frees the per-frame sets, which belong to a pipeline layout.
        void destroy_frame_sets();

        /// Resolves the cubemap and the irradiance the world names, and rebuilds
        /// the frame sets when the cubemap is not the one they already bind.
        void update_environment(const scene::World& world, const assets::AssetSource& content);

        /// Reads the irradiance sub-asset of @p environment into the member.
        /// Falls back to the constant that matches whatever cubemap is bound.
        void update_irradiance(const assets::AssetSource& content, Guid environment, bool fallback);

        /// Finds the split sum lookup table in the engine content tree and
        /// uploads it. It is one table for every environment and every scene.
        [[nodiscard]] bool resolve_brdf_lut(const assets::AssetSource& content);

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
        /**
         * @brief One light list for each frame in flight, as a storage buffer.
         *
         * The lights used to sit in the block above, which capped them at the
         * length the shader declared. A storage buffer has no declared length,
         * so the count is a number the frame block carries. See issue #98.
         */
        std::array<gfx::BufferHandle, gfx::kFramesInFlight> light_buffers_;
        /// @brief How many lights each buffer above holds. Grows, never shrinks.
        std::size_t light_capacity_ = 0;
        /**
         * @brief The lights this frame kept, rebuilt by every draw().
         *
         * A member rather than a local so the storage survives between frames
         * and the vector stops allocating after the first few.
         */
        std::vector<GpuLight> visible_lights_;
        /// @brief How many point lights the frustum test dropped last frame.
        std::size_t culled_lights_ = 0;
        /// @brief How many entities the frustum test dropped last frame.
        std::size_t culled_meshes_ = 0;
        /**
         * @brief The camera planes of this frame, in world space.
         *
         * cull() extracts them for the lights and draw() reuses them for the
         * meshes. One extraction rather than two, and the pair cannot disagree
         * about which camera the frame belongs to.
         */
        Frustum frustum_;
        /// @brief The set that binds each block above.
        std::array<gfx::DescriptorSetHandle, gfx::kFramesInFlight> frame_sets_;
        /// @brief Which slot of the ring the next draw uses.
        std::uint32_t frame_slot_ = 0;
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
        /**
         * @brief The diffuse half of the environment, as the frame block sends it.
         *
         * A sub-asset of the same source the cubemap came from, so a scene names
         * one environment and this pass finds both halves of it.
         */
        assets::IrradianceSH irradiance_;
        /// @brief The split sum lookup table, which every material shares.
        gfx::TextureHandle brdf_lut_;
        /// @brief The shadow map, owned by the shadow pass and read here.
        gfx::TextureHandle shadow_map_;
        /// @brief Where the light looked from this frame, for each cascade.
        std::array<Mat4, kCascadeCount> shadow_views_{};
        /// @brief Where each cascade ends, in front of the camera.
        std::array<float, kCascadeCount> shadow_splits_{};
        /// @brief The depth bias each cascade needs in its own clip space.
        std::array<float, kCascadeCount> shadow_biases_{};
        /// @brief Whether anything casts, which the shader reads as light_count.y.
        bool shadow_casts_ = false;
        /// @brief Which asset ::brdf_lut_ came from, so hot reload can drop it.
        Guid brdf_guid_;
        MeshCache meshes_;
        TextureCache textures_;
        MaterialCache materials_;
        /// @brief The blended submeshes of the current frame. Kept to reuse its storage.
        std::vector<OpaqueDraw> opaque_;
        std::vector<BlendedDraw> blended_;
        std::size_t draw_count_ = 0;
        /// @brief How many times the last draw() changed pipeline. See #105.
        std::size_t pipeline_switches_ = 0;

        /**
         * @brief Whether cull() has run for the frame draw() is about to record.
         *
         * cull() sets it and draw_opaque() clears it, so the pair has to
         * alternate.
         * Without it a caller that forgot cull() would draw with the frame slot
         * and the frame block of the frame before, and the only sign would be a
         * picture one frame stale.
         */
        bool culled_this_frame_ = false;

        /// @brief Whether the warning above has been logged. It says the same
        /// thing every frame once the order is wrong, and once is enough.
        bool warned_missing_cull_ = false;

        /**
         * @brief Whether draw_opaque() ran for this frame.
         *
         * draw_blended() draws the list draw_opaque() gathered, so calling it
         * alone draws nothing. The two halves are separate for the sky (#435),
         * and a caller that keeps only the first half loses every blended
         * surface with no other sign.
         */
        bool gathered_this_frame_ = false;

        /// @brief Whether the warning above has been logged. Once is enough.
        bool warned_missing_gather_ = false;

        /// @brief Reads and uploads the cluster cull compute shader, and builds its pipeline.
        [[nodiscard]] bool build_compute_pipeline(const assets::AssetSource& content);

        /// @brief Builds the per-frame compute descriptor sets. Call after build_frame_sets().
        [[nodiscard]] bool build_compute_sets();

        /// @brief Makes sure the cluster grid buffer holds enough uint32 values.
        [[nodiscard]] bool ensure_cluster_grid();

        /**
         * @brief The compute pipeline that fills the cluster grid.
         *
         * It is one pipeline, because a compute shader has no variant form. It
         * stays alive across shader reloads until a clean replacement is built.
         */
        gfx::PipelineHandle compute_pipeline_;
        /// @brief The descriptor set for the compute pass. Rebuilt when the
        /// light buffer or the compute pipeline changes.
        std::array<gfx::DescriptorSetHandle, gfx::kFramesInFlight> compute_sets_;
        /// @brief One cluster grid for each frame in flight.
        std::array<gfx::BufferHandle, gfx::kFramesInFlight> cluster_grids_;
        /// @brief The uniform buffer that holds the cluster cull parameters.
        std::array<gfx::BufferHandle, gfx::kFramesInFlight> cluster_uniforms_;
        /**
         * @brief How many light indices each cell of the grids above holds.
         *
         * Both shaders read it from the uniform block they already bind, so the
         * grid can grow without a recompile. It never shrinks, for the reason
         * the light capacity does not: shrinking would wait for the device to
         * give memory back that the next frame may ask for again.
         */
        std::uint32_t cluster_capacity_ = kMinLightsPerCell;
        /// @brief The most a cell may hold. See set_cluster_cell_ceiling().
        std::uint32_t cluster_ceiling_ = kMaxLightsPerCell;
    };

} // namespace engine::render
