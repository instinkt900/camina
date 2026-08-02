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
        VkPipelineLayout layout = VK_NULL_HANDLE; ///< Empty layout until M1.3 adds descriptors.
        /// @brief Starts at 1, so slot 0 never produces the null handle value.
        std::uint32_t generation = 1;
        bool alive = false; ///< Whether the slot holds a live pipeline.
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

        std::vector<PipelineEntry> pipelines;      ///< Indexed by PipelineHandle::index().
        std::vector<std::uint32_t> free_pipelines; ///< Slots that destroy_pipeline() released.

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
         * @brief Records a layout change for one whole color image.
         *
         * Dynamic rendering has no render pass to move layouts, so each frame
         * moves its image itself. This uses synchronization2, per DESIGN.md
         * section 2.
         *
         * @param buffer The command buffer that is open for recording.
         * @param image The image to move.
         * @param from The layout the image is in now.
         * @param to The layout the image must reach.
         */
        void transition_image(VkCommandBuffer buffer, VkImage image, VkImageLayout from,
                              VkImageLayout to);

        /**
         * @brief Destroys every live pipeline and clears the pool.
         *
         * The caller must make sure the GPU is idle first.
         *
         * @param device The device whose pool to clear.
         */
        void destroy_pipelines(Device& device);

    } // namespace vk

} // namespace engine::gfx
