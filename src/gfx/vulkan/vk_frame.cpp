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

            // The previous contents are never read, so UNDEFINED is the correct
            // source layout and lets the driver skip a decompress.
            vk::transition_image(frame.commands.buffer, frame.commands.target_image,
                                 VK_IMAGE_LAYOUT_UNDEFINED,
                                 VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                 VK_IMAGE_ASPECT_COLOR_BIT);

            if (device.depth_image != VK_NULL_HANDLE) {
                vk::transition_image(frame.commands.buffer, device.depth_image,
                                     VK_IMAGE_LAYOUT_UNDEFINED,
                                     VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                                     VK_IMAGE_ASPECT_DEPTH_BIT);
            }
            return Result::Success;
        }

    } // namespace

    Result begin_frame(Device* device, FrameInfo* out_frame) {
        ENGINE_CHECK(device != nullptr, "begin_frame needs a device.");
        ENGINE_CHECK(out_frame != nullptr, "begin_frame needs somewhere to put the frame.");
        ENGINE_ASSERT(!device->frame_open, "begin_frame was called twice without an end_frame.");

        if (device->swapchain == VK_NULL_HANDLE) {
            // The window is minimized, so there is nothing to draw into.
            return Result::OutOfDate;
        }

        Frame& frame = device->frames[device->frame_index];

        ENGINE_VK_TRY(vkWaitForFences(device->device, 1, &frame.in_flight, VK_TRUE, UINT64_MAX));

        const VkResult acquired =
            vkAcquireNextImageKHR(device->device, device->swapchain, UINT64_MAX,
                                  frame.image_available, VK_NULL_HANDLE, &device->image_index);
        if (acquired == VK_ERROR_OUT_OF_DATE_KHR) {
            // Leave the fence signaled. Nothing was submitted, so the slot is free.
            return Result::OutOfDate;
        }
        if (acquired != VK_SUCCESS && acquired != VK_SUBOPTIMAL_KHR) {
            ENGINE_LOG_ERROR("vkAcquireNextImageKHR failed with {}",
                             vk::vk_result_name(acquired));
            return vk::to_result(acquired);
        }

        ENGINE_VK_TRY(vkResetFences(device->device, 1, &frame.in_flight));

        const Result opened = open_recording(*device, frame);
        if (!succeeded(opened)) {
            return opened;
        }

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

        vk::transition_image(frame.commands.buffer, frame.commands.target_image,
                             VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                             VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_ASPECT_COLOR_BIT);

        ENGINE_VK_TRY(vkEndCommandBuffer(frame.commands.buffer));

        VkSemaphoreSubmitInfo wait{};
        wait.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
        wait.semaphore = frame.image_available;
        wait.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;

        VkSemaphoreSubmitInfo signal{};
        signal.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
        signal.semaphore = device->render_finished[device->image_index];
        signal.stageMask = VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT;

        VkCommandBufferSubmitInfo commands{};
        commands.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
        commands.commandBuffer = frame.commands.buffer;

        VkSubmitInfo2 submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
        submit.waitSemaphoreInfoCount = 1;
        submit.pWaitSemaphoreInfos = &wait;
        submit.commandBufferInfoCount = 1;
        submit.pCommandBufferInfos = &commands;
        submit.signalSemaphoreInfoCount = 1;
        submit.pSignalSemaphoreInfos = &signal;

        ENGINE_VK_TRY(vkQueueSubmit2(device->graphics_queue, 1, &submit, frame.in_flight));

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

        VkRenderingInfo info{};
        info.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
        info.renderArea.extent = VkExtent2D{ commands->extent.width, commands->extent.height };
        info.layerCount = 1;
        info.colorAttachmentCount = 1;
        info.pColorAttachments = &color;
        if (commands->owner->depth_view != VK_NULL_HANDLE) {
            info.pDepthAttachment = &depth;
        }

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

    void cmd_end_rendering(CommandList* commands) {
        ENGINE_CHECK(commands != nullptr, "cmd_end_rendering needs a command list.");
        vkCmdEndRendering(commands->buffer);
    }

} // namespace engine::gfx
