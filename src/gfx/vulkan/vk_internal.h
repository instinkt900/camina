#pragma once

/**
 * @file
 * @brief The backend structures that the backend source files share.
 *
 * Rule 4.2 shapes the public headers only. Inside this directory the code is
 * ordinary C++ and uses the standard library freely.
 */

#include "gfx/device.h"
#include "gfx/vulkan/vk_common.h"

#include <array>
#include <functional>
#include <vector>

namespace engine::gfx {

    /// @brief Where one frame records its commands, and the image it targets.
    struct CommandList {
        VkCommandBuffer buffer = VK_NULL_HANDLE;  ///< The buffer open for recording.
        VkImage target_image = VK_NULL_HANDLE;    ///< The swapchain image this frame draws into.
        VkImageView target_view = VK_NULL_HANDLE; ///< The view used as the color attachment.
        Extent2D extent;                          ///< The size of the target image.
        Device* owner = nullptr;                  ///< Resolves a handle without a second argument.
    };

    /// @brief One slot in the pipeline pool that PipelineHandle indexes.
    struct PipelineEntry {
        VkPipeline pipeline = VK_NULL_HANDLE;     ///< Null while the slot is free.
        VkPipelineLayout layout = VK_NULL_HANDLE; ///< Carries the push range and the set layouts.
        /**
         * @brief One layout for each set the reflected shader declares.
         *
         * The pipeline owns these, because they come from the shader it was
         * built with. They go away with it.
         */
        std::vector<VkDescriptorSetLayout> set_layouts;
        /// @brief Starts at 1, so slot 0 never produces the null handle value.
        std::uint32_t generation = 1;
        std::uint32_t push_constant_size = 0; ///< Checked by cmd_push_constants().
        bool alive = false;                   ///< Whether the slot holds a live pipeline.
    };

    /// @brief One slot in the buffer pool that BufferHandle indexes.
    struct BufferEntry {
        VkBuffer buffer = VK_NULL_HANDLE;          ///< Null while the slot is free.
        VmaAllocation allocation = VK_NULL_HANDLE; ///< The VMA block behind the buffer.
        /// @brief Where the buffer is mapped, or null for a device-local one.
        ///
        /// Only a uniform buffer is mapped. A vertex or an index buffer is
        /// staged once into device-local memory and never written again.
        void* mapped = nullptr;
        std::size_t size = 0;         ///< Bytes, so update_buffer() can refuse an overrun.
        std::uint32_t generation = 1; ///< Starts at 1, so slot 0 is never null.
        bool alive = false;           ///< Whether the slot holds a live buffer.
    };

    /// @brief One slot in the texture pool that TextureHandle indexes.
    struct TextureEntry {
        VkImage image = VK_NULL_HANDLE;            ///< Null while the slot is free.
        VmaAllocation allocation = VK_NULL_HANDLE; ///< The VMA block behind the image.
        VkImageView view = VK_NULL_HANDLE;         ///< The view the sampler reads.
        /**
         * @brief One view for each array layer, for rendering into it.
         *
         * Empty unless the texture is a depth target with layers. The sampler
         * reads ::view, which covers every layer at once, and an attachment
         * needs a view of one layer alone. The two cannot be the same object.
         */
        std::vector<VkImageView> layer_views;
        VkSampler sampler = VK_NULL_HANDLE; ///< Shared. The device sampler cache owns it.
        std::uint32_t width = 0;            ///< Texels across mip level 0.
        std::uint32_t height = 0;           ///< Texels down mip level 0.
        std::uint32_t generation = 1;       ///< Starts at 1, so slot 0 is never null.
        bool alive = false;                 ///< Whether the slot holds a live texture.
    };

    /**
     * @brief One slot in the descriptor set pool that DescriptorSetHandle indexes.
     *
     * A texture used to carry a set of its own, which allowed exactly one
     * texture for each draw. A material needs several textures and a block of
     * factors together, so a set is its own resource now.
     */
    struct DescriptorSetEntry {
        VkDescriptorSet set = VK_NULL_HANDLE; ///< Null while the slot is free.
        std::uint32_t generation = 1;         ///< Starts at 1, so slot 0 is never null.
        bool alive = false;                   ///< Whether the slot holds a live set.
    };

    /**
     * @brief One shared sampler, and the state it was built from.
     *
     * Textures do not own a sampler. They point at an entry here, and the entry
     * lives as long as the device. See gfx::SamplerDesc.
     */
    struct SamplerEntry {
        VkSampler sampler = VK_NULL_HANDLE; ///< The shared object.
        SamplerDesc desc;                   ///< The state that produced it. Also the cache key.
    };

    /// @brief One slot in the frames-in-flight ring.
    struct Frame {
        VkCommandPool pool = VK_NULL_HANDLE;          ///< Reset once per use, not per buffer.
        CommandList commands;                         ///< The recording context handed to the caller.
        VkSemaphore image_available = VK_NULL_HANDLE; ///< Signaled when the image is ready to draw.
        VkFence in_flight = VK_NULL_HANDLE;           ///< Signaled when this slot is free again.
    };

    /**
     * @brief Everything the backend owns for one GPU.
     *
     * The public header declares this type and never defines it, so no caller can
     * reach a Vulkan handle. See rule 4.1 in DESIGN.md.
     */
    struct Device {
        VkInstance instance = VK_NULL_HANDLE;                ///< The Vulkan instance.
        VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE; ///< Null unless validation is on.
        VkSurfaceKHR surface = VK_NULL_HANDLE;               ///< Built from the SDL window.
        VkPhysicalDevice physical = VK_NULL_HANDLE;          ///< The chosen GPU.
        VkPhysicalDeviceProperties properties{};             ///< Kept for device_name().
        VkDevice device = VK_NULL_HANDLE;                    ///< The logical device.
        VmaAllocator allocator = VK_NULL_HANDLE;             ///< Used from M1.3 onward.
        std::uint32_t graphics_family = 0;                   ///< Queue family that draws and presents.
        VkQueue graphics_queue = VK_NULL_HANDLE;             ///< The single queue this milestone uses.

        VkSwapchainKHR swapchain = VK_NULL_HANDLE;       ///< Rebuilt on every resize.
        VkFormat swapchain_format = VK_FORMAT_UNDEFINED; ///< The surface format in use.
        VkExtent2D swapchain_extent{};                   ///< The current image size.
        std::vector<VkImage> images;                     ///< Owned by the swapchain, not by us.
        std::vector<VkImageView> views;                  ///< One view for each image.
        /// @brief One semaphore for each image, signaled when its work finishes.
        std::vector<VkSemaphore> render_finished;

        VkImage depth_image = VK_NULL_HANDLE;            ///< Rebuilt with the swapchain.
        VmaAllocation depth_allocation = VK_NULL_HANDLE; ///< The VMA block behind the depth image.
        VkImageView depth_view = VK_NULL_HANDLE;         ///< The depth attachment view.
        VkFormat depth_format = VK_FORMAT_UNDEFINED;     ///< Chosen once at device creation.

        VkDescriptorPool descriptor_pool = VK_NULL_HANDLE; ///< Serves every descriptor set.
        VkCommandPool upload_pool = VK_NULL_HANDLE;        ///< Used by immediate_submit().
        std::vector<SamplerEntry> samplers;                ///< The sampler cache. Textures share these.

        std::vector<PipelineEntry> pipelines;      ///< Indexed by PipelineHandle::index().
        std::vector<std::uint32_t> free_pipelines; ///< Slots that destroy_pipeline() released.
        std::vector<BufferEntry> buffers;          ///< Indexed by BufferHandle::index().
        std::vector<std::uint32_t> free_buffers;   ///< Slots that destroy_buffer() released.
        std::vector<TextureEntry> textures;        ///< Indexed by TextureHandle::index().
        std::vector<std::uint32_t> free_textures;  ///< Slots that destroy_texture() released.
        /// @brief Indexed by DescriptorSetHandle::index().
        std::vector<DescriptorSetEntry> descriptor_sets;
        /// @brief Slots that destroy_descriptor_set() released.
        std::vector<std::uint32_t> free_descriptor_sets;

        std::array<Frame, kFramesInFlight> frames{}; ///< The frames-in-flight ring.
        std::uint32_t frame_index = 0;               ///< Which ring slot the next frame uses.
        std::uint32_t image_index = 0;               ///< Which swapchain image the open frame holds.
        bool frame_open = false;                     ///< True between begin_frame() and end_frame().
        bool vsync = true;                           ///< Chooses the present mode on rebuild.
    };

    namespace vk {

        /**
         * @brief Builds the swapchain, its image views, and the per-image semaphores.
         *
         * The caller must destroy any earlier swapchain first. A zero width or a
         * zero height means the window is minimized, so the call reports success
         * and leaves the swapchain null.
         *
         * @param device The device to build into.
         * @param size The wanted size in pixels. The surface may clamp it.
         * @return Result::Success, or the reason the build failed.
         */
        [[nodiscard]] Result create_swapchain(Device& device, Extent2D size);

        /**
         * @brief Destroys the swapchain, its views, and its semaphores.
         * @param device The device to clear. Calling this twice is safe.
         */
        void destroy_swapchain(Device& device);

        /**
         * @brief What one ResourceState means to Vulkan.
         *
         * A state names one usage, so it maps to exactly one stage mask, one
         * access mask, and one layout. That is the whole reason the vocabulary
         * carries no catch-all value: `ALL_COMMANDS` on both sides of a barrier
         * is what a catch-all would have to become.
         */
        struct StateMapping {
            VkPipelineStageFlags2 stage = VK_PIPELINE_STAGE_2_NONE; ///< When it happens.
            VkAccessFlags2 access = VK_ACCESS_2_NONE;               ///< What it does.
            VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;       ///< How it is stored.
        };

        /**
         * @brief Turns a state into the stage, the access, and the layout it means.
         * @param state The state to map.
         * @return The mapping. Every state has one.
         */
        [[nodiscard]] StateMapping map_state(ResourceState state);

        /**
         * @brief Records a layout change for one whole image.
         *
         * Dynamic rendering has no render pass to move layouts, so each frame
         * moves its images itself. This uses synchronization2, per DESIGN.md
         * section 2.
         *
         * The two states decide both sides of the barrier, so it waits on the
         * stages that were actually used and blocks the stages that will
         * actually run. See map_state().
         *
         * @param buffer The command buffer that is open for recording.
         * @param image The image to move.
         * @param from The state the image is in now.
         * @param to The state the image must reach.
         * @param aspect Which aspect to move, colour or depth.
         */
        void transition_image(VkCommandBuffer buffer, VkImage image, ResourceState from,
                              ResourceState to, VkImageAspectFlags aspect);

        /**
         * @brief Looks up a pipeline slot.
         * @param device The device that owns the pool.
         * @param handle The handle to resolve.
         * @return The live entry, or nullptr when the handle is null or stale.
         */
        [[nodiscard]] PipelineEntry* resolve_pipeline(Device& device, PipelineHandle handle);

        /**
         * @brief Looks up a buffer slot.
         * @param device The device that owns the pool.
         * @param handle The handle to resolve.
         * @return The live entry, or nullptr when the handle is null or stale.
         */
        [[nodiscard]] BufferEntry* resolve_buffer(Device& device, BufferHandle handle);

        /**
         * @brief Looks up a texture slot.
         * @param device The device that owns the pool.
         * @param handle The handle to resolve.
         * @return The live entry, or nullptr when the handle is null or stale.
         */
        [[nodiscard]] TextureEntry* resolve_texture(Device& device, TextureHandle handle);

        /**
         * @brief Looks up a descriptor set slot.
         * @param device The device that owns the pool.
         * @param handle The handle to resolve.
         * @return The live entry, or nullptr when the handle is null or stale.
         */
        [[nodiscard]] DescriptorSetEntry* resolve_descriptor_set(Device& device,
                                                                 DescriptorSetHandle handle);

        /**
         * @brief Destroys every live pipeline and clears the pool.
         *
         * The caller must make sure the GPU is idle first.
         *
         * @param device The device whose pool to clear.
         */
        void destroy_pipelines(Device& device);

        /// @brief Destroys every live buffer and clears the pool.
        /// @param device The device whose pool to clear.
        void destroy_buffers(Device& device);

        /// @brief Destroys every live texture and clears the pool.
        /// @param device The device whose pool to clear.
        void destroy_textures(Device& device);

        /**
         * @brief Finds the shared sampler for a state, and builds it once.
         *
         * The cache holds one entry for each distinct state, so two textures
         * that ask for the same filtering share one VkSampler.
         *
         * @param device The device that owns the cache.
         * @param desc The wanted sampler state.
         * @param out_sampler Receives the shared sampler. The caller does not own it.
         * @return Result::Success, or the reason the sampler did not build.
         */
        [[nodiscard]] Result resolve_sampler(Device& device, const SamplerDesc& desc,
                                             VkSampler* out_sampler);

        /// @brief Destroys every cached sampler. Call it after destroy_textures().
        /// @param device The device whose cache to clear.
        void destroy_samplers(Device& device);

        /**
         * @brief Builds the depth image at the current swapchain size.
         *
         * Reverse-Z needs a float depth format, per DESIGN.md section 3.
         *
         * @param device The device to build into.
         * @return Result::Success, or the reason the image did not build.
         */
        [[nodiscard]] Result create_depth_image(Device& device);

        /// @brief Destroys the depth image and its view. Calling this twice is safe.
        /// @param device The device to clear.
        void destroy_depth_image(Device& device);

        /**
         * @brief Creates the texture set layout, the descriptor pool, and the upload pool.
         * @param device The device to build into.
         * @return Result::Success, or the reason a resource did not build.
         */
        [[nodiscard]] Result create_shared_resources(Device& device);

        /// @brief Destroys what create_shared_resources() built.
        /// @param device The device to clear.
        void destroy_shared_resources(Device& device);

        /**
         * @brief Records and runs one command buffer, then waits for it.
         *
         * Used for staging copies and layout changes at upload time. It is
         * deliberately simple and blocking. M4 replaces it with a transfer queue.
         *
         * @param device The device to submit on.
         * @param record Fills the command buffer. It runs once.
         * @return Result::Success, or the reason the submit failed.
         */
        [[nodiscard]] Result immediate_submit(Device& device,
                                              const std::function<void(VkCommandBuffer)>& record);

    } // namespace vk

} // namespace engine::gfx
