#pragma once

/**
 * @file
 * @brief The passes that turn a world into a picture, in the order they run.
 *
 * A frame draws the shadow map, fills the cluster grid, shades the geometry
 * into a half float image, and maps that image down to what a display can
 * show. Which passes those are, what each one reads and writes, and where the
 * barriers between them go is one answer, and this file holds it.
 *
 * It used to sit inline in the frame loop of `apps/runtime`. The editor draws
 * the same world into a panel, so a second copy of the pass order would have to
 * be kept in step by hand. See DESIGN.md section 8.
 *
 * This owns the shadow pass, the mesh pass, and the tonemap pass. It does not
 * own the frame: the caller opens the frame, decides what the tonemapped
 * picture is drawn into, and draws whatever else goes over it.
 */

#include "assets/content.h"
#include "gfx/device.h"
#include "math/conventions.h"
#include "render/mesh_pass.h"
#include "render/render_graph.h"
#include "render/shadow_pass.h"
#include "render/tonemap_pass.h"
#include "scene/world.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace engine::render {

    /**
     * @brief Which pass a GPU time belongs to.
     *
     * The order is the order SceneRenderer::gpu_pass_ns() reports, and it is
     * the order the passes run in.
     */
    enum class ScenePass : std::uint32_t {
        Shadow = 0, ///< The cascaded depth render.
        Cull,       ///< The compute dispatch that fills the cluster grid.
        Mesh,       ///< The shading of every visible submesh.
        Tonemap,    ///< The full-screen triangle that writes the frame out.
        Count,      ///< How many passes are timed. Not a pass.
    };

    /// @brief How many passes ScenePass names, as a size.
    inline constexpr std::size_t kScenePassCount = static_cast<std::size_t>(ScenePass::Count);

    /**
     * @brief How one frame looks at the world.
     *
     * Everything that changes from frame to frame and is not the world itself.
     * The camera arrives already composed, because who owns the camera differs
     * between the two applications and the renderer does not need to know.
     */
    struct SceneView {
        /// @brief The camera, as clip space from world space.
        Mat4 clip_from_world{ 1.0F };
        /// @brief Where the camera stands, in world space. The specular term needs it.
        Vec3 camera_position{ 0.0F, 0.0F, 0.0F };
        /// @brief What the scene image clears to, in linear color.
        gfx::ColorRGBA clear_color{ 0.0F, 0.0F, 0.0F, 1.0F };
        /**
         * @brief The size the scene renders at.
         *
         * The cluster grid has to agree with the fragment shader about which
         * cell a pixel is in, so this is the size of the image the mesh pass
         * draws into rather than the size of the window.
         */
        gfx::Extent2D extent{};
    };

    /**
     * @brief Draws a world with the shadow, cull, mesh, and tonemap passes.
     *
     * The order of one frame is:
     *
     * @code
     * renderer.begin_frame(info.commands);
     * renderer.draw_scene(info.commands, world, game_content, view);
     * gfx::cmd_begin_rendering(info.commands, black, false);
     * renderer.draw_tonemap(info.commands, exposure);
     * // whatever else goes over the picture
     * gfx::cmd_end_rendering(info.commands);
     * @endcode
     *
     * draw_scene() opens and closes a rendering scope of its own over the scene
     * image. The tonemap is a separate call because the caller decides what it
     * writes into, and because the physics wireframe, the game UI, and the ImGui
     * overlay all draw in that same scope, after the curve.
     *
     * @warning The scene image is the size the swapchain settled on, so resize()
     * has to run whenever the swapchain is rebuilt.
     */
    class SceneRenderer {
    public:
        /**
         * @brief Builds every pass and the image the scene renders into.
         *
         * The shadow pass builds first. The mesh pass binds the shadow map into
         * every frame descriptor set, so the map has to exist before those sets
         * are built.
         *
         * @param device The device to build on. Held, not owned.
         * @param content The engine content tree, which holds the cooked shaders
         * and the split sum lookup table.
         * @param extent The size the scene renders at, which is the size the
         * swapchain settled on rather than the size that was asked for.
         * @return False when a pass did not build, which leaves nothing able to
         * draw. The caller calls destroy() either way.
         */
        [[nodiscard]] bool create(gfx::Device* device, const assets::Content& content,
                                  gfx::Extent2D extent);

        /// @brief Releases everything create() built. Safe to call twice.
        void destroy();

        /**
         * @brief Rebuilds the scene image at a new size.
         *
         * The carried state of that image resets with it, because a new image
         * has no history for a barrier to order against.
         *
         * @param extent The new size.
         * @return False when the image did not rebuild, which leaves nothing
         * able to draw.
         */
        [[nodiscard]] bool resize(gfx::Extent2D extent);

        /**
         * @brief Builds every pipeline again from the cooked shaders.
         *
         * Each pass keeps its old pipelines when the new ones will not build,
         * because somebody editing a shader breaks it often.
         *
         * @param content The engine content tree.
         * @return True when every pass took the new shaders.
         */
        [[nodiscard]] bool reload_shaders(const assets::Content& content);

        /**
         * @brief Resets the timestamp pool and reads what the last frame wrote.
         *
         * Call this once for each frame, after gfx::begin_frame() and before
         * draw_scene(). The results reach the caller through gpu_pass_ns().
         *
         * @param commands The command list from gfx::begin_frame().
         */
        void begin_frame(gfx::CommandList* commands);

        /**
         * @brief Draws the shadow map, the cluster grid, and the geometry.
         *
         * It issues the barriers the graph derived for all four passes,
         * including the tonemap pass, so the caller can open its own rendering
         * scope and call draw_tonemap() straight after.
         *
         * @param commands The open command list, outside any rendering scope.
         * @param world The entities to draw.
         * @param content The game content tree, which holds the meshes, the
         * materials, and the textures.
         * @param view The camera and the size to render at.
         * @return False when the pass declarations were refused, which means no
         * barrier is safe and the frame must be abandoned.
         *
         * @warning Call this outside a rendering scope. The cull is a compute
         * dispatch and a dispatch cannot happen inside one.
         */
        [[nodiscard]] bool draw_scene(gfx::CommandList* commands, const scene::World& world,
                                      const assets::Content& content, const SceneView& view);

        /**
         * @brief Maps the scene image down and writes it out.
         *
         * @param commands The open command list, inside the caller's rendering
         * scope over whatever the picture is written into.
         * @param exposure A linear scale on the scene before the ACES curve.
         *
         * @warning draw_scene() must have run for this frame. It is what draws
         * the image this reads and what issues the barrier that makes it
         * readable.
         */
        void draw_tonemap(gfx::CommandList* commands, float exposure);

        /**
         * @brief What one pass cost on the GPU, for the frame before this one.
         *
         * @param pass Which pass to report. ScenePass::Count reads as zero.
         * @return Nanoseconds, or zero before the first pair of frames has run.
         */
        [[nodiscard]] double gpu_pass_ns(ScenePass pass) const;

        /// @brief The mesh pass, for the counts it reports and the assets it caches.
        /// @return The pass. Owned here and borrowed by the caller.
        [[nodiscard]] MeshPass& mesh() { return mesh_; }
        /// @brief The mesh pass, read only.
        /// @return The pass.
        [[nodiscard]] const MeshPass& mesh() const { return mesh_; }
        /// @brief The shadow pass, for the counts it reports.
        /// @return The pass. Owned here and borrowed by the caller.
        [[nodiscard]] ShadowPass& shadow() { return shadow_; }
        /// @brief The shadow pass, read only.
        /// @return The pass.
        [[nodiscard]] const ShadowPass& shadow() const { return shadow_; }
        /// @brief The tonemap pass, which owns the image the scene renders into.
        /// @return The pass. Owned here and borrowed by the caller.
        [[nodiscard]] TonemapPass& tonemap() { return tonemap_; }

    private:
        /**
         * Works out the barriers this frame needs, from what each pass declared.
         *
         * @param out Receives the schedule.
         * @return False when the declarations were refused.
         */
        [[nodiscard]] bool derive_frame_barriers(GraphSchedule& out);

        gfx::Device* device_ = nullptr;
        /// Renders the directional light's depth, which the mesh pass samples.
        ShadowPass shadow_;
        /// Shades every visible submesh, and culls the lights into the grid.
        MeshPass mesh_;
        /// Owns the half float image the scene renders into, and writes it out.
        TonemapPass tonemap_;

        /**
         * What state each graph resource is in.
         *
         * Carried across frames, because the shadow map, the scene image, and
         * the cluster grid are each one resource that every frame in flight
         * shares. The state the last frame left one in is what this frame's
         * barrier has to order against.
         */
        std::array<gfx::ResourceState, kFrameResourceCount> states_{};

        /// GPU time for the last frame, one entry for each ScenePass.
        std::array<double, kScenePassCount> gpu_pass_ns_{};
        /// Nanoseconds for one timestamp tick, asked of the device once.
        float timestamp_period_ = 0.0F;
        /// True once a frame has reset the pool, so a read is valid.
        bool timestamps_ready_ = false;
    };

} // namespace engine::render
