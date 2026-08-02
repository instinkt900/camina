#include "gfx/vulkan/vk_internal.h"

#include "core/assert.h"
#include "core/log.h"

#include <array>
#include <cstring>

namespace engine::gfx {

    namespace {

        constexpr std::uint32_t kMaxTextures = 64;
        /// Four bytes for each texel, in RGBA order.
        constexpr std::size_t kBytesPerTexel = 4;

        /// Creates a host-visible buffer holding a copy of the source bytes.
        Result create_staging(Device& device, const void* data, std::size_t size,
                              VkBuffer* out_buffer, VmaAllocation* out_allocation) {
            VkBufferCreateInfo info{};
            info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
            info.size = size;
            info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;

            VmaAllocationCreateInfo allocation{};
            allocation.usage = VMA_MEMORY_USAGE_AUTO;
            allocation.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                               VMA_ALLOCATION_CREATE_MAPPED_BIT;

            VmaAllocationInfo mapped{};
            ENGINE_VK_TRY(vmaCreateBuffer(device.allocator, &info, &allocation, out_buffer,
                                          out_allocation, &mapped));
            std::memcpy(mapped.pMappedData, data, size);
            return Result::Success;
        }

        VkFormat choose_depth_format(VkPhysicalDevice physical) {
            // The specification guarantees that at least one of these supports a
            // depth attachment. Reverse-Z wants the float format, so ask first.
            const std::array<VkFormat, 2> candidates{ VK_FORMAT_D32_SFLOAT,
                                                      VK_FORMAT_X8_D24_UNORM_PACK32 };
            for (VkFormat format : candidates) {
                VkFormatProperties properties{};
                vkGetPhysicalDeviceFormatProperties(physical, format, &properties);
                if ((properties.optimalTilingFeatures &
                     VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0U) {
                    return format;
                }
            }
            return VK_FORMAT_UNDEFINED;
        }

        void record_texture_upload(VkCommandBuffer buffer, VkImage image, VkBuffer staging,
                                   std::uint32_t width, std::uint32_t height) {
            vk::transition_image(buffer, image, VK_IMAGE_LAYOUT_UNDEFINED,
                                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                 VK_IMAGE_ASPECT_COLOR_BIT);

            VkBufferImageCopy region{};
            region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            region.imageSubresource.layerCount = 1;
            region.imageExtent = VkExtent3D{ width, height, 1 };
            vkCmdCopyBufferToImage(buffer, staging, image,
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

            vk::transition_image(buffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                 VK_IMAGE_ASPECT_COLOR_BIT);
        }

    } // namespace

    namespace vk {

        BufferEntry* resolve_buffer(Device& device, BufferHandle handle) {
            if (!handle.valid() || handle.index() >= device.buffers.size()) {
                return nullptr;
            }
            BufferEntry& entry = device.buffers[handle.index()];
            if (!entry.alive || entry.generation != handle.generation()) {
                return nullptr;
            }
            return &entry;
        }

        TextureEntry* resolve_texture(Device& device, TextureHandle handle) {
            if (!handle.valid() || handle.index() >= device.textures.size()) {
                return nullptr;
            }
            TextureEntry& entry = device.textures[handle.index()];
            if (!entry.alive || entry.generation != handle.generation()) {
                return nullptr;
            }
            return &entry;
        }

        Result immediate_submit(Device& device,
                                const std::function<void(VkCommandBuffer)>& record) {
            VkCommandBufferAllocateInfo allocate{};
            allocate.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
            allocate.commandPool = device.upload_pool;
            allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            allocate.commandBufferCount = 1;

            VkCommandBuffer buffer = VK_NULL_HANDLE;
            ENGINE_VK_TRY(vkAllocateCommandBuffers(device.device, &allocate, &buffer));

            VkCommandBufferBeginInfo begin{};
            begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            ENGINE_VK_TRY(vkBeginCommandBuffer(buffer, &begin));
            record(buffer);
            ENGINE_VK_TRY(vkEndCommandBuffer(buffer));

            VkCommandBufferSubmitInfo commands{};
            commands.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
            commands.commandBuffer = buffer;

            VkSubmitInfo2 submit{};
            submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
            submit.commandBufferInfoCount = 1;
            submit.pCommandBufferInfos = &commands;

            ENGINE_VK_TRY(vkQueueSubmit2(device.graphics_queue, 1, &submit, VK_NULL_HANDLE));
            ENGINE_VK_TRY(vkQueueWaitIdle(device.graphics_queue));

            vkFreeCommandBuffers(device.device, device.upload_pool, 1, &buffer);
            return Result::Success;
        }

        Result create_shared_resources(Device& device) {
            VkDescriptorSetLayoutBinding binding{};
            binding.binding = 0;
            binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            binding.descriptorCount = 1;
            binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

            VkDescriptorSetLayoutCreateInfo layout{};
            layout.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
            layout.bindingCount = 1;
            layout.pBindings = &binding;
            ENGINE_VK_TRY(
                vkCreateDescriptorSetLayout(device.device, &layout, nullptr, &device.texture_layout));

            VkDescriptorPoolSize size{};
            size.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            size.descriptorCount = kMaxTextures;

            VkDescriptorPoolCreateInfo pool{};
            pool.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
            pool.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
            pool.maxSets = kMaxTextures;
            pool.poolSizeCount = 1;
            pool.pPoolSizes = &size;
            ENGINE_VK_TRY(
                vkCreateDescriptorPool(device.device, &pool, nullptr, &device.descriptor_pool));

            VkCommandPoolCreateInfo upload{};
            upload.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
            upload.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
            upload.queueFamilyIndex = device.graphics_family;
            ENGINE_VK_TRY(vkCreateCommandPool(device.device, &upload, nullptr, &device.upload_pool));

            device.depth_format = choose_depth_format(device.physical);
            if (device.depth_format == VK_FORMAT_UNDEFINED) {
                ENGINE_LOG_CRITICAL("No depth attachment format is available.");
                return Result::ErrorNoDevice;
            }
            return Result::Success;
        }

        void destroy_shared_resources(Device& device) {
            if (device.upload_pool != VK_NULL_HANDLE) {
                vkDestroyCommandPool(device.device, device.upload_pool, nullptr);
                device.upload_pool = VK_NULL_HANDLE;
            }
            if (device.descriptor_pool != VK_NULL_HANDLE) {
                vkDestroyDescriptorPool(device.device, device.descriptor_pool, nullptr);
                device.descriptor_pool = VK_NULL_HANDLE;
            }
            if (device.texture_layout != VK_NULL_HANDLE) {
                vkDestroyDescriptorSetLayout(device.device, device.texture_layout, nullptr);
                device.texture_layout = VK_NULL_HANDLE;
            }
        }

        Result create_depth_image(Device& device) {
            if (device.swapchain_extent.width == 0 || device.swapchain_extent.height == 0) {
                return Result::Success;
            }

            VkImageCreateInfo info{};
            info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
            info.imageType = VK_IMAGE_TYPE_2D;
            info.format = device.depth_format;
            info.extent = VkExtent3D{ device.swapchain_extent.width,
                                      device.swapchain_extent.height, 1 };
            info.mipLevels = 1;
            info.arrayLayers = 1;
            info.samples = VK_SAMPLE_COUNT_1_BIT;
            info.tiling = VK_IMAGE_TILING_OPTIMAL;
            info.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;

            VmaAllocationCreateInfo allocation{};
            allocation.usage = VMA_MEMORY_USAGE_AUTO;
            allocation.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;

            ENGINE_VK_TRY(vmaCreateImage(device.allocator, &info, &allocation, &device.depth_image,
                                         &device.depth_allocation, nullptr));

            VkImageViewCreateInfo view{};
            view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            view.image = device.depth_image;
            view.viewType = VK_IMAGE_VIEW_TYPE_2D;
            view.format = device.depth_format;
            view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
            view.subresourceRange.levelCount = 1;
            view.subresourceRange.layerCount = 1;
            ENGINE_VK_TRY(vkCreateImageView(device.device, &view, nullptr, &device.depth_view));

            return Result::Success;
        }

        void destroy_depth_image(Device& device) {
            if (device.depth_view != VK_NULL_HANDLE) {
                vkDestroyImageView(device.device, device.depth_view, nullptr);
                device.depth_view = VK_NULL_HANDLE;
            }
            if (device.depth_image != VK_NULL_HANDLE) {
                vmaDestroyImage(device.allocator, device.depth_image, device.depth_allocation);
                device.depth_image = VK_NULL_HANDLE;
                device.depth_allocation = VK_NULL_HANDLE;
            }
        }

        void destroy_buffers(Device& device) {
            for (BufferEntry& entry : device.buffers) {
                if (entry.buffer != VK_NULL_HANDLE) {
                    vmaDestroyBuffer(device.allocator, entry.buffer, entry.allocation);
                }
            }
            device.buffers.clear();
            device.free_buffers.clear();
        }

        void destroy_textures(Device& device) {
            for (TextureEntry& entry : device.textures) {
                if (entry.sampler != VK_NULL_HANDLE) {
                    vkDestroySampler(device.device, entry.sampler, nullptr);
                }
                if (entry.view != VK_NULL_HANDLE) {
                    vkDestroyImageView(device.device, entry.view, nullptr);
                }
                if (entry.image != VK_NULL_HANDLE) {
                    vmaDestroyImage(device.allocator, entry.image, entry.allocation);
                }
            }
            device.textures.clear();
            device.free_textures.clear();
        }

    } // namespace vk

    Result create_buffer(Device* device, const BufferDesc& desc, BufferHandle* out_buffer) {
        ENGINE_CHECK(device != nullptr, "create_buffer needs a device.");
        ENGINE_CHECK(out_buffer != nullptr, "create_buffer needs somewhere to put the handle.");
        *out_buffer = BufferHandle{};

        if (desc.data == nullptr || desc.size == 0) {
            ENGINE_LOG_ERROR("create_buffer needs data and a size.");
            return Result::ErrorInit;
        }

        VkBuffer staging = VK_NULL_HANDLE;
        VmaAllocation staging_allocation = VK_NULL_HANDLE;
        const Result staged = create_staging(*device, desc.data, desc.size, &staging,
                                             &staging_allocation);
        if (!succeeded(staged)) {
            return staged;
        }

        VkBufferCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        info.size = desc.size;
        info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                     (desc.usage == BufferUsage::Index ? VK_BUFFER_USAGE_INDEX_BUFFER_BIT
                                                       : VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);

        VmaAllocationCreateInfo allocation{};
        allocation.usage = VMA_MEMORY_USAGE_AUTO;

        VkBuffer buffer = VK_NULL_HANDLE;
        VmaAllocation buffer_allocation = VK_NULL_HANDLE;
        VkResult created = vmaCreateBuffer(device->allocator, &info, &allocation, &buffer,
                                           &buffer_allocation, nullptr);
        Result result = vk::to_result(created);

        if (succeeded(result)) {
            const std::size_t size = desc.size;
            result = vk::immediate_submit(*device, [&](VkCommandBuffer commands) {
                VkBufferCopy region{};
                region.size = size;
                vkCmdCopyBuffer(commands, staging, buffer, 1, &region);
            });
        }

        vmaDestroyBuffer(device->allocator, staging, staging_allocation);

        if (!succeeded(result)) {
            if (buffer != VK_NULL_HANDLE) {
                vmaDestroyBuffer(device->allocator, buffer, buffer_allocation);
            }
            ENGINE_LOG_ERROR("The buffer upload failed: {}", result_name(result));
            return result;
        }

        std::uint32_t index = 0;
        if (!device->free_buffers.empty()) {
            index = device->free_buffers.back();
            device->free_buffers.pop_back();
        } else {
            index = static_cast<std::uint32_t>(device->buffers.size());
            device->buffers.emplace_back();
        }

        BufferEntry& entry = device->buffers[index];
        entry.buffer = buffer;
        entry.allocation = buffer_allocation;
        entry.alive = true;
        *out_buffer = BufferHandle::make(index, entry.generation);
        return Result::Success;
    }

    void destroy_buffer(Device* device, BufferHandle buffer) {
        if (device == nullptr) {
            return;
        }
        BufferEntry* entry = vk::resolve_buffer(*device, buffer);
        if (entry == nullptr) {
            return;
        }

        vmaDestroyBuffer(device->allocator, entry->buffer, entry->allocation);
        entry->buffer = VK_NULL_HANDLE;
        entry->allocation = VK_NULL_HANDLE;
        entry->alive = false;
        ++entry->generation;
        device->free_buffers.push_back(buffer.index());
    }

    Result create_texture(Device* device, const TextureDesc& desc, TextureHandle* out_texture) {
        ENGINE_CHECK(device != nullptr, "create_texture needs a device.");
        ENGINE_CHECK(out_texture != nullptr, "create_texture needs somewhere to put the handle.");
        *out_texture = TextureHandle{};

        if (desc.pixels == nullptr || desc.width == 0 || desc.height == 0) {
            ENGINE_LOG_ERROR("create_texture needs pixels and a size.");
            return Result::ErrorInit;
        }

        const std::size_t size =
            static_cast<std::size_t>(desc.width) * desc.height * kBytesPerTexel;

        VkBuffer staging = VK_NULL_HANDLE;
        VmaAllocation staging_allocation = VK_NULL_HANDLE;
        Result result = create_staging(*device, desc.pixels, size, &staging, &staging_allocation);
        if (!succeeded(result)) {
            return result;
        }

        TextureEntry built;

        VkImageCreateInfo image{};
        image.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        image.imageType = VK_IMAGE_TYPE_2D;
        // sRGB, so the sampler converts to linear on read. See DESIGN.md section 3.
        image.format = VK_FORMAT_R8G8B8A8_SRGB;
        image.extent = VkExtent3D{ desc.width, desc.height, 1 };
        image.mipLevels = 1;
        image.arrayLayers = 1;
        image.samples = VK_SAMPLE_COUNT_1_BIT;
        image.tiling = VK_IMAGE_TILING_OPTIMAL;
        image.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;

        VmaAllocationCreateInfo allocation{};
        allocation.usage = VMA_MEMORY_USAGE_AUTO;

        result = vk::to_result(vmaCreateImage(device->allocator, &image, &allocation, &built.image,
                                              &built.allocation, nullptr));

        if (succeeded(result)) {
            const std::uint32_t width = desc.width;
            const std::uint32_t height = desc.height;
            VkImage target = built.image;
            result = vk::immediate_submit(*device, [&](VkCommandBuffer commands) {
                record_texture_upload(commands, target, staging, width, height);
            });
        }

        vmaDestroyBuffer(device->allocator, staging, staging_allocation);

        if (succeeded(result)) {
            VkImageViewCreateInfo view{};
            view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            view.image = built.image;
            view.viewType = VK_IMAGE_VIEW_TYPE_2D;
            view.format = VK_FORMAT_R8G8B8A8_SRGB;
            view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            view.subresourceRange.levelCount = 1;
            view.subresourceRange.layerCount = 1;
            result = vk::to_result(
                vkCreateImageView(device->device, &view, nullptr, &built.view));
        }

        if (succeeded(result)) {
            VkSamplerCreateInfo sampler{};
            sampler.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
            sampler.magFilter = VK_FILTER_NEAREST;
            sampler.minFilter = VK_FILTER_NEAREST;
            sampler.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
            sampler.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
            sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
            sampler.maxLod = VK_LOD_CLAMP_NONE;
            result =
                vk::to_result(vkCreateSampler(device->device, &sampler, nullptr, &built.sampler));
        }

        if (succeeded(result)) {
            VkDescriptorSetAllocateInfo allocate{};
            allocate.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            allocate.descriptorPool = device->descriptor_pool;
            allocate.descriptorSetCount = 1;
            allocate.pSetLayouts = &device->texture_layout;
            result = vk::to_result(
                vkAllocateDescriptorSets(device->device, &allocate, &built.set));
        }

        if (!succeeded(result)) {
            if (built.sampler != VK_NULL_HANDLE) {
                vkDestroySampler(device->device, built.sampler, nullptr);
            }
            if (built.view != VK_NULL_HANDLE) {
                vkDestroyImageView(device->device, built.view, nullptr);
            }
            if (built.image != VK_NULL_HANDLE) {
                vmaDestroyImage(device->allocator, built.image, built.allocation);
            }
            ENGINE_LOG_ERROR("The texture upload failed: {}", result_name(result));
            return result;
        }

        VkDescriptorImageInfo info{};
        info.sampler = built.sampler;
        info.imageView = built.view;
        info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = built.set;
        write.dstBinding = 0;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.pImageInfo = &info;
        vkUpdateDescriptorSets(device->device, 1, &write, 0, nullptr);

        std::uint32_t index = 0;
        if (!device->free_textures.empty()) {
            index = device->free_textures.back();
            device->free_textures.pop_back();
        } else {
            index = static_cast<std::uint32_t>(device->textures.size());
            device->textures.emplace_back();
        }

        TextureEntry& entry = device->textures[index];
        const std::uint32_t generation = entry.generation;
        entry = built;
        entry.generation = generation;
        entry.alive = true;
        *out_texture = TextureHandle::make(index, generation);
        return Result::Success;
    }

    void destroy_texture(Device* device, TextureHandle texture) {
        if (device == nullptr) {
            return;
        }
        TextureEntry* entry = vk::resolve_texture(*device, texture);
        if (entry == nullptr) {
            return;
        }

        vkFreeDescriptorSets(device->device, device->descriptor_pool, 1, &entry->set);
        vkDestroySampler(device->device, entry->sampler, nullptr);
        vkDestroyImageView(device->device, entry->view, nullptr);
        vmaDestroyImage(device->allocator, entry->image, entry->allocation);

        entry->set = VK_NULL_HANDLE;
        entry->sampler = VK_NULL_HANDLE;
        entry->view = VK_NULL_HANDLE;
        entry->image = VK_NULL_HANDLE;
        entry->allocation = VK_NULL_HANDLE;
        entry->alive = false;
        ++entry->generation;
        device->free_textures.push_back(texture.index());
    }

    void cmd_bind_vertex_buffer(CommandList* commands, BufferHandle buffer) {
        ENGINE_CHECK(commands != nullptr, "cmd_bind_vertex_buffer needs a command list.");
        const BufferEntry* entry = vk::resolve_buffer(*commands->owner, buffer);
        if (entry == nullptr) {
            ENGINE_LOG_ERROR("cmd_bind_vertex_buffer received a stale or null handle.");
            return;
        }
        const VkDeviceSize offset = 0;
        vkCmdBindVertexBuffers(commands->buffer, 0, 1, &entry->buffer, &offset);
    }

    void cmd_bind_index_buffer(CommandList* commands, BufferHandle buffer) {
        ENGINE_CHECK(commands != nullptr, "cmd_bind_index_buffer needs a command list.");
        const BufferEntry* entry = vk::resolve_buffer(*commands->owner, buffer);
        if (entry == nullptr) {
            ENGINE_LOG_ERROR("cmd_bind_index_buffer received a stale or null handle.");
            return;
        }
        vkCmdBindIndexBuffer(commands->buffer, entry->buffer, 0, VK_INDEX_TYPE_UINT32);
    }

    void cmd_draw_indexed(CommandList* commands, std::uint32_t index_count,
                          std::uint32_t instance_count, std::uint32_t first_index,
                          std::uint32_t first_instance) {
        ENGINE_CHECK(commands != nullptr, "cmd_draw_indexed needs a command list.");
        vkCmdDrawIndexed(commands->buffer, index_count, instance_count, first_index, 0,
                         first_instance);
    }

} // namespace engine::gfx
