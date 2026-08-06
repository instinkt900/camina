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

    /// @brief Distinguishes a descriptor set handle from every other kind.
    struct DescriptorSetTag {};

    /**
     * @brief Refers to a descriptor set the device owns.
     *
     * A set holds the textures and the buffers one draw call reads together. A
     * material is the case this exists for: five textures and a block of
     * factors, bound once for every submesh that uses them.
     */
    using DescriptorSetHandle = Handle<DescriptorSetTag>;

    /// @brief What a buffer is bound as.
    enum class BufferUsage : std::uint32_t {
        Vertex = 0, ///< Bound with cmd_bind_vertex_buffer().
        Index,      ///< Bound with cmd_bind_index_buffer(). Holds 32-bit indices.
        Uniform,    ///< Read through a descriptor set, as a block of parameters.
        /**
         * @brief Read through a descriptor set, as an array the shader indexes.
         *
         * The difference from Uniform is size and shape rather than speed. A
         * uniform block is a fixed set of named fields, so an array inside one
         * has a length the shader declares. A storage buffer is an array whose
         * length the shader does not need to know. That is what carries a light
         * count that is a number rather than a constant.
         *
         * Like Uniform, this lives in host-visible memory and stays mapped, so
         * update_buffer() writes it. See DESIGN.md section 9 and issue #98.
         */
        Storage,
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

    /// @brief How a sampler picks a color between texel centers.
    enum class Filter : std::uint32_t {
        Linear = 0, ///< Blend the nearest texels. The right default for a photograph.
        Nearest,    ///< Take the nearest texel. Keeps a texel grid crisp.
    };

    /// @brief What a sampler reads outside the 0 to 1 range.
    enum class AddressMode : std::uint32_t {
        Repeat = 0,     ///< Tile the texture.
        ClampToEdge,    ///< Hold the edge texel.
        MirroredRepeat, ///< Tile, and flip every other copy.
        /**
         * @brief Hold a border of zero, which reads as "nothing in the way".
         *
         * A shadow map needs this. A fragment outside the light's view has no
         * depth recorded for it, and repeating or clamping would carry an edge
         * texel across the whole scene as a smear of shadow.
         *
         * Zero is the far plane under reverse-Z, and a surface is lit when its
         * own depth is at or in front of the stored one. So every fragment
         * passes against this border. Under a conventional depth range the same
         * argument asks for one instead, which is why this names the value
         * rather than a color.
         */
        ClampToZeroBorder,
    };

    /**
     * @brief The sampler state a texture reads through.
     *
     * A driver keeps a small number of distinct sampler states, so the device
     * shares one VkSampler between every texture that asks for the same state.
     * A scene with 500 textures holds about 5 samplers, not 500.
     */
    struct SamplerDesc {
        Filter filter = Filter::Linear;            ///< Applies to both magnify and minify.
        AddressMode address = AddressMode::Repeat; ///< Applies to all three axes.
        /**
         * @brief Whether the sampler compares rather than returns the texel.
         *
         * A shadow map is read this way. The sampler compares the stored depth
         * against a value the shader supplies and returns how much of the
         * neighbourhood passed, so a linear filter gives four taps of percentage
         * closer filtering for the cost of one read.
         *
         * The comparison is "greater or equal", which is what reverse-Z needs: a
         * surface is lit when its own depth is at or in front of the depth the
         * light recorded. A shader reads such a texture as a `sampler2DShadow`.
         */
        bool compare = false;
    };

    /**
     * @brief Settings for create_depth_target().
     *
     * A depth target carries no pixels. The device allocates it empty, a pass
     * renders into it, and a later pass samples it. That is the difference from
     * TextureDesc, which uploads bytes the caller already has.
     */
    struct DepthTargetDesc {
        std::uint32_t width = 0;  ///< Width in texels. Required.
        std::uint32_t height = 0; ///< Height in texels. Required.
        /**
         * @brief How many array layers the image holds. 1 for a plain image.
         *
         * A cascade set is one image with a layer for each cascade. A shader
         * reads the whole thing as one `sampler2DArrayShadow` and picks a layer,
         * which is what keeps the selection to one sampler and no branch over
         * several bindings.
         *
         * A pass renders into one layer at a time, so
         * cmd_begin_depth_rendering() names which.
         */
        std::uint32_t layer_count = 1;
        SamplerDesc sampler; ///< How a later pass reads it. Shared, not owned.
    };

    /**
     * @brief The format of a color attachment.
     *
     * This is a separate enum from TextureFormat, for two reasons. A render
     * target has to be able to say "whatever the swapchain chose", and that
     * format is picked at run time from what the surface offers, so no compile
     * time name can stand for it. And most of TextureFormat can never be a
     * render target, because a GPU does not write BC7 blocks from a fragment
     * shader.
     *
     * A pipeline and the target it draws into name the same value here. Vulkan
     * calls a mismatch undefined rather than an error, so the two have to agree
     * and this is the one word they agree on.
     */
    enum class ColorTargetFormat : std::uint32_t {
        /**
         * @brief Whatever create_swapchain() chose, which is an 8-bit sRGB format.
         *
         * The final write of a frame goes here, and the hardware converts from
         * linear to sRGB as it lands. See DESIGN.md section 3.
         */
        Swapchain = 0,
        /**
         * @brief Four 16-bit half floats.
         *
         * What a scene renders into before it is tonemapped. A half float
         * carries values above 1, which is the whole point: an sRGB target
         * clips them at the moment the fragment shader writes, and nothing
         * after that can recover them.
         */
        RGBA16F,
    };

    /**
     * @brief Settings for create_color_target().
     *
     * The color partner of DepthTargetDesc. The image carries no pixels: one
     * pass renders color into it and a later pass samples it. That pair is what
     * an intermediate target is, and it is what a tonemap needs.
     */
    struct ColorTargetDesc {
        std::uint32_t width = 0;  ///< Width in texels. Required.
        std::uint32_t height = 0; ///< Height in texels. Required.
        /// @brief What the texels hold. The pipeline that draws into it declares the same.
        ColorTargetFormat format = ColorTargetFormat::RGBA16F;
        /**
         * @brief How a later pass reads it. Shared, not owned.
         *
         * AddressMode::ClampToEdge is the sane default for a full-screen read.
         * A target the size of the window is sampled at the texel centers it
         * already has, so nothing should ever reach outside it, and clamping is
         * what keeps a rounding error at the edge from wrapping to the far side.
         */
        SamplerDesc sampler{ .filter = Filter::Linear, .address = AddressMode::ClampToEdge };
    };

    /**
     * @brief How the texels of a texture are stored, and how a shader reads them.
     *
     * The sRGB entries make the sampler convert to linear on read, which is what
     * DESIGN.md section 3 asks for. The Unorm entries hand the texel through
     * unchanged, which is what a normal map or a roughness map needs.
     *
     * A BC7 entry needs the `textureCompressionBC` device feature. Every desktop
     * GPU that runs Vulkan 1.3 has it. create_texture() reports it by name when
     * a device does not.
     */
    enum class TextureFormat : std::uint32_t {
        RGBA8Srgb = 0, ///< Four 8-bit channels, converted from sRGB on read.
        RGBA8Unorm,    ///< Four 8-bit channels, read as they are.
        BC7Srgb,       ///< BC7 blocks, converted from sRGB on read.
        BC7Unorm,      ///< BC7 blocks, read as they are.
        /**
         * @brief Four 16-bit half floats, read as they are.
         *
         * There is no sRGB partner for this one, and there must not be. A half
         * float already carries values above 1, which is the whole reason an
         * HDR environment uses it, and an sRGB transfer function is defined
         * only from 0 to 1.
         */
        RGBA16F,
    };

    /**
     * @brief Settings for create_texture().
     *
     * The pixels hold every mip level, largest first, packed with no padding
     * between them. That is the layout a cooked texture file already has, so the
     * caller passes a pointer into the bytes it read and the device copies once.
     *
     * @warning @c size must cover every level. The device works out where each
     * level starts from the format and the extent, and a short buffer would make
     * it read past the end.
     */
    struct TextureDesc {
        const void* pixels = nullptr; ///< The first byte of mip level 0. Required.
        std::size_t size = 0;         ///< Bytes of every level together. Required.
        std::uint32_t width = 0;      ///< Width of mip level 0, in texels.
        std::uint32_t height = 0;     ///< Height of mip level 0, in texels.
        std::uint32_t mip_count = 1;  ///< How many levels @c pixels holds. At least 1.
        /**
         * @brief 1 for a flat texture, or 6 for a cubemap.
         *
         * Six makes a texture the shader reads as a `samplerCube`. The pixels
         * hold every level of face 0, then every level of face 1, and so on.
         * The faces run +X, −X, +Y, −Y, +Z, −Z, and a cubemap face is square.
         */
        std::uint32_t face_count = 1;
        TextureFormat format = TextureFormat::RGBA8Srgb; ///< How the texels are stored.
        SamplerDesc sampler;                             ///< How the shader reads it. Shared, not owned.
    };

    /// @brief The element type of one vertex attribute.
    enum class VertexFormat : std::uint32_t {
        Float2 = 0, ///< Two 32-bit floats.
        Float3,     ///< Three 32-bit floats.
        Float4,     ///< Four 32-bit floats. A tangent, with its sign in w.
    };

    /// @brief One input to the vertex shader.
    struct VertexAttribute {
        std::uint32_t location = 0;                 ///< The `layout(location = N)` in the shader.
        std::uint32_t offset = 0;                   ///< Byte offset inside one vertex.
        VertexFormat format = VertexFormat::Float3; ///< The element type.
    };

    /// @brief What kind of resource one descriptor binding holds.
    enum class DescriptorKind : std::uint32_t {
        CombinedImageSampler = 0, ///< A texture and its sampler together.
        UniformBuffer,            ///< A read-only block of parameters.
        StorageBuffer,            ///< A read-write block.
    };

    /// @brief The stage bit for the vertex stage, used in DescriptorBinding::stages.
    inline constexpr std::uint32_t kStageBitVertex = 1U << 0U;
    /// @brief The stage bit for the fragment stage, used in DescriptorBinding::stages.
    inline constexpr std::uint32_t kStageBitFragment = 1U << 1U;
    /// @brief The stage bit for the compute stage, used in DescriptorBinding::stages.
    inline constexpr std::uint32_t kStageBitCompute = 1U << 2U;

    /**
     * @brief One descriptor a pipeline reads.
     *
     * The caller builds these from a cooked shader, which got them by reflecting
     * the SPIR-V. Nothing here is written by hand, which is the point: a layout
     * a person maintained beside the shader used to drift from it silently. See
     * DESIGN.md section 9 "Shader pipeline".
     */
    struct DescriptorBinding {
        std::uint32_t set = 0;     ///< The `layout(set = N)` in the shader.
        std::uint32_t binding = 0; ///< The `layout(binding = N)` in the shader.
        std::uint32_t count = 1;   ///< Array length, or 1 for a plain declaration.
        std::uint32_t stages = 0;  ///< Which stages read it, as ::kStageBitVertex and friends.
        /// @brief What the binding holds.
        DescriptorKind kind = DescriptorKind::CombinedImageSampler;
    };

    /**
     * @brief One entry to write into a descriptor set.
     *
     * The kind decides which handle is read. A CombinedImageSampler reads
     * @c texture, and a UniformBuffer or a StorageBuffer reads @c buffer. The
     * other stays null, because rule 4.2 keeps this a POD struct rather than a
     * variant.
     */
    struct DescriptorWrite {
        std::uint32_t binding = 0; ///< The `layout(binding = N)` this fills.
        /// @brief What the binding holds, which decides the handle below.
        DescriptorKind kind = DescriptorKind::CombinedImageSampler;
        TextureHandle texture; ///< The texture, for a CombinedImageSampler.
        BufferHandle buffer;   ///< The buffer, for a UniformBuffer or StorageBuffer.
    };

    /// @brief Settings for create_graphics_pipeline().
    struct GraphicsPipelineDesc {
        ShaderCode vertex; ///< The vertex stage. Required.
        /// @brief The fragment stage. Required unless @c depth_only is true.
        ShaderCode fragment;

        /// @brief The vertex attributes, or null to build positions from the vertex index.
        const VertexAttribute* attributes = nullptr;
        std::size_t attribute_count = 0; ///< How many entries @c attributes holds.
        std::uint32_t vertex_stride = 0; ///< Bytes from one vertex to the next.

        /// @brief How many bytes of push constants the pipeline reads. 0 for none.
        std::uint32_t push_constant_size = 0;
        /**
         * @brief Which stages read the push constants, as kStageBit values.
         *
         * The default is the vertex stage, because a model matrix is what a push
         * constant carried first. A pass whose fragment stage needs one says so
         * here, and the exposure the tonemap applies is the first of those.
         *
         * @warning This must name every stage that declares the block. Vulkan
         * matches the range in the layout against the stages cmd_push_constants()
         * writes, and a stage that reads a block nobody wrote to it reads
         * undefined values rather than reporting anything.
         */
        std::uint32_t push_constant_stages = kStageBitVertex;

        /**
         * @brief The descriptors the pipeline reads, or null for none.
         *
         * These come from the cooked shaders, which carry what SPIRV-Reflect
         * found. The two stages are merged by the caller, so a binding both
         * stages read appears once with both stage bits set.
         *
         * @warning The entries must be sorted by set and then by binding, and no
         * set may be skipped. Vulkan numbers set layouts by position, so a gap
         * would silently shift every set after it.
         */
        const DescriptorBinding* bindings = nullptr;
        std::size_t binding_count = 0; ///< How many entries @c bindings holds.
        /**
         * @brief Whether to test and write depth. Reverse-Z keeps nearer fragments.
         *
         * This does not decide whether a depth attachment is present. Every frame
         * attaches one, and every pipeline declares its format. A pipeline that
         * leaves this false still renders into the same attachment, and simply
         * does not read or write it.
         */
        bool depth_test = false;
        /**
         * @brief Whether a fragment that passes the depth test writes its depth.
         *
         * This does nothing while @c depth_test is false, because a pipeline that
         * does not read depth does not write it either.
         *
         * A blended surface leaves this false. Blending reads what is already in
         * the attachment, so two transparent surfaces must both survive the depth
         * test. One that wrote its depth would hide the other.
         */
        bool depth_write = true;
        /**
         * @brief Whether to blend the fragment over the attachment.
         *
         * False replaces the attachment, which is what an opaque surface wants.
         * True blends by source alpha, which is the "over" operator glTF asks for
         * with `alphaMode` `BLEND`.
         *
         * @warning Blending depends on draw order. The caller sorts back to
         * front, because nothing in the pipeline can do it.
         */
        bool blend = false;
        /**
         * @brief Whether to cull back faces.
         *
         * A front face is counter-clockwise, as glTF supplies it. Vulkan clip
         * space puts +Y down, and the projection in `math/conventions.h` negates
         * the Y row, so the two cancel and no winding change is needed.
         */
        bool cull_back = false;
        /**
         * @brief Whether the pipeline renders depth alone, with no color target.
         *
         * A shadow pass wants this. It attaches no color image, so the pipeline
         * must declare none, and it needs no fragment stage at all because
         * nothing consumes a color. Leaving the fragment stage out is what makes
         * the pass cheap, since the fixed-function depth write is the whole job.
         *
         * @warning A pipeline built with this draws only inside a
         * cmd_begin_depth_rendering() scope. The attachments a pipeline declares
         * must match the ones the scope binds, and Vulkan calls a mismatch
         * undefined rather than an error.
         */
        bool depth_only = false;
        /**
         * @brief The format of the color attachment the pipeline draws into.
         *
         * It has to match the target the rendering scope binds, because Vulkan
         * calls a mismatch undefined rather than an error. A pipeline the mesh
         * pass builds names ColorTargetFormat::RGBA16F, and one that writes the
         * frame out names ColorTargetFormat::Swapchain.
         *
         * @c depth_only ignores this, because such a pipeline attaches no color
         * image at all.
         */
        ColorTargetFormat color_format = ColorTargetFormat::Swapchain;
    };

    /**
     * @brief What a pass does with a resource, which is what decides a barrier.
     *
     * This is the whole vocabulary the render graph speaks. A pass says which
     * state it needs a resource in, the graph compares that against the state
     * the resource is already in, and a barrier is the difference between two
     * of these values. See DESIGN.md section 9.
     *
     * Each value names one usage rather than a set of them, and there is no
     * catch-all on purpose. A state that meant "anything" would map to
     * `ALL_COMMANDS` on both sides of every barrier, which is the shortcut M5.3
     * exists to remove. So the backend can turn each of these into one stage
     * mask, one access mask, and one image layout with nothing left over.
     *
     * The read states and the write states are not marked apart here, because
     * the caller says which it means by putting the access in ResourceRead or
     * in ResourceWrite.
     */
    enum class ResourceState : std::uint32_t {
        /// @brief No contents worth keeping. What a resource starts a frame in.
        Undefined = 0,
        /// @brief Written by the fragment stage as a color attachment.
        ColorTarget,
        /// @brief Written by the depth test as a depth attachment.
        DepthTarget,
        /// @brief Tested against but not written, so several passes may share it.
        DepthRead,
        /// @brief Sampled or read by a shader, in any stage that declares it.
        ShaderRead,
        /// @brief Written by a compute shader through a storage binding.
        ComputeWrite,
        /// @brief The source of a copy.
        CopySource,
        /// @brief The destination of a copy.
        CopyDestination,
        /// @brief Handed to the presentation engine. The last state of a frame.
        Present,
    };

    /**
     * @brief A short name for a ResourceState.
     * @param state The value to name.
     * @return A static string. The caller must not free it.
     */
    [[nodiscard]] const char* resource_state_name(ResourceState state);

    /**
     * @brief Which image of the frame a barrier means.
     *
     * A frame owns its color target and its depth target, and neither is a
     * TextureHandle: the swapchain hands the color image over on every
     * acquire, and the depth image belongs to the swapchain beside it. So a
     * barrier on one of them names it this way rather than by handle.
     *
     * A pass that owns a transient of its own will need handles here. Nothing
     * does yet, and rule 4.6 says to add that when a pass asks for it.
     */
    enum class FrameTarget : std::uint32_t {
        Color = 0, ///< The image that gets presented.
        Depth,     ///< The depth buffer beside it.
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

    /// @brief The offscreen size a device renders at when it has no window.
    inline constexpr std::uint32_t kDefaultOffscreenWidth = 1280;
    /// @brief The offscreen height that goes with kDefaultOffscreenWidth.
    inline constexpr std::uint32_t kDefaultOffscreenHeight = 720;

    /// @brief Settings for create_device().
    struct DeviceDesc {
        /**
         * @brief The `SDL_Window` to draw into, as an opaque pointer.
         *
         * Null renders offscreen: no surface, no swapchain, and no window ever
         * appears. The device then draws into images it owns, at
         * ::offscreen_extent, and capture_frame() reads the last one.
         *
         * Nothing else changes. The passes, the barriers, and the color format
         * are the same either way, because a test that rendered differently
         * from the program would be testing something that does not ship.
         */
        void* window = nullptr;
        /// @brief The application name reported to the driver.
        const char* app_name = "camina";
        /// @brief Whether to turn on the Vulkan validation layer and the debug messenger.
        bool enable_validation = false;
        /**
         * @brief Whether to also turn on synchronization validation.
         *
         * This is the check that reads the barriers rather than the calls. It
         * reports a read that races a write, which is the failure a wrong
         * barrier produces and the one that shows on one vendor and not
         * another. It costs real time on every frame, so it is off by default
         * and a person turns it on while they work on a pass.
         *
         * It does nothing unless ::enable_validation is also true.
         */
        bool enable_sync_validation = false;
        /// @brief Whether to wait for vertical blank. False selects the lowest latency mode.
        bool vsync = true;
        /**
         * @brief The size to render at when there is no window.
         *
         * Ignored when @c window is set, because the window decides the size
         * then. With no window this is the whole answer, so two runs at the same
         * value produce images that can be compared texel for texel.
         */
        Extent2D offscreen_extent{ kDefaultOffscreenWidth, kDefaultOffscreenHeight };
    };

} // namespace engine::gfx
