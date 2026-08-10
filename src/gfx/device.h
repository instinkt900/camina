#pragma once

/**
 * @file
 * @brief Device lifetime, the frame loop, and command recording.
 *
 * Every function here takes an opaque pointer. The backend owns the definition,
 * so no caller can reach a Vulkan type. See rule 4.1 in DESIGN.md.
 */

#include "gfx/types.h"

namespace engine::gfx {

    /**
     * @brief A live connection to one GPU, with its swapchain and frame state.
     *
     * The backend defines this type. Create one with create_device() and release
     * it with destroy_device().
     */
    struct Device;

    /**
     * @brief A recording context for one frame.
     *
     * begin_frame() hands one out. It stays valid until the matching end_frame().
     * Do not keep it past that point.
     */
    struct CommandList;

    /// @brief What begin_frame() reports about the frame it opened.
    struct FrameInfo {
        /// @brief Where to record commands for this frame.
        CommandList* commands = nullptr;
        /// @brief The size of the image this frame draws into.
        Extent2D extent;
        /// @brief Which slot in the frames-in-flight ring this frame uses.
        std::uint32_t frame_index = 0;
    };

    /**
     * @brief Starts the loader, picks a GPU, and builds the swapchain.
     *
     * @param desc The window, the application name, and the debug options.
     * @param out_device Receives the new device on success, and nullptr on failure.
     * @return Result::Success, or the reason the device did not start.
     *
     * @warning The device holds a surface built from `desc.window`. Destroy the
     * device before you destroy the window.
     *
     * @code
     * engine::gfx::Device* device = nullptr;
     * const auto result = engine::gfx::create_device(
     *     { .window = window.native(), .enable_validation = true }, &device);
     * @endcode
     */
    [[nodiscard]] Result create_device(const DeviceDesc& desc, Device** out_device);

    /**
     * @brief Waits for the GPU to go idle, then releases everything the device owns.
     * @param device The device to release. A null pointer is allowed and does nothing.
     */
    void destroy_device(Device* device);

    /**
     * @brief The name the driver reports for the chosen GPU.
     * @param device The device to query.
     * @return A static string owned by the device. It stays valid until destroy_device().
     */
    [[nodiscard]] const char* device_name(const Device* device);

    /**
     * @brief Blocks until the GPU finishes all submitted work.
     * @param device The device to wait on.
     */
    void device_wait_idle(Device* device);

    /**
     * @brief Rebuilds the swapchain at a new size.
     *
     * Call this when the window changes size, and when begin_frame() reports
     * Result::OutOfDate. A width or a height of zero means the window is
     * minimized, so the call does nothing and reports success.
     *
     * @param device The device to rebuild.
     * @param size The new client size in pixels.
     * @return Result::Success, or the reason the swapchain did not rebuild.
     */
    [[nodiscard]] Result device_resize(Device* device, Extent2D size);

    /**
     * @brief Waits for a free frame slot and acquires the next swapchain image.
     *
     * @param device The device to record against.
     * @param out_frame Receives the command list and the image size on success.
     * @return Result::Success when the frame opened. Result::OutOfDate when the
     * swapchain no longer matches the window, in which case no frame opened and
     * the caller must call device_resize() and try again.
     *
     * @warning Every successful call must be paired with end_frame().
     *
     * @warning The color target and the depth target are both left in
     * ResourceState::Undefined. The render graph is what moves them, because it
     * is what knows which pass needs them first. So a caller issues the
     * barriers it derived before it opens a rendering scope.
     */
    [[nodiscard]] Result begin_frame(Device* device, FrameInfo* out_frame);

    /**
     * @brief Closes the command list, submits it, and presents the image.
     * @param device The device that opened the frame.
     * @return Result::Success, or Result::OutOfDate when the swapchain needs a rebuild.
     */
    [[nodiscard]] Result end_frame(Device* device);

    /**
     * @brief Copies the frame that was presented last into host memory.
     *
     * This is how a person or a test looks at what the renderer drew. A run
     * that ends with no error says the commands were valid. It says nothing
     * about a mesh that came out mirrored, inside out, or upside down, and
     * those are exactly the mistakes a new importer makes.
     *
     * The pixels arrive as 8 bits for each channel in RGBA order, whatever the
     * swapchain format is. The alpha is whatever the frame wrote.
     *
     * This waits for the device to go idle, so it costs a stall. Call it once
     * at the end of a run and not on every frame.
     *
     * @warning Call this after end_frame() and not between begin_frame() and
     * end_frame(). There is no frame to read while one is still being recorded.
     *
     * @param device The device that drew the frame.
     * @param pixels Where to write. It must hold width times height times 4 bytes.
     * @param size How many bytes @p pixels holds.
     * @param out_extent The size that was written. Ask for it with a null
     * @p pixels to size a buffer before the second call.
     * @return Result::Success when the pixels were written. Result::ErrorInit
     * when @p size is too small, or when the swapchain cannot be read from.
     */
    [[nodiscard]] Result capture_frame(Device* device, void* pixels, std::size_t size,
                                       Extent2D* out_extent);

    /**
     * @brief Moves one of the frame's images from one state to another.
     *
     * This is what the render graph issues. `render::derive_barriers` works out
     * which of these a frame needs, and this puts one into the command list.
     *
     * The two states decide the stage mask, the access mask, and the image
     * layout on each side. Nothing here is a catch-all, so a barrier costs only
     * the stages the two states name.
     *
     * A barrier whose two states match is not a mistake and is not skipped. Two
     * writes to one image have to be ordered against each other even when the
     * layout does not change, and such a barrier is what carries that.
     *
     * @param commands The command list from begin_frame().
     * @param target Which image to move.
     * @param before The state the image is in now.
     * @param after The state the next pass needs it in.
     *
     * @warning Call this outside a cmd_begin_rendering() scope. A barrier
     * inside a rendering scope is invalid unless it is a self-dependency, and
     * nothing here declares one.
     */
    void cmd_frame_barrier(CommandList* commands, FrameTarget target, ResourceState before,
                           ResourceState after);

    /**
     * @brief Moves any texture from one state to another.
     *
     * The same thing cmd_frame_barrier() does, for an image the frame does not
     * own. A shadow map is the first: the shadow pass writes it as a depth
     * target and the mesh pass reads it as a shader resource, and that
     * transition is the barrier between them.
     *
     * The two entry points stay separate because a frame target is named by an
     * enum and reaches a different image on each acquire, while this one names a
     * handle that stays put. Folding them together would need a union or a
     * handle for the swapchain image, and neither pays for itself with two
     * callers.
     *
     * @param commands The command list from begin_frame().
     * @param texture The image to move. A null or stale handle logs and does nothing.
     * @param before The state the image is in now.
     * @param after The state the next pass needs it in.
     *
     * @warning Call this outside a rendering scope, for the reason
     * cmd_frame_barrier() gives.
     */
    void cmd_texture_barrier(CommandList* commands, TextureHandle texture, ResourceState before,
                             ResourceState after);

    /**
     * @brief Opens dynamic rendering into the current swapchain image.
     *
     * The image loads with a clear to @p clear_color and stores at the end. There
     * is no render pass object and no framebuffer, per DESIGN.md section 2.
     *
     * @warning Both images must already be in the state this needs, which is
     * ResourceState::ColorTarget and ResourceState::DepthTarget. begin_frame()
     * leaves them in ResourceState::Undefined, so the caller issues the
     * barriers the render graph derived before it calls this.
     *
     * @param commands The command list from begin_frame().
     * @param clear_color The linear color to clear to.
     * @param attach_depth Whether to attach the frame depth image. A pass that
     * neither reads nor writes depth passes false to skip the clear.
     */
    void cmd_begin_rendering(CommandList* commands, const ColorRGBA& clear_color,
                             bool attach_depth = true);

    /**
     * @brief Opens dynamic rendering into a color target and the frame depth.
     *
     * The same scope cmd_begin_rendering() opens, over an image the caller owns
     * rather than the swapchain image. A scene draws into one of these and the
     * tonemap pass then writes the swapchain, which is what keeps a value above
     * 1 alive until something maps it down.
     *
     * The render area, the viewport, and the scissor are the size of @p color.
     *
     * @param commands The command list from begin_frame().
     * @param color The image to render into, from create_color_target().
     * @param clear_color The linear color to clear to.
     * @return True when the scope is open. False means the handle was null or
     * stale and no scope was opened.
     *
     * @warning Record nothing when this returns false, and do not call
     * cmd_end_rendering(). A draw or an end outside a rendering scope is
     * invalid, so a caller that ignores the result turns a bad handle into
     * undefined behavior.
     *
     * @warning @p color must already be in ResourceState::ColorTarget and the
     * frame depth image in ResourceState::DepthTarget. The caller issues the
     * barriers the render graph derived.
     *
     * @warning The pipeline that draws here must declare the same
     * ColorTargetFormat the target holds. Close the scope with
     * cmd_end_rendering().
     */
    [[nodiscard]] bool cmd_begin_color_rendering(CommandList* commands, TextureHandle color,
                                                 const ColorRGBA& clear_color);

    /**
     * @brief Opens dynamic rendering into a depth image and no color image.
     *
     * This is what a shadow pass records into. The image clears to zero, which
     * is the far plane under reverse-Z, and stores at the end so a later pass
     * can sample it.
     *
     * The viewport and the scissor are set to the whole image. A shadow map is
     * rarely the size of the window, and a scope that left the frame's viewport
     * in place would render the scene into one corner of the map.
     *
     * @param commands The command list from begin_frame().
     * @param depth The image to render into.
     * @param layer Which array layer to render into. Pass 0 for an image with
     * one layer.
     * @return True when the scope is open. False means the handle was null or
     * stale, or the layer was past the end, and no scope was opened.
     *
     * @warning Record nothing when this returns false, and do not call
     * cmd_end_rendering(). A draw or an end outside a rendering scope is
     * invalid, so a caller that ignores the result turns a bad handle into
     * undefined behavior.
     *
     * @warning The image must already be in ResourceState::DepthTarget. Close
     * the scope with cmd_end_rendering().
     *
     * @warning The clear covers only the layer named here. A cascade set needs
     * one scope for each layer, because a layer nobody rendered into keeps
     * whatever the allocation held.
     */
    [[nodiscard]] bool cmd_begin_depth_rendering(CommandList* commands, TextureHandle depth,
                                                 std::uint32_t layer);

    /**
     * @brief Closes the rendering scope that either begin function opened.
     * @param commands The command list from begin_frame().
     */
    void cmd_end_rendering(CommandList* commands);

    /**
     * @brief Builds a compute pipeline from one SPIR-V module.
     *
     * A compute pipeline needs only the compute stage. It writes into storage
     * buffers and images that the bindings declare, and it has no vertex
     * input, no rasterizer, and no color target.
     *
     * @param device The device that owns the pipeline.
     * @param desc The compute stage and its bindings.
     * @param out_pipeline Receives the handle on success, and a null handle on failure.
     * @return Result::Success, or the reason the pipeline did not build.
     */
    [[nodiscard]] Result create_compute_pipeline(Device* device,
                                                 const ComputePipelineDesc& desc,
                                                 PipelineHandle* out_pipeline);

    /**
     * @brief Builds a graphics pipeline from two SPIR-V modules.
     *
     * The pipeline draws into the swapchain format with dynamic rendering, and it
     * keeps the viewport and the scissor dynamic. A resize therefore needs no
     * rebuild. There is no vertex input state, so the vertex shader must build its
     * positions from the vertex index.
     *
     * @param device The device that owns the pipeline.
     * @param desc The two shader stages.
     * @param out_pipeline Receives the handle on success, and a null handle on failure.
     * @return Result::Success, or the reason the pipeline did not build.
     */
    [[nodiscard]] Result create_graphics_pipeline(Device* device,
                                                  const GraphicsPipelineDesc& desc,
                                                  PipelineHandle* out_pipeline);

    /**
     * @brief Releases a pipeline and frees its slot for reuse.
     *
     * The caller must make sure the GPU has finished with the pipeline. A null or
     * stale handle does nothing.
     *
     * @param device The device that owns the pipeline.
     * @param pipeline The handle to release.
     */
    void destroy_pipeline(Device* device, PipelineHandle pipeline);

    /**
     * @brief Binds a pipeline for the draws that follow.
     * @param commands The command list from begin_frame().
     * @param pipeline The pipeline to bind. A stale handle logs and does nothing.
     */
    void cmd_bind_pipeline(CommandList* commands, PipelineHandle pipeline);

    /**
     * @brief Binds a compute pipeline for the dispatches that follow.
     * @param commands The command list from begin_frame().
     * @param pipeline The compute pipeline to bind. A stale handle logs and does nothing.
     */
    void cmd_bind_compute_pipeline(CommandList* commands, PipelineHandle pipeline);

    /**
     * @brief Binds a descriptor set for the dispatches that follow.
     *
     * @param commands The command list from begin_frame().
     * @param pipeline The bound compute pipeline, which supplies the layout.
     * @param set_index Which set this fills, matching create_descriptor_set().
     * @param set The set to bind. A stale handle logs and does nothing.
     */
    void cmd_bind_compute_descriptor_set(CommandList* commands, PipelineHandle pipeline,
                                         std::uint32_t set_index, DescriptorSetHandle set);

    /**
     * @brief Dispatches a compute shader.
     *
     * The pipeline bound by cmd_bind_compute_pipeline() runs over the group
     * counts given. A group size the shader declares decides how many threads
     * each group runs.
     *
     * @param commands The command list from begin_frame().
     * @param group_count_x Groups in the X dimension.
     * @param group_count_y Groups in the Y dimension.
     * @param group_count_z Groups in the Z dimension.
     */
    void cmd_dispatch(CommandList* commands, std::uint32_t group_count_x,
                      std::uint32_t group_count_y, std::uint32_t group_count_z);

    /**
     * @brief Orders memory access between two states.
     *
     * A global memory barrier between two passes. It carries only the stage and
     * the access masks, because a buffer has no image layout. It names no
     * buffer, so it orders every access the two states cover.
     *
     * A compute pass that writes a cluster grid and a mesh pass that reads it
     * is the pair this exists for.
     *
     * @param commands The command list from begin_frame().
     * @param before The state the buffer is in now.
     * @param after The state the next pass needs it in.
     *
     * @warning Call this outside a rendering scope, for the reason
     * cmd_frame_barrier() gives.
     */
    void cmd_buffer_barrier(CommandList* commands, ResourceState before, ResourceState after);

    /**
     * @brief Sets whether back faces are culled for the draws that follow.
     *
     * The pipeline must declare dynamic cull mode, which the mesh pipeline does.
     * A material whose glTF says doubleSided asks for no culling here.
     *
     * @param commands The command list from begin_frame().
     * @param cull_back Pass true to cull back faces. Pass false to draw both
     * faces, for thin geometry that the author meant to be seen from both sides.
     */
    void cmd_set_cull_mode(CommandList* commands, bool cull_back);

    /**
     * @brief Restricts drawing to a rectangle of the render target.
     *
     * Every graphics pipeline declares a dynamic scissor, and each
     * cmd_begin_*_rendering() call resets it to the whole target. So a pass
     * that wants a smaller area sets one, and the next pass starts clean.
     *
     * A UI clip rectangle is what needed this. Such a rectangle can reach past
     * the left or the top edge, and Vulkan refuses a negative offset, so this
     * clamps the rectangle to the target rather than failing.
     *
     * @param commands The command list from begin_frame().
     * @param x Left edge in pixels. A negative value clamps to zero, and the
     * width shrinks by as much as the clamp moved the edge.
     * @param y Top edge in pixels, clamped the same way.
     * @param width Width in pixels. Zero draws nothing.
     * @param height Height in pixels. Zero draws nothing.
     *
     * @warning The scissor is dynamic state and it carries from one draw to the
     *          next inside a pass. Set it back when the clip ends. Issue #188
     *          is what happens when dynamic state leaks, and it cost a whole
     *          black frame that no validation layer reported.
     */
    void cmd_set_scissor(CommandList* commands, std::int32_t x, std::int32_t y,
                         std::uint32_t width, std::uint32_t height);

    /**
     * @brief Draws without an index buffer.
     * @param commands The command list from begin_frame().
     * @param vertex_count How many vertices the vertex shader runs for.
     * @param instance_count How many instances to draw. Pass 1 for a single copy.
     * @param first_vertex The value gl_VertexIndex starts at.
     * @param first_instance The value gl_InstanceIndex starts at.
     */
    void cmd_draw(CommandList* commands, std::uint32_t vertex_count,
                  std::uint32_t instance_count, std::uint32_t first_vertex,
                  std::uint32_t first_instance);

    /**
     * @brief Uploads data into a device-local buffer.
     * @param device The device that owns the buffer.
     * @param desc The bytes, the size, and how the buffer will be bound.
     * @param out_buffer Receives the handle on success, and a null handle on failure.
     * @return Result::Success, or the reason the upload failed.
     */
    [[nodiscard]] Result create_buffer(Device* device, const BufferDesc& desc,
                                       BufferHandle* out_buffer);

    /**
     * @brief Releases a buffer and frees its slot for reuse.
     * @param device The device that owns the buffer.
     * @param buffer The handle to release. A null or stale handle does nothing.
     */
    void destroy_buffer(Device* device, BufferHandle buffer);

    /**
     * @brief Writes new contents into a uniform or a storage buffer.
     *
     * The buffer stays mapped, so this is a copy and nothing else. It works on a
     * BufferUsage::Uniform or a BufferUsage::Storage buffer, because those are
     * the two that live in host-visible memory. A vertex or an index buffer
     * lives in device-local memory the host cannot reach.
     *
     * @param device The device that owns the buffer.
     * @param buffer The buffer to write. A null or stale handle logs and does nothing.
     * @param data The bytes to copy in.
     * @param size How many bytes. It must not be more than the buffer holds.
     *
     * @warning This writes straight into memory the GPU may be reading. A buffer
     * a frame in flight still reads must not be written. Either wait for the
     * device, or keep one buffer for each frame in flight.
     */
    void update_buffer(Device* device, BufferHandle buffer, const void* data, std::size_t size);

    /**
     * @brief Uploads pixels into a sampled texture with its own descriptor set.
     * @param device The device that owns the texture.
     * @param desc The pixels and the size.
     * @param out_texture Receives the handle on success, and a null handle on failure.
     * @return Result::Success, or the reason the upload failed.
     */
    [[nodiscard]] Result create_texture(Device* device, const TextureDesc& desc,
                                        TextureHandle* out_texture);

    /**
     * @brief Creates an empty depth image that a pass renders into and samples.
     *
     * The image carries no pixels. One pass renders depth into it and a later
     * pass reads it, which is what a shadow map is. It uses the same depth
     * format the frame does, so a pipeline needs no second format to declare.
     *
     * The result is a TextureHandle and destroy_texture() releases it, because
     * it is a texture in every way that matters to a caller. Only the way it
     * gets its contents differs.
     *
     * @param device The device that owns the image.
     * @param desc The size and the sampler state.
     * @param out_texture Receives the handle on success, and a null handle on failure.
     * @return Result::Success, or the reason the image was not created.
     *
     * @warning The image starts in ResourceState::Undefined, and it holds
     * nothing until a pass has rendered into it. Sampling it before that reads
     * whatever the allocation held.
     *
     * @warning A GPU that cannot filter its depth format gets Filter::Nearest
     * whatever @p desc asked for, and the log says so. Vulkan allows a linear
     * comparison read without that feature and leaves the result
     * implementation-dependent, which is worse than a hard edge.
     */
    [[nodiscard]] Result create_depth_target(Device* device, const DepthTargetDesc& desc,
                                             TextureHandle* out_texture);

    /**
     * @brief Creates an empty color image that a pass renders into and samples.
     *
     * The color partner of create_depth_target(), and the same shape: the image
     * carries no pixels, one pass renders into it, and a later pass reads it.
     * destroy_texture() releases it.
     *
     * A scene renders into one of these in half float, and the tonemap pass
     * reads it and writes the swapchain. That intermediate is what stops a
     * value above 1 clipping at the moment the fragment shader writes it.
     *
     * @param device The device that owns the image.
     * @param desc The size, the format, and the sampler state.
     * @param out_texture Receives the handle on success, and a null handle on failure.
     * @return Result::Success, or the reason the image was not created.
     *
     * @warning The image starts in ResourceState::Undefined, and it holds
     * nothing until a pass has rendered into it.
     *
     * @warning A target the size of the window has to be rebuilt when the
     * window resizes, and anything that named the old handle has to be rebuilt
     * with it. The device does not do this for the caller.
     */
    [[nodiscard]] Result create_color_target(Device* device, const ColorTargetDesc& desc,
                                             TextureHandle* out_texture);

    /**
     * @brief Releases a texture and frees its slot for reuse.
     * @param device The device that owns the texture.
     * @param texture The handle to release. A null or stale handle does nothing.
     */
    void destroy_texture(Device* device, TextureHandle texture);

    /**
     * @brief Binds the vertex buffer that the draws that follow read.
     * @param commands The command list from begin_frame().
     * @param buffer The buffer to bind. A stale handle logs and does nothing.
     */
    void cmd_bind_vertex_buffer(CommandList* commands, BufferHandle buffer);

    /**
     * @brief Binds the index buffer that the draws that follow read.
     * @param commands The command list from begin_frame().
     * @param buffer The buffer to bind. It holds 32-bit indices.
     */
    void cmd_bind_index_buffer(CommandList* commands, BufferHandle buffer);

    /**
     * @brief Builds a descriptor set that a pipeline can bind.
     *
     * The set matches one of the layouts the pipeline was built with, which came
     * from the reflected shader. So a caller fills the bindings the shader
     * declares, and a binding the shader does not have is refused here rather
     * than by the driver later.
     *
     * Every binding the layout declares must appear in @p writes. A descriptor
     * left unwritten is undefined to read, and the validation layer reports it
     * far from the call that skipped it.
     *
     * @param device The device that owns the pipeline.
     * @param pipeline The pipeline whose layout the set must match.
     * @param set_index Which set of that pipeline this fills.
     * @param writes What to put in each binding. Order does not matter.
     * @param write_count How many entries @p writes holds.
     * @param out_set Receives the handle on success, and a null handle on failure.
     * @return Result::Success, or the reason the set could not be built.
     *
     * @code
     * const std::array<gfx::DescriptorWrite, 2> writes{ {
     *     { .binding = 0, .kind = gfx::DescriptorKind::CombinedImageSampler,
     *       .texture = base_color },
     *     { .binding = 5, .kind = gfx::DescriptorKind::UniformBuffer,
     *       .buffer = factors },
     * } };
     * gfx::create_descriptor_set(device, pipeline, 0, writes.data(), writes.size(), &set);
     * @endcode
     */
    [[nodiscard]] Result create_descriptor_set(Device* device, PipelineHandle pipeline,
                                               std::uint32_t set_index,
                                               const DescriptorWrite* writes,
                                               std::size_t write_count,
                                               DescriptorSetHandle* out_set);

    /**
     * @brief Releases a descriptor set and frees its slot for reuse.
     *
     * @param device The device that owns the set.
     * @param set The handle to release. A null or stale handle does nothing.
     *
     * @warning A set a frame in flight still reads must not be freed. Wait for
     * the device, or free it behind the frames.
     */
    void destroy_descriptor_set(Device* device, DescriptorSetHandle set);

    /**
     * @brief Binds a descriptor set for the draws that follow.
     *
     * @param commands The command list from begin_frame().
     * @param pipeline The bound pipeline, which supplies the layout.
     * @param set_index Which set this fills, matching create_descriptor_set().
     * @param set The set to bind. A stale handle logs and does nothing.
     */
    void cmd_bind_descriptor_set(CommandList* commands, PipelineHandle pipeline,
                                 std::uint32_t set_index, DescriptorSetHandle set);

    /**
     * @brief Sends push constants to the stages the pipeline declared.
     *
     * Which stages those are comes from
     * GraphicsPipelineDesc::push_constant_stages, so the caller does not repeat
     * it here and the two cannot disagree.
     *
     * @param commands The command list from begin_frame().
     * @param pipeline The bound pipeline, which supplies the layout.
     * @param data The bytes to send.
     * @param size How many bytes to send. It must match the pipeline's declared size.
     */
    void cmd_push_constants(CommandList* commands, PipelineHandle pipeline, const void* data,
                            std::uint32_t size);

    /**
     * @brief Draws with the bound index buffer.
     * @param commands The command list from begin_frame().
     * @param index_count How many indices to read.
     * @param instance_count How many instances to draw. Pass 1 for a single copy.
     * @param first_index The first index to read.
     * @param first_instance The value gl_InstanceIndex starts at.
     */
    void cmd_draw_indexed(CommandList* commands, std::uint32_t index_count,
                          std::uint32_t instance_count, std::uint32_t first_index,
                          std::uint32_t first_instance);

    /**
     * @brief Resets the timestamp query pool for a new frame.
     *
     * Call this at the start of the frame, before any cmd_write_timestamp().
     *
     * @param commands The command list from begin_frame().
     */
    void cmd_reset_timestamps(CommandList* commands);

    /**
     * @brief Writes a GPU timestamp into a query pool slot.
     *
     * Timestamps are written when the GPU reaches this point in the command
     * stream. Read them back a frame or two later with read_timestamps().
     *
     * @param commands The command list from begin_frame().
     * @param index Which pool slot to write. The caller allocates these.
     *
     * @warning Timestamps on different queues are not comparable.
     */
    void cmd_write_timestamp(CommandList* commands, std::uint32_t index);

    /**
     * @brief Reads GPU timestamp query results back.
     *
     * The queries must have finished (the frame fence signaled) before this
     * is called. The pool holds one query per slot, and each is 64 bits wide.
     *
     * @param device The device that owns the pool.
     * @param first The first query to read.
     * @param count How many queries to read.
     * @param out Where to write the results. Must hold at least @p count entries.
     * @return True when the results were read.
     */
    [[nodiscard]] bool read_timestamps(Device* device, std::uint32_t first, std::uint32_t count,
                                       std::uint64_t* out);

    /**
     * @brief How many ticks one nanosecond is, from the physical device.
     *
     * Multiply a timestamp delta by this to get nanoseconds.
     *
     * @param device The device.
     * @return The timestamp period in nanoseconds per tick. Zero when the device
     * supports no timestamps.
     */
    [[nodiscard]] float timestamp_period(Device* device);

} // namespace engine::gfx
