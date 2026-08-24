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

        // This slot's last frame has finished, which is what makes anything
        // retired kFramesInFlight frames ago unreferenced. The counter moves
        // first, so a resource retired during this frame is compared against
        // this frame's number and not the last one's.
        ++device->frame_counter;
        vk::release_retired(*device);

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

    namespace {

        /// Makes the readback buffer at least @p wanted bytes, keeping what fits.
        [[nodiscard]] bool grow_capture_buffer(Device& device, std::size_t wanted) {
            if (device.capture_buffer != VK_NULL_HANDLE && device.capture_size >= wanted) {
                return true;
            }
            if (device.capture_buffer != VK_NULL_HANDLE) {
                // Nothing reads it now. A capture is recorded and read inside one
                // frame, and this runs before the next one records.
                vmaDestroyBuffer(device.allocator, device.capture_buffer,
                                 device.capture_allocation);
                device.capture_buffer = VK_NULL_HANDLE;
                device.capture_allocation = VK_NULL_HANDLE;
                device.capture_mapped = nullptr;
                device.capture_size = 0;
            }

            VkBufferCreateInfo buffer_info{};
            buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
            buffer_info.size = wanted;
            buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;

            VmaAllocationCreateInfo allocation{};
            allocation.usage = VMA_MEMORY_USAGE_AUTO;
            allocation.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT |
                               VMA_ALLOCATION_CREATE_MAPPED_BIT;

            VmaAllocationInfo mapped{};
            if (vmaCreateBuffer(device.allocator, &buffer_info, &allocation,
                                &device.capture_buffer, &device.capture_allocation,
                                &mapped) != VK_SUCCESS) {
                ENGINE_LOG_ERROR("A capture of {} bytes would not allocate.", wanted);
                device.capture_buffer = VK_NULL_HANDLE;
                device.capture_allocation = VK_NULL_HANDLE;
                return false;
            }
            device.capture_mapped = mapped.pMappedData;
            device.capture_size = wanted;
            return true;
        }

        /**
         * Copies the frame's own target into the readback buffer.
         *
         * This runs while the target is still a colour attachment this side
         * owns, which is the whole point. It leaves the image in CopySource, and
         * end_frame() takes it from there.
         */
        [[nodiscard]] bool record_capture(Device& device, Frame& frame) {
            constexpr std::size_t kBytesPerPixel = 4;
            const Extent2D extent = frame.commands.extent;
            const std::size_t wanted =
                static_cast<std::size_t>(extent.width) * extent.height * kBytesPerPixel;
            if (extent.width == 0 || extent.height == 0 || !grow_capture_buffer(device, wanted)) {
                return false;
            }

            vk::transition_image(frame.commands.buffer, frame.commands.target_image,
                                 ResourceState::ColorTarget, ResourceState::CopySource,
                                 VK_IMAGE_ASPECT_COLOR_BIT);

            VkBufferImageCopy region{};
            region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            region.imageSubresource.layerCount = 1;
            region.imageExtent = VkExtent3D{ extent.width, extent.height, 1 };
            vkCmdCopyImageToBuffer(frame.commands.buffer, frame.commands.target_image,
                                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, device.capture_buffer,
                                   1, &region);

            device.capture_extent = extent;
            device.capture_frame_index = device.frame_index;
            device.capture_ready = true;
            return true;
        }

    } // namespace

    Result end_frame(Device* device) {
        ENGINE_CHECK(device != nullptr, "end_frame needs a device.");
        ENGINE_ASSERT(device->frame_open, "end_frame was called without a begin_frame.");

        Frame& frame = device->frames[device->frame_index];
        device->frame_open = false;

        // A capture copies here, inside the frame, while this side still owns
        // the image. Reading it after the present cannot be made safe: the
        // presentation engine keeps using the image, and vkDeviceWaitIdle waits
        // only for queue work. See issue #124.
        //
        // The copy goes in before the transition below, so the image is still a
        // colour target and one barrier moves it to CopySource.
        const bool capturing = device->capture_requested && record_capture(*device, frame);
        device->capture_requested = false;

        // Presentation stays here rather than in the graph. The wait belongs to
        // the present semaphore below, and a caller that forgot this barrier
        // would present an image in the wrong layout. So the graph owns every
        // barrier inside the frame and this one owns the way out of it.
        //
        // Offscreen there is nothing to present to, and PRESENT_SRC is a layout
        // only a swapchain image may take. The way out is a copy instead, so the
        // image ends where capture_frame() needs it.
        //
        // A capture already moved it to CopySource, so that is where it starts.
        vk::transition_image(frame.commands.buffer, frame.commands.target_image,
                             capturing ? ResourceState::CopySource : ResourceState::ColorTarget,
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

    bool cmd_begin_depth_rendering(CommandList* commands, TextureHandle depth_target,
                                   std::uint32_t layer) {
        ENGINE_CHECK(commands != nullptr, "cmd_begin_depth_rendering needs a command list.");
        Device& device = *commands->owner;

        const TextureEntry* entry = vk::resolve_texture(device, depth_target);
        if (entry == nullptr) {
            ENGINE_LOG_ERROR("cmd_begin_depth_rendering received a stale or null handle.");
            return false;
        }
        if (layer >= entry->layer_views.size()) {
            ENGINE_LOG_ERROR("cmd_begin_depth_rendering asked for layer {} of an image with {}.",
                             layer, entry->layer_views.size());
            return false;
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
        return true;
    }

    void cmd_begin_rendering(CommandList* commands, const ColorRGBA& clear_color,
                             bool attach_depth) {
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
        if (attach_depth) {
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
        }

        VkRenderingInfo info{};
        info.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
        info.renderArea.extent = VkExtent2D{ commands->extent.width, commands->extent.height };
        info.layerCount = 1;
        info.colorAttachmentCount = 1;
        info.pColorAttachments = &color;
        info.pDepthAttachment = attach_depth ? &depth : nullptr;

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

    bool cmd_begin_color_rendering(CommandList* commands, TextureHandle color_target,
                                   const ColorRGBA& clear_color, bool attach_depth) {
        ENGINE_CHECK(commands != nullptr, "cmd_begin_color_rendering needs a command list.");
        Device& device = *commands->owner;

        const TextureEntry* entry = vk::resolve_texture(device, color_target);
        if (entry == nullptr) {
            ENGINE_LOG_ERROR("cmd_begin_color_rendering received a stale or null handle.");
            return false;
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
        //
        // A pass that neither reads nor writes depth attaches none. Its
        // pipeline then declares no depth format, and Vulkan compares the two
        // at every draw, so attaching one anyway is an error rather than waste.
        VkRenderingAttachmentInfo depth{};
        if (attach_depth) {
            depth.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            depth.imageView = device.depth_view;
            depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
            depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            depth.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            depth.clearValue.depthStencil.depth = 0.0F;

            ENGINE_ASSERT(device.depth_view != VK_NULL_HANDLE,
                          "A frame is open but the depth attachment is missing.");
        }

        VkRenderingInfo info{};
        info.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
        // The target's own size, not the frame's. They match while the target is
        // rebuilt with the swapchain, and reading it from the image is what
        // keeps a missed rebuild from rendering into part of it.
        info.renderArea.extent = VkExtent2D{ entry->width, entry->height };
        info.layerCount = 1;
        info.colorAttachmentCount = 1;
        info.pColorAttachments = &color;
        info.pDepthAttachment = attach_depth ? &depth : nullptr;

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
        return true;
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

        // The size query answers from the swapchain, because a caller asks it to
        // size a buffer before any capture has been recorded. device_extent() in
        // the runtime uses it that way on a frame of its own.
        const std::uint32_t width = device->swapchain_extent.width;
        const std::uint32_t height = device->swapchain_extent.height;
        if (out_extent != nullptr) {
            *out_extent = Extent2D{ width, height };
        }
        if (pixels == nullptr) {
            // The caller is asking how big a buffer it needs.
            return Result::Success;
        }

        if (!device->capture_ready) {
            ENGINE_LOG_ERROR("capture_frame found no capture. Call request_capture() before "
                             "the end_frame() of the frame you want.");
            return Result::ErrorInit;
        }

        // The capture is the size the frame drew at, which a resize can leave
        // behind the swapchain. Report what was actually taken, so the caller
        // writes a PNG of the right shape rather than a sheared one.
        if (out_extent != nullptr) {
            *out_extent = device->capture_extent;
        }

        constexpr std::size_t kBytesPerPixel = 4;
        const std::size_t wanted = static_cast<std::size_t>(device->capture_extent.width) *
                                   device->capture_extent.height * kBytesPerPixel;
        if (wanted == 0 || size < wanted) {
            ENGINE_LOG_ERROR("capture_frame got {} bytes and {} by {} pixels need {}.", size,
                             device->capture_extent.width, device->capture_extent.height,
                             wanted);
            return Result::ErrorInit;
        }

        // Wait for the frame that recorded the copy, and nothing else. The copy
        // is queue work in that frame's command buffer, so its fence is exactly
        // the right thing to wait on.
        //
        // vkDeviceWaitIdle used to stand here, with a comment saying the frame
        // had been presented so nothing was reading the image. That was wrong
        // twice over. It waits for queue work, and presentation is not queue
        // work, so it said nothing about the compositor. And the copy it guarded
        // read the presented image, which the compositor still owns. See #124.
        Frame& recorded = device->frames[device->capture_frame_index];
        ENGINE_VK_TRY(
            vkWaitForFences(device->device, 1, &recorded.in_flight, VK_TRUE, UINT64_MAX));

        ENGINE_VK_TRY(vmaInvalidateAllocation(device->allocator, device->capture_allocation, 0,
                                              wanted));
        copy_as_rgba(device->capture_mapped, pixels, wanted, device->swapchain_format);
        return Result::Success;
    }

    void request_capture(Device* device) {
        ENGINE_CHECK(device != nullptr, "request_capture needs a device.");
        device->capture_requested = true;
    }

    void cmd_reset_timestamps(CommandList* commands) {
        ENGINE_CHECK(commands != nullptr, "cmd_reset_timestamps needs a command list.");
        Device& device = *commands->owner;
        if (device.timestamp_pool != VK_NULL_HANDLE && device.timestamp_count > 0) {
            vkCmdResetQueryPool(commands->buffer, device.timestamp_pool, 0,
                                device.timestamp_count);
        }
    }

    void cmd_write_timestamp(CommandList* commands, std::uint32_t index) {
        ENGINE_CHECK(commands != nullptr, "cmd_write_timestamp needs a command list.");
        Device& device = *commands->owner;

        if (device.timestamp_pool == VK_NULL_HANDLE || index >= device.timestamp_count) {
            return;
        }
        vkCmdWriteTimestamp2(commands->buffer, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT,
                             device.timestamp_pool, index);
    }

    bool read_timestamps(Device* device, std::uint32_t first, std::uint32_t count,
                         std::uint64_t* out) {
        if (device == nullptr || device->timestamp_pool == VK_NULL_HANDLE || out == nullptr) {
            return false;
        }
        const VkResult result = vkGetQueryPoolResults(
            device->device, device->timestamp_pool, first, count, count * sizeof(std::uint64_t),
            out, sizeof(std::uint64_t), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
        return result == VK_SUCCESS;
    }

    float timestamp_period(Device* device) {
        if (device == nullptr) {
            return 0.0F;
        }
        return device->properties.limits.timestampPeriod;
    }

} // namespace engine::gfx
