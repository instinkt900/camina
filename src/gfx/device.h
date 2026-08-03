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
     * @brief Opens dynamic rendering into the current swapchain image.
     *
     * The image loads with a clear to @p clear_color and stores at the end. There
     * is no render pass object and no framebuffer, per DESIGN.md section 2.
     *
     * @param commands The command list from begin_frame().
     * @param clear_color The linear color to clear to.
     */
    void cmd_begin_rendering(CommandList* commands, const ColorRGBA& clear_color);

    /**
     * @brief Closes the rendering scope that cmd_begin_rendering() opened.
     * @param commands The command list from begin_frame().
     */
    void cmd_end_rendering(CommandList* commands);

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
     * @brief Uploads pixels into a sampled texture with its own descriptor set.
     * @param device The device that owns the texture.
     * @param desc The pixels and the size.
     * @param out_texture Receives the handle on success, and a null handle on failure.
     * @return Result::Success, or the reason the upload failed.
     */
    [[nodiscard]] Result create_texture(Device* device, const TextureDesc& desc,
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
     * @brief Binds a texture at set 0, binding 0.
     *
     * The pipeline must have asked for it with GraphicsPipelineDesc::sample_texture.
     *
     * @param commands The command list from begin_frame().
     * @param pipeline The bound pipeline, which supplies the layout.
     * @param texture The texture to bind.
     */
    void cmd_bind_texture(CommandList* commands, PipelineHandle pipeline, TextureHandle texture);

    /**
     * @brief Sends push constants to the vertex stage.
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

} // namespace engine::gfx
