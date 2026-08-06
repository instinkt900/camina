#include "gfx/vulkan/vk_internal.h"

#include "core/assert.h"
#include "core/log.h"

namespace engine::gfx {

    namespace {

        /// Opens the command buffer and moves both attachments into their
        /// rendering layouts.
        Result open_recording(Device& device, Frame& frame) {
            ENGINE_VK_TRY(vkResetCommandPool(device.device, frame.pool, 0));

            VkCommandBufferBeginInfo begin{};
            begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            ENGINE_VK_TRY(vkBeginCommandBuffer(frame.commands.buffer, &begin));

            frame.commands.target_image = device.images[device.image_index];
            frame.commands.target_view = device.views[device.image_index];
            frame.commands.extent =
                Extent2D{ device.swapchain_extent.width, device.swapchain_extent.height };

            // Both images are left in ResourceState::Undefined. The render graph
            // moves them, because it is what knows which pass needs them first
            // and in what state. A caller therefore has to issue the barriers
            // the graph derived before it opens a rendering scope.
            //
            // The previous contents are never read, so Undefined is the right
            // state to start from and it lets the driver skip a decompress.
            return Result::Success;
        }

        /**
         * Copies a captured frame out as RGBA, whatever the surface handed over.
         *
         * Most surfaces are BGRA rather than RGBA, so the red and the blue
         * channel swap on the way out. Getting this backwards makes a
         * screenshot that looks almost right, which is worse than one that
         * looks obviously wrong.
         */
        void copy_as_rgba(const void* source, void* destination, std::size_t bytes,
                          VkFormat format) {
            constexpr std::size_t kBytesPerPixel = 4;
            const bool swap = format == VK_FORMAT_B8G8R8A8_SRGB ||
                              format == VK_FORMAT_B8G8R8A8_UNORM;
            const auto* from = static_cast<const std::uint8_t*>(source);
            auto* to = static_cast<std::uint8_t*>(destination);
            for (std::size_t at = 0; at < bytes; at += kBytesPerPixel) {
                to[at + 0] = swap ? from[at + 2] : from[at + 0];
                to[at + 1] = from[at + 1];
                to[at + 2] = swap ? from[at + 0] : from[at + 2];
                to[at + 3] = from[at + 3];
            }
        }

        /**
         * Points the frame at the image it will draw into.
         *
         * A windowed device asks the swapchain. An offscreen one rotates the
         * images it owns, which is the same rotation an acquire would give and
         * is the only place the two paths differ before recording starts.
         */
        [[nodiscard]] Result acquire_target(Device& device, Frame& frame) {
            if (device.headless) {
                device.image_index = device.frame_index;
                return Result::Success;
            }

            const VkResult acquired =
                vkAcquireNextImageKHR(device.device, device.swapchain, UINT64_MAX,
                                      frame.image_available, VK_NULL_HANDLE, &device.image_index);
            if (acquired == VK_ERROR_OUT_OF_DATE_KHR) {
                return Result::OutOfDate;
            }
            if (acquired != VK_SUCCESS && acquired != VK_SUBOPTIMAL_KHR) {
                ENGINE_LOG_ERROR("vkAcquireNextImageKHR failed with {}",
                                 vk::vk_result_name(acquired));
                return vk::to_result(acquired);
            }
            return Result::Success;
        }

    } // namespace

    Result begin_frame(Device* device, FrameInfo* out_frame) {
        ENGINE_CHECK(device != nullptr, "begin_frame needs a device.");
        ENGINE_CHECK(out_frame != nullptr, "begin_frame needs somewhere to put the frame.");
        ENGINE_ASSERT(!device->frame_open, "begin_frame was called twice without an end_frame.");

        if (device->images.empty() || (!device->headless && device->swapchain == VK_NULL_HANDLE)) {
            // The window is minimized, so there is nothing to draw into.
            return Result::OutOfDate;
        }

        Frame& frame = device->frames[device->frame_index];

        ENGINE_VK_TRY(vkWaitForFences(device->device, 1, &frame.in_flight, VK_TRUE, UINT64_MAX));

        const Result got = acquire_target(*device, frame);
        if (!succeeded(got)) {
            // Leave the fence signaled. Nothing was submitted, so the slot is free.
            return got;
        }

        const Result opened = open_recording(*device, frame);
        if (!succeeded(opened)) {
            // Leave the fence signaled. Nothing was submitted, so this slot must
            // stay free, or the next begin_frame waits on it forever.
            return opened;
        }

        ENGINE_VK_TRY(vkResetFences(device->device, 1, &frame.in_flight));

        device->frame_open = true;

        out_frame->commands = &frame.commands;
        out_frame->extent = frame.commands.extent;
        out_frame->frame_index = device->frame_index;
        return Result::Success;
    }

    Result end_frame(Device* device) {
        ENGINE_CHECK(device != nullptr, "end_frame needs a device.");
        ENGINE_ASSERT(device->frame_open, "end_frame was called without a begin_frame.");

        Frame& frame = device->frames[device->frame_index];
        device->frame_open = false;

        // Presentation stays here rather than in the graph. The wait belongs to
        // the present semaphore below, and a caller that forgot this barrier
        // would present an image in the wrong layout. So the graph owns every
        // barrier inside the frame and this one owns the way out of it.
        //
        // Offscreen there is nothing to present to, and PRESENT_SRC is a layout
        // only a swapchain image may take. The way out is a copy instead, so the
        // image ends where capture_frame() needs it.
        vk::transition_image(frame.commands.buffer, frame.commands.target_image,
                             ResourceState::ColorTarget,
                             device->headless ? ResourceState::CopySource : ResourceState::Present,
                             VK_IMAGE_ASPECT_COLOR_BIT);

        ENGINE_VK_TRY(vkEndCommandBuffer(frame.commands.buffer));

        VkSemaphoreSubmitInfo wait{};
        wait.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
        wait.semaphore = frame.image_available;
        wait.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;

        VkSemaphoreSubmitInfo signal{};
        signal.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
        // Offscreen there are no present semaphores at all, so this must not be
        // indexed. Only the swapchain path builds that list.
        if (!device->headless) {
            signal.semaphore = device->render_finished[device->image_index];
        }
        signal.stageMask = VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT;

        VkCommandBufferSubmitInfo commands{};
        commands.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
        commands.commandBuffer = frame.commands.buffer;

        VkSubmitInfo2 submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
        // Offscreen waits on nothing and signals nothing. Both semaphores exist
        // to order the frame against an acquire and a present, and there is
        // neither. The fence still gates the frames in flight.
        submit.waitSemaphoreInfoCount = device->headless ? 0U : 1U;
        submit.pWaitSemaphoreInfos = device->headless ? nullptr : &wait;
        submit.commandBufferInfoCount = 1;
        submit.pCommandBufferInfos = &commands;
        submit.signalSemaphoreInfoCount = device->headless ? 0U : 1U;
        submit.pSignalSemaphoreInfos = device->headless ? nullptr : &signal;

        ENGINE_VK_TRY(vkQueueSubmit2(device->graphics_queue, 1, &submit, frame.in_flight));

        if (device->headless) {
            device->frame_index = (device->frame_index + 1) % kFramesInFlight;
            return Result::Success;
        }

        VkPresentInfoKHR present{};
        present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        present.waitSemaphoreCount = 1;
        present.pWaitSemaphores = &device->render_finished[device->image_index];
        present.swapchainCount = 1;
        present.pSwapchains = &device->swapchain;
        present.pImageIndices = &device->image_index;

        const VkResult presented = vkQueuePresentKHR(device->graphics_queue, &present);

        device->frame_index = (device->frame_index + 1) % kFramesInFlight;

        if (presented == VK_ERROR_OUT_OF_DATE_KHR || presented == VK_SUBOPTIMAL_KHR) {
            return Result::OutOfDate;
        }
        if (presented != VK_SUCCESS) {
            ENGINE_LOG_ERROR("vkQueuePresentKHR failed with {}", vk::vk_result_name(presented));
            return vk::to_result(presented);
        }
        return Result::Success;
    }

    void cmd_frame_barrier(CommandList* commands, FrameTarget target, ResourceState before,
                           ResourceState after) {
        ENGINE_CHECK(commands != nullptr, "cmd_frame_barrier needs a command list.");
        Device& device = *commands->owner;

        const bool depth = target == FrameTarget::Depth;
        VkImage image = depth ? device.depth_image : commands->target_image;
        if (image == VK_NULL_HANDLE) {
            // A minimized window has no depth image. There is nothing to move
            // and nothing to report, because the frame draws nothing either.
            return;
        }

        vk::transition_image(commands->buffer, image, before, after,
                             depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT);
    }

    void cmd_texture_barrier(CommandList* commands, TextureHandle texture, ResourceState before,
                             ResourceState after) {
        ENGINE_CHECK(commands != nullptr, "cmd_texture_barrier needs a command list.");
        Device& device = *commands->owner;

        const TextureEntry* entry = vk::resolve_texture(device, texture);
        if (entry == nullptr) {
            ENGINE_LOG_ERROR("cmd_texture_barrier received a stale or null handle.");
            return;
        }

        // The aspect follows the state rather than the handle, because a handle
        // does not say which it is. A shadow map moves between depth states and
        // a scene color target moves between color ones, and no state names both
        // aspects, so the four depth states below decide it.
        const bool depth = before == ResourceState::DepthTarget ||
                           after == ResourceState::DepthTarget ||
                           before == ResourceState::DepthRead || after == ResourceState::DepthRead;
        vk::transition_image(commands->buffer, entry->image, before, after,
                             depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT);
    }

    void cmd_begin_depth_rendering(CommandList* commands, TextureHandle depth_target,
                                   std::uint32_t layer) {
        ENGINE_CHECK(commands != nullptr, "cmd_begin_depth_rendering needs a command list.");
        Device& device = *commands->owner;

        const TextureEntry* entry = vk::resolve_texture(device, depth_target);
        if (entry == nullptr) {
            ENGINE_LOG_ERROR("cmd_begin_depth_rendering received a stale or null handle.");
            return;
        }
        if (layer >= entry->layer_views.size()) {
            ENGINE_LOG_ERROR("cmd_begin_depth_rendering asked for layer {} of an image with {}.",
                             layer, entry->layer_views.size());
            return;
        }

        // Reverse-Z clears to 0, the far plane, exactly as the frame does.
        VkRenderingAttachmentInfo depth{};
        depth.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        // The view of that one layer. The sampling view covers every layer and
        // cannot be an attachment.
        depth.imageView = entry->layer_views[layer];
        depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        // Stored, not discarded. The frame's own depth is scratch, and this one
        // is the output of the pass.
        depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        depth.clearValue.depthStencil.depth = 0.0F;

        VkRenderingInfo info{};
        info.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
        info.renderArea.extent = VkExtent2D{ entry->width, entry->height };
        info.layerCount = 1;
        info.colorAttachmentCount = 0;
        info.pDepthAttachment = &depth;

        vkCmdBeginRendering(commands->buffer, &info);

        // The map is not the size of the window, and the viewport is dynamic
        // state that the frame set for the swapchain. Without this the scene
        // would render into the corner of the map that matches the window.
        VkViewport viewport{};
        viewport.width = static_cast<float>(entry->width);
        viewport.height = static_cast<float>(entry->height);
        viewport.minDepth = 0.0F;
        viewport.maxDepth = 1.0F;
        vkCmdSetViewport(commands->buffer, 0, 1, &viewport);

        VkRect2D scissor{};
        scissor.extent = VkExtent2D{ entry->width, entry->height };
        vkCmdSetScissor(commands->buffer, 0, 1, &scissor);
    }

    void cmd_begin_rendering(CommandList* commands, const ColorRGBA& clear_color) {
        ENGINE_CHECK(commands != nullptr, "cmd_begin_rendering needs a command list.");

        VkRenderingAttachmentInfo color{};
        color.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        color.imageView = commands->target_view;
        color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        color.clearValue.color.float32[0] = clear_color.r;
        color.clearValue.color.float32[1] = clear_color.g;
        color.clearValue.color.float32[2] = clear_color.b;
        color.clearValue.color.float32[3] = clear_color.a;

        // Reverse-Z clears depth to 0, which is the far plane. See DESIGN.md
        // section 3.
        VkRenderingAttachmentInfo depth{};
        depth.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        depth.imageView = commands->owner->depth_view;
        depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depth.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        depth.clearValue.depthStencil.depth = 0.0F;

        // create_swapchain() builds the depth image, and begin_frame() opens a
        // frame only when the swapchain is live. So a frame always has depth, and
        // every pipeline declares the depth format for attachment compatibility.
        ENGINE_ASSERT(commands->owner->depth_view != VK_NULL_HANDLE,
                      "A frame is open but the depth attachment is missing.");

        VkRenderingInfo info{};
        info.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
        info.renderArea.extent = VkExtent2D{ commands->extent.width, commands->extent.height };
        info.layerCount = 1;
        info.colorAttachmentCount = 1;
        info.pColorAttachments = &color;
        info.pDepthAttachment = &depth;

        vkCmdBeginRendering(commands->buffer, &info);

        // Viewport and scissor stay dynamic, so every pipeline from M1.2 onward
        // inherits the current swapchain size without a rebuild on resize.
        VkViewport viewport{};
        viewport.width = static_cast<float>(commands->extent.width);
        viewport.height = static_cast<float>(commands->extent.height);
        viewport.minDepth = 0.0F;
        // Reverse-Z keeps the viewport range at 0 to 1. The projection matrix
        // maps near to 1 and far to 0. See DESIGN.md section 3.
        viewport.maxDepth = 1.0F;
        vkCmdSetViewport(commands->buffer, 0, 1, &viewport);

        VkRect2D scissor{};
        scissor.extent = VkExtent2D{ commands->extent.width, commands->extent.height };
        vkCmdSetScissor(commands->buffer, 0, 1, &scissor);
    }

    void cmd_begin_color_rendering(CommandList* commands, TextureHandle color_target,
                                   const ColorRGBA& clear_color) {
        ENGINE_CHECK(commands != nullptr, "cmd_begin_color_rendering needs a command list.");
        Device& device = *commands->owner;

        const TextureEntry* entry = vk::resolve_texture(device, color_target);
        if (entry == nullptr) {
            ENGINE_LOG_ERROR("cmd_begin_color_rendering received a stale or null handle.");
            return;
        }

        VkRenderingAttachmentInfo color{};
        color.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        // One layer, so the view the sampler reads is the attachment as well.
        color.imageView = entry->view;
        color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        // Stored, because the next pass reads it. The frame's own color image is
        // stored for the same reason and the depth image is not.
        color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        color.clearValue.color.float32[0] = clear_color.r;
        color.clearValue.color.float32[1] = clear_color.g;
        color.clearValue.color.float32[2] = clear_color.b;
        color.clearValue.color.float32[3] = clear_color.a;

        // The frame's own depth image, not one of the caller's. A scene pass
        // needs depth and there is only ever one, so nothing would be gained by
        // asking for it here. Reverse-Z clears to 0, the far plane.
        VkRenderingAttachmentInfo depth{};
        depth.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        depth.imageView = device.depth_view;
        depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depth.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        depth.clearValue.depthStencil.depth = 0.0F;

        ENGINE_ASSERT(device.depth_view != VK_NULL_HANDLE,
                      "A frame is open but the depth attachment is missing.");

        VkRenderingInfo info{};
        info.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
        // The target's own size, not the frame's. They match while the target is
        // rebuilt with the swapchain, and reading it from the image is what
        // keeps a missed rebuild from rendering into part of it.
        info.renderArea.extent = VkExtent2D{ entry->width, entry->height };
        info.layerCount = 1;
        info.colorAttachmentCount = 1;
        info.pColorAttachments = &color;
        info.pDepthAttachment = &depth;

        vkCmdBeginRendering(commands->buffer, &info);

        VkViewport viewport{};
        viewport.width = static_cast<float>(entry->width);
        viewport.height = static_cast<float>(entry->height);
        viewport.minDepth = 0.0F;
        viewport.maxDepth = 1.0F;
        vkCmdSetViewport(commands->buffer, 0, 1, &viewport);

        VkRect2D scissor{};
        scissor.extent = VkExtent2D{ entry->width, entry->height };
        vkCmdSetScissor(commands->buffer, 0, 1, &scissor);
    }

    void cmd_end_rendering(CommandList* commands) {
        ENGINE_CHECK(commands != nullptr, "cmd_end_rendering needs a command list.");
        vkCmdEndRendering(commands->buffer);
    }

    Result capture_frame(Device* device, void* pixels, std::size_t size, Extent2D* out_extent) {
        ENGINE_CHECK(device != nullptr, "capture_frame needs a device.");
        ENGINE_ASSERT(!device->frame_open,
                      "capture_frame was called while a frame was still open.");

        if (device->images.empty() ||
            (!device->headless && device->swapchain == VK_NULL_HANDLE)) {
            return Result::ErrorInit;
        }

        const std::uint32_t width = device->swapchain_extent.width;
        const std::uint32_t height = device->swapchain_extent.height;
        if (out_extent != nullptr) {
            *out_extent = Extent2D{ width, height };
        }
        if (pixels == nullptr) {
            // The caller is asking how big a buffer it needs.
            return Result::Success;
        }

        constexpr std::size_t kBytesPerPixel = 4;
        const std::size_t wanted =
            static_cast<std::size_t>(width) * height * kBytesPerPixel;
        if (width == 0 || height == 0 || size < wanted) {
            ENGINE_LOG_ERROR("capture_frame got {} bytes and {} by {} pixels need {}.", size,
                             width, height, wanted);
            return Result::ErrorInit;
        }

        // The frame has been presented, so nothing is reading the image. Waiting
        // is the simplest correct synchronization, and a capture happens once.
        vkDeviceWaitIdle(device->device);

        VkBufferCreateInfo buffer_info{};
        buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        buffer_info.size = wanted;
        buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;

        VmaAllocationCreateInfo allocation{};
        allocation.usage = VMA_MEMORY_USAGE_AUTO;
        allocation.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT |
                           VMA_ALLOCATION_CREATE_MAPPED_BIT;

        VkBuffer readback = VK_NULL_HANDLE;
        VmaAllocation readback_allocation = VK_NULL_HANDLE;
        VmaAllocationInfo mapped{};
        ENGINE_VK_TRY(vmaCreateBuffer(device->allocator, &buffer_info, &allocation, &readback,
                                      &readback_allocation, &mapped));

        VkImage image = device->images[device->image_index];
        Result result = vk::immediate_submit(*device, [&](VkCommandBuffer commands) {
            // end_frame() left it in PRESENT_SRC, or in CopySource offscreen
            // where there is no presentation to leave it ready for.
            vk::transition_image(commands, image,
                                 device->headless ? ResourceState::CopySource
                                                  : ResourceState::Present,
                                 ResourceState::CopySource, VK_IMAGE_ASPECT_COLOR_BIT);

            VkBufferImageCopy region{};
            region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            region.imageSubresource.layerCount = 1;
            region.imageExtent = VkExtent3D{ width, height, 1 };
            vkCmdCopyImageToBuffer(commands, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                   readback, 1, &region);

            // Put it back, because the presentation engine still owns it and the
            // next acquire expects to find it as it was.
            //
            // Offscreen it stays where it is. PRESENT_SRC needs the swapchain
            // extension, which a device with no surface never enabled, and the
            // next frame starts this image from Undefined anyway.
            if (!device->headless) {
                vk::transition_image(commands, image, ResourceState::CopySource,
                                     ResourceState::Present, VK_IMAGE_ASPECT_COLOR_BIT);
            }
        });

        if (succeeded(result)) {
            copy_as_rgba(mapped.pMappedData, pixels, wanted, device->swapchain_format);
        } else {
            ENGINE_LOG_ERROR("capture_frame could not read the swapchain: {}",
                             result_name(result));
        }

        vmaDestroyBuffer(device->allocator, readback, readback_allocation);
        return result;
    }

} // namespace engine::gfx
