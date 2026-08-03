#include "gfx/vulkan/vk_internal.h"

#include "core/log.h"

#include <algorithm>
#include <vector>

namespace engine::gfx {

    namespace {

        /// Picks an sRGB surface format so the driver converts on the final write.
        /// DESIGN.md section 3 keeps the working space linear and converts once here.
        VkSurfaceFormatKHR choose_format(VkPhysicalDevice physical, VkSurfaceKHR surface) {
            std::uint32_t count = 0;
            vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &count, nullptr);
            std::vector<VkSurfaceFormatKHR> formats(count);
            vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &count, formats.data());

            for (const VkSurfaceFormatKHR& format : formats) {
                const bool srgb = format.format == VK_FORMAT_B8G8R8A8_SRGB ||
                                  format.format == VK_FORMAT_R8G8B8A8_SRGB;
                if (srgb && format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
                    return format;
                }
            }

            // No sRGB surface. The first entry still presents, and the color will
            // be wrong until the final write learns to convert in software.
            ENGINE_LOG_WARN("No sRGB surface format is available. Colors will be too bright.");
            return formats.empty() ? VkSurfaceFormatKHR{ VK_FORMAT_B8G8R8A8_UNORM,
                                                         VK_COLOR_SPACE_SRGB_NONLINEAR_KHR }
                                   : formats[0];
        }

        VkPresentModeKHR choose_present_mode(VkPhysicalDevice physical, VkSurfaceKHR surface,
                                             bool vsync) {
            // FIFO is the only mode the specification guarantees, and it is what
            // vsync means. Ask for mailbox only when the caller turns vsync off.
            if (vsync) {
                return VK_PRESENT_MODE_FIFO_KHR;
            }

            std::uint32_t count = 0;
            vkGetPhysicalDeviceSurfacePresentModesKHR(physical, surface, &count, nullptr);
            std::vector<VkPresentModeKHR> modes(count);
            vkGetPhysicalDeviceSurfacePresentModesKHR(physical, surface, &count, modes.data());

            const auto has = [&modes](VkPresentModeKHR mode) {
                return std::find(modes.begin(), modes.end(), mode) != modes.end();
            };

            if (has(VK_PRESENT_MODE_MAILBOX_KHR)) {
                return VK_PRESENT_MODE_MAILBOX_KHR;
            }
            if (has(VK_PRESENT_MODE_IMMEDIATE_KHR)) {
                return VK_PRESENT_MODE_IMMEDIATE_KHR;
            }
            return VK_PRESENT_MODE_FIFO_KHR;
        }

        VkExtent2D choose_extent(const VkSurfaceCapabilitiesKHR& capabilities, Extent2D wanted) {
            // A currentExtent of all ones means the surface lets us choose.
            if (capabilities.currentExtent.width != UINT32_MAX) {
                return capabilities.currentExtent;
            }

            VkExtent2D extent{};
            extent.width = std::clamp(wanted.width, capabilities.minImageExtent.width,
                                      capabilities.maxImageExtent.width);
            extent.height = std::clamp(wanted.height, capabilities.minImageExtent.height,
                                       capabilities.maxImageExtent.height);
            return extent;
        }

        Result create_image_views(Device& device) {
            device.views.resize(device.images.size(), VK_NULL_HANDLE);
            for (std::size_t i = 0; i < device.images.size(); ++i) {
                VkImageViewCreateInfo view{};
                view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
                view.image = device.images[i];
                view.viewType = VK_IMAGE_VIEW_TYPE_2D;
                view.format = device.swapchain_format;
                view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                view.subresourceRange.levelCount = 1;
                view.subresourceRange.layerCount = 1;
                ENGINE_VK_TRY(vkCreateImageView(device.device, &view, nullptr, &device.views[i]));
            }
            return Result::Success;
        }

        /// One semaphore for each image, not for each frame in flight. A frame slot
        /// can present any image, and reusing a semaphore that the presentation
        /// engine still waits on is a validation error.
        Result create_render_semaphores(Device& device) {
            device.render_finished.resize(device.images.size(), VK_NULL_HANDLE);
            for (VkSemaphore& semaphore : device.render_finished) {
                VkSemaphoreCreateInfo info{};
                info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
                ENGINE_VK_TRY(vkCreateSemaphore(device.device, &info, nullptr, &semaphore));
            }
            return Result::Success;
        }

        Result read_swapchain_images(Device& device) {
            std::uint32_t count = 0;
            ENGINE_VK_TRY(
                vkGetSwapchainImagesKHR(device.device, device.swapchain, &count, nullptr));
            device.images.resize(count);
            ENGINE_VK_TRY(vkGetSwapchainImagesKHR(device.device, device.swapchain, &count,
                                                  device.images.data()));
            return Result::Success;
        }

    } // namespace

    namespace vk {

        Result create_swapchain(Device& device, Extent2D size) {
            if (size.width == 0 || size.height == 0) {
                // Minimized. Leave the swapchain null and let begin_frame() skip.
                device.swapchain_extent = VkExtent2D{ 0, 0 };
                return Result::Success;
            }

            VkSurfaceCapabilitiesKHR capabilities{};
            ENGINE_VK_TRY(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(device.physical,
                                                                    device.surface, &capabilities));

            const VkExtent2D extent = choose_extent(capabilities, size);
            if (extent.width == 0 || extent.height == 0) {
                device.swapchain_extent = VkExtent2D{ 0, 0 };
                return Result::Success;
            }

            const VkSurfaceFormatKHR format = choose_format(device.physical, device.surface);

            // One more than the driver's minimum lets the CPU keep working while
            // the presentation engine holds an image.
            std::uint32_t image_count = capabilities.minImageCount + 1;
            if (capabilities.maxImageCount > 0 && image_count > capabilities.maxImageCount) {
                image_count = capabilities.maxImageCount;
            }

            VkSwapchainCreateInfoKHR info{};
            info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
            info.surface = device.surface;
            info.minImageCount = image_count;
            info.imageFormat = format.format;
            info.imageColorSpace = format.colorSpace;
            info.imageExtent = extent;
            info.imageArrayLayers = 1;
            // TRANSFER_SRC is what lets capture_frame() read a frame back.
            // Every driver this engine targets offers it, and a driver that
            // does not gets a swapchain it can draw to and not capture from.
            info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
            if ((capabilities.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0U) {
                info.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
            }
            info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
            info.preTransform = capabilities.currentTransform;
            info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
            info.presentMode = choose_present_mode(device.physical, device.surface, device.vsync);
            info.clipped = VK_TRUE;
            info.oldSwapchain = VK_NULL_HANDLE;

            ENGINE_VK_TRY(vkCreateSwapchainKHR(device.device, &info, nullptr, &device.swapchain));

            device.swapchain_format = format.format;
            device.swapchain_extent = extent;

            Result result = read_swapchain_images(device);
            if (!succeeded(result)) {
                return result;
            }
            result = create_image_views(device);
            if (!succeeded(result)) {
                return result;
            }
            result = create_render_semaphores(device);
            if (!succeeded(result)) {
                return result;
            }
            return create_depth_image(device);
        }

        void destroy_swapchain(Device& device) {
            destroy_depth_image(device);

            for (VkSemaphore& semaphore : device.render_finished) {
                if (semaphore != VK_NULL_HANDLE) {
                    vkDestroySemaphore(device.device, semaphore, nullptr);
                }
            }
            device.render_finished.clear();

            for (VkImageView& view : device.views) {
                if (view != VK_NULL_HANDLE) {
                    vkDestroyImageView(device.device, view, nullptr);
                }
            }
            device.views.clear();
            device.images.clear();

            if (device.swapchain != VK_NULL_HANDLE) {
                vkDestroySwapchainKHR(device.device, device.swapchain, nullptr);
                device.swapchain = VK_NULL_HANDLE;
            }
        }

        void transition_image(VkCommandBuffer buffer, VkImage image, VkImageLayout from,
                              VkImageLayout to, VkImageAspectFlags aspect) {
            VkImageMemoryBarrier2 barrier{};
            barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
            // ALL_COMMANDS is heavier than this milestone needs. M5 replaces it
            // with the stages the render graph knows about.
            barrier.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
            barrier.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
            barrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
            barrier.dstAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_MEMORY_READ_BIT;
            barrier.oldLayout = from;
            barrier.newLayout = to;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.image = image;
            barrier.subresourceRange.aspectMask = aspect;
            barrier.subresourceRange.levelCount = VK_REMAINING_MIP_LEVELS;
            barrier.subresourceRange.layerCount = VK_REMAINING_ARRAY_LAYERS;

            VkDependencyInfo dependency{};
            dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
            dependency.imageMemoryBarrierCount = 1;
            dependency.pImageMemoryBarriers = &barrier;

            vkCmdPipelineBarrier2(buffer, &dependency);
        }

    } // namespace vk

    Result device_resize(Device* device, Extent2D size) {
        if (device == nullptr) {
            return Result::ErrorInit;
        }

        vkDeviceWaitIdle(device->device);
        vk::destroy_swapchain(*device);
        return vk::create_swapchain(*device, size);
    }

} // namespace engine::gfx
