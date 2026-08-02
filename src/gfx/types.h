#pragma once

/**
 * @file
 * @brief Plain data types shared by the whole gfx interface.
 *
 * Rule 4.2 in DESIGN.md keeps this header C-compatible. There is no
 * `std::string`, no `std::vector`, no virtual function, and no exception. The
 * cost today is nothing. The gain later is that the plugin ABI becomes a header
 * rename instead of a rewrite.
 */

#include "core/handle.h"

#include <cstddef>
#include <cstdint>

/// @brief The public render interface. The backend behind it stays hidden.
namespace engine::gfx {

    /// @brief Distinguishes a pipeline handle from every other kind of handle.
    struct PipelineTag {};

    /**
     * @brief Refers to a graphics pipeline the device owns.
     *
     * The handle is 8 bytes and carries a generation, so a stale handle resolves
     * to nothing instead of to the wrong pipeline. See rule 4.2 in DESIGN.md.
     */
    using PipelineHandle = Handle<PipelineTag>;

    /**
     * @brief A compiled SPIR-V module, as a pointer and a word count.
     *
     * Rule 4.2 forbids a container in this interface, so the caller keeps the
     * storage. The words must stay alive only for the create call.
     */
    struct ShaderCode {
        const std::uint32_t* spirv = nullptr; ///< The first word of the module.
        std::size_t word_count = 0;           ///< How many 32-bit words follow.
    };

    /// @brief Distinguishes a buffer handle from every other kind of handle.
    struct BufferTag {};

    /// @brief Refers to a GPU buffer the device owns.
    using BufferHandle = Handle<BufferTag>;

    /// @brief Distinguishes a texture handle from every other kind of handle.
    struct TextureTag {};

    /// @brief Refers to a sampled texture the device owns.
    using TextureHandle = Handle<TextureTag>;

    /// @brief What a buffer is bound as.
    enum class BufferUsage : std::uint32_t {
        Vertex = 0, ///< Bound with cmd_bind_vertex_buffer().
        Index,      ///< Bound with cmd_bind_index_buffer(). Holds 32-bit indices.
    };

    /**
     * @brief Settings for create_buffer().
     *
     * The device copies the data through a staging buffer, so the memory ends up
     * in device-local storage. The source only has to live for the create call.
     */
    struct BufferDesc {
        const void* data = nullptr;              ///< The bytes to upload. Required.
        std::size_t size = 0;                    ///< How many bytes to upload.
        BufferUsage usage = BufferUsage::Vertex; ///< How the buffer will be bound.
    };

    /**
     * @brief Settings for create_texture().
     *
     * The pixels are 8 bits for each channel in RGBA order, and the device reads
     * them as sRGB. DESIGN.md section 3 converts to linear at the texture read.
     */
    struct TextureDesc {
        const void* pixels = nullptr; ///< Width times height times 4 bytes. Required.
        std::uint32_t width = 0;      ///< Width in texels.
        std::uint32_t height = 0;     ///< Height in texels.
    };

    /// @brief The element type of one vertex attribute.
    enum class VertexFormat : std::uint32_t {
        Float2 = 0, ///< Two 32-bit floats.
        Float3,     ///< Three 32-bit floats.
    };

    /// @brief One input to the vertex shader.
    struct VertexAttribute {
        std::uint32_t location = 0;                 ///< The `layout(location = N)` in the shader.
        std::uint32_t offset = 0;                   ///< Byte offset inside one vertex.
        VertexFormat format = VertexFormat::Float3; ///< The element type.
    };

    /// @brief Settings for create_graphics_pipeline().
    struct GraphicsPipelineDesc {
        ShaderCode vertex;   ///< The vertex stage. Required.
        ShaderCode fragment; ///< The fragment stage. Required.

        /// @brief The vertex attributes, or null to build positions from the vertex index.
        const VertexAttribute* attributes = nullptr;
        std::size_t attribute_count = 0; ///< How many entries @c attributes holds.
        std::uint32_t vertex_stride = 0; ///< Bytes from one vertex to the next.

        /// @brief How many bytes of push constants the vertex stage reads. 0 for none.
        std::uint32_t push_constant_size = 0;
        /// @brief Whether the fragment stage samples the texture at set 0, binding 0.
        bool sample_texture = false;
        /// @brief Whether to test and write depth. Reverse-Z keeps nearer fragments.
        bool depth_test = false;
        /**
         * @brief Whether to cull back faces.
         *
         * A front face is counter-clockwise, as glTF supplies it. Vulkan clip
         * space puts +Y down, and the projection in `math/conventions.h` negates
         * the Y row, so the two cancel and no winding change is needed.
         */
        bool cull_back = false;
    };

    /// @brief The outcome of a gfx call.
    enum class Result : std::uint32_t {
        Success = 0,      ///< The call did what it says.
        OutOfDate,        ///< The swapchain no longer matches the window. Resize, then retry.
        ErrorInit,        ///< The loader or the instance failed to start.
        ErrorNoDevice,    ///< No physical device meets the requirements.
        ErrorSurface,     ///< The window surface failed, or the driver lost it.
        ErrorDeviceLost,  ///< The driver reset the device or removed it.
        ErrorOutOfMemory, ///< Host memory or device memory ran out.
        ErrorUnknown,     ///< The backend failed and no other value fits.
    };

    /**
     * @brief A short name for a Result.
     * @param result The value to name.
     * @return A static string. The caller must not free it.
     */
    [[nodiscard]] const char* result_name(Result result);

    /**
     * @brief Whether a Result reports success.
     * @param result The value to test.
     * @return True only for Result::Success. Result::OutOfDate is not a success.
     */
    [[nodiscard]] constexpr bool succeeded(Result result) {
        return result == Result::Success;
    }

    /// @brief A width and a height in pixels.
    struct Extent2D {
        std::uint32_t width = 0;  ///< Width in pixels.
        std::uint32_t height = 0; ///< Height in pixels.
    };

    /**
     * @brief A color in the linear working space, with straight alpha.
     *
     * DESIGN.md section 3 sets linear as the working space. The backend converts
     * to sRGB at the final write, so do not pre-convert a value you put here.
     */
    struct ColorRGBA {
        float r = 0.0F; ///< Red, linear.
        float g = 0.0F; ///< Green, linear.
        float b = 0.0F; ///< Blue, linear.
        float a = 1.0F; ///< Alpha, straight.
    };

    /**
     * @brief How many frames the device records before it waits for the oldest.
     *
     * Two lets the CPU record frame N+1 while the GPU runs frame N. Three adds
     * latency for little gain at this stage.
     */
    inline constexpr std::uint32_t kFramesInFlight = 2;

    /// @brief Settings for create_device().
    struct DeviceDesc {
        /// @brief The `SDL_Window` to draw into, as an opaque pointer. Required.
        void* window = nullptr;
        /// @brief The application name reported to the driver.
        const char* app_name = "camina";
        /// @brief Whether to turn on the Vulkan validation layer and the debug messenger.
        bool enable_validation = false;
        /// @brief Whether to wait for vertical blank. False selects the lowest latency mode.
        bool vsync = true;
    };

} // namespace engine::gfx
