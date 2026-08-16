#include "render/scene_renderer.h"

#include "core/assert.h"
#include "core/log.h"

#include <cstdint>

namespace engine::render {

    namespace {

        /// Where each pass sits in the schedule derive_barriers() returns. The
        /// order is the order the declarations are listed in, and it is the
        /// order the passes record in.
        constexpr std::size_t kShadowPassIndex = 0;
        constexpr std::size_t kCullPassIndex = 1;
        constexpr std::size_t kMeshPassIndex = 2;
        constexpr std::size_t kTonemapPassIndex = 3;

        /**
         * Where one pass writes its pair of timestamps.
         *
         * A pass writes the first slot before it records and the second after,
         * so the difference is what it cost. One constant gives both the pair
         * and the slot in gpu_pass_ns_, so a pass cannot put a value in one
         * place and read it from another.
         *
         * @param pass Which pass to ask about.
         * @return The index of the first slot of the pair.
         */
        [[nodiscard]] constexpr std::uint32_t timestamp_slot(ScenePass pass) {
            return static_cast<std::uint32_t>(pass) * 2U;
        }

        /// How many slots the pairs take together.
        constexpr std::uint32_t kTimestampCount = static_cast<std::uint32_t>(kScenePassCount) * 2U;

        /// Which image each graph resource is, for the ones a frame does not own.
        using GraphTextures = std::array<gfx::TextureHandle, kFrameResourceCount>;

        /**
         * Puts the barriers of one pass into the command list.
         *
         * @param commands The open command list, outside a rendering scope.
         * @param schedule What derive_barriers() worked out.
         * @param pass Which pass of the schedule to issue. One past the end
         * does nothing.
         * @param textures Which image each resource is. A null handle means the
         * resource is a frame target, which an enum names instead.
         */
        void issue_pass_barriers(gfx::CommandList* commands, const GraphSchedule& schedule,
                                 std::size_t pass, const GraphTextures& textures) {
            if (pass >= schedule.passes.size()) {
                return;
            }
            for (const GraphBarrier& barrier : schedule.passes[pass].before) {
                if (barrier.resource == kClusterGrid) {
                    // A buffer, so there is no layout to change and nothing to
                    // name. gfx::cmd_buffer_barrier is a global memory barrier,
                    // and the two states carry the whole dependency.
                    gfx::cmd_buffer_barrier(commands, barrier.before, barrier.after);
                    continue;
                }
                const gfx::TextureHandle texture = textures[barrier.resource.index];
                if (texture.valid()) {
                    // Not a frame target, so it is named by a handle rather
                    // than by an enum. See gfx::cmd_texture_barrier.
                    gfx::cmd_texture_barrier(commands, texture, barrier.before, barrier.after);
                    continue;
                }
                const gfx::FrameTarget target = barrier.resource == kFrameDepth
                                                    ? gfx::FrameTarget::Depth
                                                    : gfx::FrameTarget::Color;
                gfx::cmd_frame_barrier(commands, target, barrier.before, barrier.after);
            }
        }

    } // namespace

    bool SceneRenderer::create(gfx::Device* device, const assets::Content& content,
                               gfx::Extent2D extent) {
        ENGINE_CHECK(device != nullptr, "SceneRenderer::create needs a device.");
        device_ = device;

        // The shadow pass first. The mesh pass binds its map into every frame
        // descriptor set, so the map has to exist before those sets are built.
        if (!shadow_.create(device, content)) {
            return false;
        }
        if (!mesh_.create(device, content, shadow_.map())) {
            return false;
        }
        if (!tonemap_.create(device, content, extent)) {
            return false;
        }
        return true;
    }

    void SceneRenderer::destroy() {
        mesh_.destroy();
        shadow_.destroy();
        tonemap_.destroy();
        device_ = nullptr;
    }

    bool SceneRenderer::resize(gfx::Extent2D extent) {
        if (!tonemap_.resize(extent)) {
            ENGINE_LOG_CRITICAL("The scene color target did not rebuild, so nothing can draw.");
            return false;
        }
        // A new image carries no history, so there is nothing for the first
        // barrier of the next frame to order against.
        states_[kSceneColor.index] = gfx::ResourceState::Undefined;
        return true;
    }

    bool SceneRenderer::reload_shaders(const assets::Content& content) {
        // Every pass, and each reports on its own. One that will not build
        // keeps the pipelines it had, so a broken shader costs the picture of
        // that pass rather than the program.
        bool ok = mesh_.reload_shaders(content);
        ok = shadow_.reload_shaders(content) && ok;
        ok = tonemap_.reload_shaders(content) && ok;
        return ok;
    }

    void SceneRenderer::begin_frame(gfx::CommandList* commands) {
        gfx::cmd_reset_timestamps(commands);

        // The first frame has written no pair yet, so there is nothing to read
        // and the pool it would read from is the one just reset.
        if (!timestamps_ready_) {
            timestamps_ready_ = true;
            return;
        }

        if (timestamp_period_ == 0.0F) {
            timestamp_period_ = gfx::timestamp_period(device_);
        }
        if (timestamp_period_ <= 0.0F) {
            return;
        }

        std::array<std::uint64_t, kTimestampCount> ticks{};
        if (!gfx::read_timestamps(device_, 0, kTimestampCount, ticks.data())) {
            return;
        }

        const double period = static_cast<double>(timestamp_period_);
        for (std::size_t i = 0; i < kScenePassCount; ++i) {
            const std::uint32_t first = timestamp_slot(static_cast<ScenePass>(i));
            gpu_pass_ns_[i] = static_cast<double>(ticks[first + 1] - ticks[first]) * period;
        }
    }

    bool SceneRenderer::derive_frame_barriers(GraphSchedule& out) {
        const std::array passes{ ShadowPass::declare(), MeshPass::declare_cull(),
                                 MeshPass::declare(), TonemapPass::declare() };

        // The two frame targets start over every frame. The swapchain image is
        // a different image on almost every acquire, and the depth image is
        // scratch that nothing reads across a frame boundary.
        states_[kFrameColor.index] = gfx::ResourceState::Undefined;
        states_[kFrameDepth.index] = gfx::ResourceState::Undefined;
        // The shadow map, the scene color, and the cluster grid do not. Each is
        // shared across the frames in flight, so the state the last frame left
        // it in is what the barrier has to order against. Calling one Undefined
        // here would derive a barrier that waits on the stage the new state
        // uses, and what it has to wait for is the previous frame's fragment
        // shader reading it. That is a write after read, and it is the same
        // hazard #125 found on the shared depth image.

        if (!derive_barriers(passes, states_, out)) {
            ENGINE_LOG_CRITICAL("The frame declarations were refused, so no barrier is safe.");
            return false;
        }

        // Carry each state into the next frame. The graph works this out
        // already, which is what final_states is for.
        for (std::size_t i = 0; i < states_.size(); ++i) {
            states_[i] = out.final_states[i];
        }
        return true;
    }

    bool SceneRenderer::draw_scene(gfx::CommandList* commands, const scene::World& world,
                                   const assets::Content& content, const SceneView& view) {
        // The render graph, before anything opens a rendering scope.
        // gfx::begin_frame() leaves the frame images in Undefined and this is
        // what moves them, because the graph is what knows which pass needs
        // them and in what state.
        GraphSchedule schedule;
        if (!derive_frame_barriers(schedule)) {
            return false;
        }

        // Which image each resource is. The two frame targets stay null,
        // because an enum names those and the handle table is how the rest are
        // found.
        GraphTextures textures{};
        textures[kShadowMap.index] = shadow_.map();
        textures[kSceneColor.index] = tonemap_.target();

        // The shadow pass first, because the mesh pass reads what it wrote. Its
        // barriers go in before it records, and its own rendering scope opens
        // and closes inside draw().
        issue_pass_barriers(commands, schedule, kShadowPassIndex, textures);
        gfx::cmd_write_timestamp(commands, timestamp_slot(ScenePass::Shadow));
        shadow_.draw(commands, world, content, mesh_.meshes(), view.clip_from_world);
        gfx::cmd_write_timestamp(commands, timestamp_slot(ScenePass::Shadow) + 1);
        mesh_.set_shadow_view(shadow_.light_view_projections(), shadow_.cascade_splits(),
                              shadow_.cascade_biases(), shadow_.has_light());

        // The cluster cull, which is a pass of its own. It runs here rather
        // than inside the mesh pass because a compute dispatch cannot happen
        // inside a rendering scope. Its barrier moves the cluster grid from the
        // read the last frame left it in to a compute write.
        issue_pass_barriers(commands, schedule, kCullPassIndex, textures);
        gfx::cmd_write_timestamp(commands, timestamp_slot(ScenePass::Cull));
        const ClusterView cluster_view{
            .z_near = kDefaultNearPlane,
            .viewport_width = static_cast<float>(view.extent.width),
            .viewport_height = static_cast<float>(view.extent.height),
        };
        mesh_.cull(commands, world, content, view.clip_from_world, view.camera_position,
                   cluster_view);
        gfx::cmd_write_timestamp(commands, timestamp_slot(ScenePass::Cull) + 1);

        // Then the mesh pass barriers, which include moving the shadow map from
        // a depth target to something a shader can read, and the cluster grid
        // from the compute write to a fragment read.
        issue_pass_barriers(commands, schedule, kMeshPassIndex, textures);

        // Into the half float scene image, not the swapchain. An 8-bit sRGB
        // image would clip every value above 1 as the fragment shader wrote it.
        //
        // A draw or an end outside a rendering scope is invalid, so nothing is
        // recorded when the scope did not open. The frame then reaches the
        // tonemap pass with a scene image nobody drew into, which is a black
        // picture rather than undefined behavior.
        if (gfx::cmd_begin_color_rendering(commands, tonemap_.target(), view.clear_color)) {
            // Every entity that names a mesh draws it, and that is every entity
            // that draws at all. This is the pipeline made visible: the
            // geometry comes from a cooked file that a glTF produced, and
            // nothing here knows which file that was.
            gfx::cmd_write_timestamp(commands, timestamp_slot(ScenePass::Mesh));
            mesh_.draw(commands, world, content, view.camera_position);
            gfx::cmd_write_timestamp(commands, timestamp_slot(ScenePass::Mesh) + 1);

            gfx::cmd_end_rendering(commands);
        }

        // The barriers of the tonemap pass, which include moving the scene
        // image to a shader read. They go in here rather than in draw_tonemap,
        // because a barrier inside a rendering scope is invalid and the caller
        // has that scope open by then.
        issue_pass_barriers(commands, schedule, kTonemapPassIndex, textures);
        return true;
    }

    void SceneRenderer::draw_tonemap(gfx::CommandList* commands, float exposure) {
        gfx::cmd_write_timestamp(commands, timestamp_slot(ScenePass::Tonemap));
        tonemap_.draw(commands, exposure);
        gfx::cmd_write_timestamp(commands, timestamp_slot(ScenePass::Tonemap) + 1);
    }

    double SceneRenderer::gpu_pass_ns(ScenePass pass) const {
        const auto index = static_cast<std::size_t>(pass);
        return index < kScenePassCount ? gpu_pass_ns_[index] : 0.0;
    }

} // namespace engine::render
