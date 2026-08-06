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

        Result create_offscreen_targets(Device& device, Extent2D size) {
            if (size.width == 0 || size.height == 0) {
                ENGINE_LOG_CRITICAL("An offscreen device needs a size, and this one is {}x{}.",
                                    size.width, size.height);
                return Result::ErrorInit;
            }

            // An sRGB format, because that is what converts linear to sRGB on
            // write and a different one would silently change every color. See
            // DESIGN.md section 3.
            //
            // Which sRGB format is asked rather than assumed. A surface decides
            // it for the windowed path, and there is no surface here, so this
            // takes the first that can be a color attachment and a copy source.
            // BGRA first because that is what a surface usually reports, which
            // keeps the two paths on the same format where it is possible.
            device.swapchain_format = VK_FORMAT_UNDEFINED;
            constexpr VkFormatFeatureFlags kNeeded =
                VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT;
            for (const VkFormat candidate :
                 { VK_FORMAT_B8G8R8A8_SRGB, VK_FORMAT_R8G8B8A8_SRGB }) {
                VkFormatProperties properties{};
                vkGetPhysicalDeviceFormatProperties(device.physical, candidate, &properties);
                if ((properties.optimalTilingFeatures & kNeeded) == kNeeded) {
                    device.swapchain_format = candidate;
                    break;
                }
            }
            if (device.swapchain_format == VK_FORMAT_UNDEFINED) {
                ENGINE_LOG_CRITICAL("This GPU can render into no 8-bit sRGB format, so an "
                                    "offscreen image would not carry the right colors.");
                return Result::ErrorInit;
            }

            // One clear message beats the wall of validation errors that an
            // absurd size produces, and a size comes from the command line.
            const std::uint32_t largest = device.properties.limits.maxImageDimension2D;
            if (size.width > largest || size.height > largest) {
                ENGINE_LOG_CRITICAL("This GPU renders at most {} texels on an axis, and {}x{} "
                                    "was asked for.",
                                    largest, size.width, size.height);
                return Result::ErrorInit;
            }

            device.swapchain_extent = VkExtent2D{ size.width, size.height };

            device.images.resize(kFramesInFlight, VK_NULL_HANDLE);
            device.offscreen_allocations.resize(kFramesInFlight, VK_NULL_HANDLE);
            for (std::uint32_t i = 0; i < kFramesInFlight; ++i) {
                VkImageCreateInfo info{};
                info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
                info.imageType = VK_IMAGE_TYPE_2D;
                info.format = device.swapchain_format;
                info.extent = VkExtent3D{ size.width, size.height, 1 };
                info.mipLevels = 1;
                info.arrayLayers = 1;
                info.samples = VK_SAMPLE_COUNT_1_BIT;
                info.tiling = VK_IMAGE_TILING_OPTIMAL;
                // Drawn into, then copied out by capture_frame().
                info.usage =
                    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;

                VmaAllocationCreateInfo allocation{};
                allocation.usage = VMA_MEMORY_USAGE_AUTO;
                allocation.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
                ENGINE_VK_TRY(vmaCreateImage(device.allocator, &info, &allocation,
                                             &device.images[i],
                                             &device.offscreen_allocations[i], nullptr));
            }

            const Result views = create_image_views(device);
            if (!succeeded(views)) {
                return views;
            }

            // One for each image, exactly as the swapchain path builds them.
            // Nothing waits on them offscreen, because there is no present, but
            // keeping the shape identical is what keeps the two paths one path.
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

            // A swapchain owns its images and we do not. An offscreen device
            // owns them, so it frees them here.
            for (std::size_t i = 0; i < device.offscreen_allocations.size(); ++i) {
                if (i < device.images.size() && device.images[i] != VK_NULL_HANDLE) {
                    vmaDestroyImage(device.allocator, device.images[i],
                                    device.offscreen_allocations[i]);
                }
            }
            device.offscreen_allocations.clear();
            device.images.clear();

            if (device.swapchain != VK_NULL_HANDLE) {
                vkDestroySwapchainKHR(device.device, device.swapchain, nullptr);
                device.swapchain = VK_NULL_HANDLE;
            }
        }

        StateMapping map_state(ResourceState state) {
            switch (state) {
            case ResourceState::Undefined:
                // Nothing to wait for and nothing to keep. TOP_OF_PIPE with no
                // access is the cheapest source a barrier can have, and it is
                // correct only because the contents are not worth keeping.
                return { VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, VK_ACCESS_2_NONE,
                         VK_IMAGE_LAYOUT_UNDEFINED };
            case ResourceState::ColorTarget:
                return { VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT |
                             VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT,
                         VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
            case ResourceState::DepthTarget:
                // Both test stages, because a depth write can happen in either
                // depending on whether the fragment shader discards.
                return { VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                             VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                         VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                             VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT,
                         VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL };
            case ResourceState::DepthRead:
                return { VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                             VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                         VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT,
                         VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL };
            case ResourceState::ShaderRead:
                // Every stage that can sample, because the state does not say
                // which one will. Naming the three is still far short of
                // ALL_COMMANDS, and a pass that knows better can gain a state.
                return { VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT |
                             VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
                             VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                         VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
            case ResourceState::ComputeWrite:
                return { VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                         VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT |
                             VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
                         VK_IMAGE_LAYOUT_GENERAL };
            case ResourceState::CopySource:
                return { VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                         VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL };
            case ResourceState::CopyDestination:
                return { VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL };
            case ResourceState::Present:
                break;
            }
            // Presentation is not a pipeline stage the barrier can name, and the
            // wait belongs to the present semaphore rather than here. So this
            // side carries the layout and no access at all.
            return { VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, VK_ACCESS_2_NONE,
                     VK_IMAGE_LAYOUT_PRESENT_SRC_KHR };
        }

        void transition_image(VkCommandBuffer buffer, VkImage image, ResourceState from,
                              ResourceState to, VkImageAspectFlags aspect) {
            StateMapping source = map_state(from);
            const StateMapping destination = map_state(to);

            // Undefined means the contents are not worth keeping. It does not
            // mean nothing was using the image, and those are different claims.
            //
            // Taking the first at face value gives TOP_OF_PIPE with no access,
            // which orders this barrier against nothing at all. Two real races
            // follow, and synchronization validation reports both:
            //
            // - A swapchain image is still being read by vkAcquireNextImageKHR.
            //   The acquire semaphore is waited on at COLOR_ATTACHMENT_OUTPUT,
            //   so a transition at TOP_OF_PIPE runs before the image is ours.
            //
            // - The depth image is one image shared by every frame in flight, so
            //   the frame before may still be writing it.
            //
            // Waiting on the stage the new state uses fixes both, and it is the
            // narrowest thing that can: whatever touched this image last did so
            // in the same way this pass is about to.
            if (from == ResourceState::Undefined) {
                source.stage = destination.stage;
                source.access = destination.access;
            }

            VkImageMemoryBarrier2 barrier{};
            barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
            barrier.srcStageMask = source.stage;
            barrier.srcAccessMask = source.access;
            barrier.dstStageMask = destination.stage;
            barrier.dstAccessMask = destination.access;
            barrier.oldLayout = source.layout;
            barrier.newLayout = destination.layout;
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
