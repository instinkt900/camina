#include "gfx/vulkan/vk_internal.h"

#include "core/assert.h"
#include "core/log.h"

#include <array>
#include <cstring>
#include <vector>

namespace engine::gfx {

    namespace {

        constexpr std::uint32_t kMaxTextures = 64;
        /**
         * How many descriptor sets the pool serves.
         *
         * One for each material, so this is a ceiling on how many distinct
         * materials a scene draws. The pool is fixed size because a growing pool
         * needs the old sets kept alive while a frame reads them, and nothing
         * needs that yet. See rule 4.6.
         */
        constexpr std::uint32_t kMaxSets = 128;
        /// Four bytes for each texel, in RGBA order.
        constexpr std::size_t kBytesPerTexel = 4;

        /// What a buffer of this usage is bound as.
        [[nodiscard]] VkBufferUsageFlags to_vk_buffer_usage(BufferUsage usage) {
            switch (usage) {
            case BufferUsage::Index:
                return VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
            case BufferUsage::Uniform:
                return VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
            case BufferUsage::Vertex:
                break;
            }
            return VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
        }

        /// The Vulkan descriptor type for a described kind.
        [[nodiscard]] VkDescriptorType to_vk_descriptor_type(DescriptorKind kind) {
            switch (kind) {
            case DescriptorKind::UniformBuffer:
                return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            case DescriptorKind::StorageBuffer:
                return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            case DescriptorKind::CombinedImageSampler:
                break;
            }
            return VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        }

        VkSamplerAddressMode to_address_mode(AddressMode mode) {
            switch (mode) {
            case AddressMode::Repeat:
                return VK_SAMPLER_ADDRESS_MODE_REPEAT;
            case AddressMode::ClampToEdge:
                return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            case AddressMode::MirroredRepeat:
                return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
            case AddressMode::ClampToZeroBorder:
                return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
            }
            return VK_SAMPLER_ADDRESS_MODE_REPEAT;
        }

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

        VkFormat to_vk_format(TextureFormat format) {
            switch (format) {
            case TextureFormat::RGBA8Srgb:
                return VK_FORMAT_R8G8B8A8_SRGB;
            case TextureFormat::RGBA8Unorm:
                return VK_FORMAT_R8G8B8A8_UNORM;
            case TextureFormat::BC7Srgb:
                return VK_FORMAT_BC7_SRGB_BLOCK;
            case TextureFormat::BC7Unorm:
                return VK_FORMAT_BC7_UNORM_BLOCK;
            case TextureFormat::RGBA16F:
                return VK_FORMAT_R16G16B16A16_SFLOAT;
            }
            return VK_FORMAT_R8G8B8A8_SRGB;
        }

        const char* texture_format_name(TextureFormat format) {
            switch (format) {
            case TextureFormat::RGBA8Srgb:
                return "RGBA8Srgb";
            case TextureFormat::RGBA8Unorm:
                return "RGBA8Unorm";
            case TextureFormat::BC7Srgb:
                return "BC7Srgb";
            case TextureFormat::BC7Unorm:
                return "BC7Unorm";
            case TextureFormat::RGBA16F:
                return "RGBA16F";
            }
            return "an unknown format";
        }

        /// Whether this GPU can sample the format at all.
        bool can_sample(VkPhysicalDevice physical, VkFormat format) {
            VkFormatProperties properties{};
            vkGetPhysicalDeviceFormatProperties(physical, format, &properties);
            return (properties.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) != 0U;
        }

        /// The size of one mip level on one axis, never smaller than one texel.
        std::uint32_t mip_extent(std::uint32_t base, std::uint32_t level) {
            const std::uint32_t shifted = level >= 32U ? 0U : base >> level;
            return shifted < 1U ? 1U : shifted;
        }

        /// How many levels an extent can hold. The chain runs down to 1 by 1.
        std::uint32_t max_mip_levels(std::uint32_t width, std::uint32_t height) {
            std::uint32_t levels = 1;
            std::uint32_t size = width > height ? width : height;
            while (size > 1) {
                size /= 2;
                ++levels;
            }
            return levels;
        }

        /// How many bytes one mip level takes in the staging buffer.
        std::size_t level_bytes(TextureFormat format, std::uint32_t width,
                                std::uint32_t height) {
            if (format == TextureFormat::BC7Srgb || format == TextureFormat::BC7Unorm) {
                // A block covers 4 by 4 texels whether or not the level fills
                // it, so a 2 by 2 level still costs one whole block.
                constexpr std::uint32_t kBlockSize = 4;
                constexpr std::size_t kBytesPerBlock = 16;
                const std::size_t across = (width + kBlockSize - 1) / kBlockSize;
                const std::size_t down = (height + kBlockSize - 1) / kBlockSize;
                return across * down * kBytesPerBlock;
            }
            // Four half floats rather than four bytes. This has to agree with
            // assets::level_bytes, because the cooker sizes the payload with
            // that one and this call sizes the copy out of it.
            if (format == TextureFormat::RGBA16F) {
                constexpr std::size_t kBytesPerHalf4 = 8;
                return static_cast<std::size_t>(width) * height * kBytesPerHalf4;
            }
            return static_cast<std::size_t>(width) * height * kBytesPerTexel;
        }

        /// How many faces a cubemap holds. A texture is flat or it is a cube.
        constexpr std::uint32_t kCubeFaces = 6;

        /**
         * Refuses a description that would build an image nothing can use.
         *
         * Every one of these is a case Vulkan would either reject with a
         * message that names no caller, or accept and then read past the end of
         * the caller's buffer. Doing it here names the engine call instead.
         *
         * @param desc What the caller asked for.
         * @param out_size The bytes the levels really need, when this returns Ok.
         */
        [[nodiscard]] Result check_texture_desc(const TextureDesc& desc, std::size_t& out_size) {
            if (desc.pixels == nullptr || desc.width == 0 || desc.height == 0 ||
                desc.mip_count == 0) {
                ENGINE_LOG_ERROR(
                    "create_texture needs pixels, a size, and at least one mip level.");
                return Result::ErrorInit;
            }

            // Vulkan would take any layer count, but a cubemap is the only array
            // shape this engine has a view type for, and a count of two would
            // build an image nothing can sample.
            if (desc.face_count != 1 && desc.face_count != kCubeFaces) {
                ENGINE_LOG_ERROR("create_texture got {} faces, and a texture holds 1 or {}.",
                                 desc.face_count, kCubeFaces);
                return Result::ErrorInit;
            }
            if (desc.face_count == kCubeFaces && desc.width != desc.height) {
                ENGINE_LOG_ERROR(
                    "create_texture got a {} by {} cubemap face, and a face is square.",
                    desc.width, desc.height);
                return Result::ErrorInit;
            }

            // Vulkan allows no more levels than the extent can halve down to.
            // The size check below does not catch an oversized count, because
            // mip_extent clamps at one texel, so every extra level asks for a
            // few more bytes and a caller can hand over a buffer that matches.
            const std::uint32_t allowed = max_mip_levels(desc.width, desc.height);
            if (desc.mip_count > allowed) {
                ENGINE_LOG_ERROR("create_texture got {} levels, and {} by {} texels hold {}.",
                                 desc.mip_count, desc.width, desc.height, allowed);
                return Result::ErrorInit;
            }

            // The caller says how many bytes it holds, and this works out how
            // many the levels need. A mismatch means the two disagree about the
            // layout, and copying anyway would read past the end of the buffer.
            std::size_t size = 0;
            for (std::uint32_t level = 0; level < desc.mip_count; ++level) {
                size += level_bytes(desc.format, mip_extent(desc.width, level),
                                    mip_extent(desc.height, level));
            }
            // Every face carries the whole chain.
            size *= desc.face_count;
            if (desc.size != size) {
                ENGINE_LOG_ERROR(
                    "create_texture got {} bytes, and {} by {} texels in {} levels of {} "
                    "across {} face(s) needs {}.",
                    desc.size, desc.width, desc.height, desc.mip_count,
                    texture_format_name(desc.format), desc.face_count, size);
                return Result::ErrorInit;
            }

            out_size = size;
            return Result::Success;
        }

        void record_texture_upload(VkCommandBuffer buffer, VkImage image, VkBuffer staging,
                                   const TextureDesc& desc) {
            vk::transition_image(buffer, image, ResourceState::Undefined,
                                 ResourceState::CopyDestination, VK_IMAGE_ASPECT_COLOR_BIT);

            // One copy for each level. The levels sit end to end in the staging
            // buffer, largest first, which is the order the cooked file holds
            // them in. So the offset only has to run forward.
            //
            // bufferRowLength and bufferImageHeight stay 0, which tells the
            // driver the rows are tightly packed. A block-compressed level
            // counts in blocks there, and 0 avoids having to say so.
            //
            // A cubemap holds the whole chain of face 0, then the whole chain of
            // face 1, and so on. So the face is the outer loop and the offset
            // still only runs forward.
            std::vector<VkBufferImageCopy> regions;
            regions.reserve(static_cast<std::size_t>(desc.mip_count) * desc.face_count);

            VkDeviceSize offset = 0;
            for (std::uint32_t face = 0; face < desc.face_count; ++face) {
                for (std::uint32_t level = 0; level < desc.mip_count; ++level) {
                    const std::uint32_t width = mip_extent(desc.width, level);
                    const std::uint32_t height = mip_extent(desc.height, level);

                    VkBufferImageCopy region{};
                    region.bufferOffset = offset;
                    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                    region.imageSubresource.mipLevel = level;
                    region.imageSubresource.baseArrayLayer = face;
                    region.imageSubresource.layerCount = 1;
                    region.imageExtent = VkExtent3D{ width, height, 1 };
                    regions.push_back(region);

                    offset += level_bytes(desc.format, width, height);
                }
            }

            vkCmdCopyBufferToImage(buffer, staging, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                   static_cast<std::uint32_t>(regions.size()), regions.data());

            vk::transition_image(buffer, image, ResourceState::CopyDestination,
                                 ResourceState::ShaderRead, VK_IMAGE_ASPECT_COLOR_BIT);
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

        DescriptorSetEntry* resolve_descriptor_set(Device& device, DescriptorSetHandle handle) {
            if (!handle.valid() || handle.index() >= device.descriptor_sets.size()) {
                return nullptr;
            }
            DescriptorSetEntry& entry = device.descriptor_sets[handle.index()];
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
            // The pool serves whole sets now, and a set holds several textures
            // and a block of factors. A material set is the case it exists for.
            // There is no shared layout any more, because every set matches a
            // layout the reflected shader described.
            const std::array<VkDescriptorPoolSize, 2> sizes{ {
                { .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                  .descriptorCount = kMaxTextures },
                { .type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .descriptorCount = kMaxSets },
            } };

            VkDescriptorPoolCreateInfo pool{};
            pool.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
            pool.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
            pool.maxSets = kMaxSets;
            pool.poolSizeCount = static_cast<std::uint32_t>(sizes.size());
            pool.pPoolSizes = sizes.data();
            ENGINE_VK_TRY(
                vkCreateDescriptorPool(device.device, &pool, nullptr, &device.descriptor_pool));

            VkCommandPoolCreateInfo upload{};
            upload.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
            upload.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
            upload.queueFamilyIndex = device.graphics_family;
            ENGINE_VK_TRY(vkCreateCommandPool(device.device, &upload, nullptr, &device.upload_pool));

            // The sampler cache fills on demand, so create_shared_resources()
            // builds nothing here. destroy_shared_resources() still clears it.
            device.depth_format = choose_depth_format(device.physical);
            if (device.depth_format == VK_FORMAT_UNDEFINED) {
                ENGINE_LOG_CRITICAL("No depth attachment format is available.");
                return Result::ErrorNoDevice;
            }
            return Result::Success;
        }

        void destroy_shared_resources(Device& device) {
            destroy_samplers(device);
            if (device.upload_pool != VK_NULL_HANDLE) {
                vkDestroyCommandPool(device.device, device.upload_pool, nullptr);
                device.upload_pool = VK_NULL_HANDLE;
            }
            if (device.descriptor_pool != VK_NULL_HANDLE) {
                // Freeing the pool frees every set it served, so the slots only
                // need forgetting rather than releasing one at a time.
                vkDestroyDescriptorPool(device.device, device.descriptor_pool, nullptr);
                device.descriptor_pool = VK_NULL_HANDLE;
                device.descriptor_sets.clear();
                device.free_descriptor_sets.clear();
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
                // The sampler belongs to the cache, not to the texture.
                // destroy_samplers() releases it.
                for (VkImageView layer : entry.layer_views) {
                    if (layer != VK_NULL_HANDLE) {
                        vkDestroyImageView(device.device, layer, nullptr);
                    }
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

        Result resolve_sampler(Device& device, const SamplerDesc& desc, VkSampler* out_sampler) {
            *out_sampler = VK_NULL_HANDLE;

            for (const SamplerEntry& entry : device.samplers) {
                // Every field is part of the key. Leaving one out would hand a
                // caller a sampler built for a different request, and a shadow
                // sampler that came back without its comparison would read a raw
                // depth as a coverage value and shadow nothing.
                if (entry.desc.filter == desc.filter && entry.desc.address == desc.address &&
                    entry.desc.compare == desc.compare) {
                    *out_sampler = entry.sampler;
                    return Result::Success;
                }
            }

            const VkFilter filter =
                desc.filter == Filter::Nearest ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
            const VkSamplerAddressMode address = to_address_mode(desc.address);

            VkSamplerCreateInfo info{};
            info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
            info.magFilter = filter;
            info.minFilter = filter;
            info.mipmapMode = desc.filter == Filter::Nearest ? VK_SAMPLER_MIPMAP_MODE_NEAREST
                                                             : VK_SAMPLER_MIPMAP_MODE_LINEAR;
            info.addressModeU = address;
            info.addressModeV = address;
            info.addressModeW = address;
            info.maxLod = VK_LOD_CLAMP_NONE;
            // Opaque black is depth zero, which is the far plane under reverse-Z.
            // See AddressMode::ClampToZeroBorder.
            info.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
            if (desc.compare) {
                info.compareEnable = VK_TRUE;
                // Reverse-Z: a surface is lit when its own depth is at or in
                // front of the depth the light recorded.
                info.compareOp = VK_COMPARE_OP_GREATER_OR_EQUAL;
            }

            SamplerEntry built;
            built.desc = desc;
            ENGINE_VK_TRY(vkCreateSampler(device.device, &info, nullptr, &built.sampler));

            device.samplers.push_back(built);
            ENGINE_LOG_DEBUG("Sampler cache: built entry {} for filter {} and address mode {}.",
                             device.samplers.size(), static_cast<std::uint32_t>(desc.filter),
                             static_cast<std::uint32_t>(desc.address));
            *out_sampler = built.sampler;
            return Result::Success;
        }

        void destroy_samplers(Device& device) {
            for (const SamplerEntry& entry : device.samplers) {
                if (entry.sampler != VK_NULL_HANDLE) {
                    vkDestroySampler(device.device, entry.sampler, nullptr);
                }
            }
            device.samplers.clear();
        }

        VkFormat color_target_format(const Device& device, ColorTargetFormat format) {
            switch (format) {
            case ColorTargetFormat::Swapchain:
                return device.swapchain_format;
            case ColorTargetFormat::RGBA16F:
                return VK_FORMAT_R16G16B16A16_SFLOAT;
            }
            return device.swapchain_format;
        }

    } // namespace vk

    namespace {

        /**
         * Builds a host-visible buffer that stays mapped, for update_buffer().
         *
         * @c desc.data may be null, which leaves the contents undefined until the
         * first update. A caller that binds it before writing gets whatever the
         * allocator handed back, so update it first.
         */
        Result create_mapped_buffer(Device& device, const BufferDesc& desc,
                                    BufferHandle* out_buffer) {
            VkBufferCreateInfo info{};
            info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
            info.size = desc.size;
            info.usage = to_vk_buffer_usage(desc.usage);

            VmaAllocationCreateInfo allocation{};
            allocation.usage = VMA_MEMORY_USAGE_AUTO;
            allocation.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                               VMA_ALLOCATION_CREATE_MAPPED_BIT;

            VkBuffer buffer = VK_NULL_HANDLE;
            VmaAllocation buffer_allocation = VK_NULL_HANDLE;
            VmaAllocationInfo mapped{};
            const VkResult created = vmaCreateBuffer(device.allocator, &info, &allocation, &buffer,
                                                     &buffer_allocation, &mapped);
            if (created != VK_SUCCESS) {
                ENGINE_LOG_ERROR("A uniform buffer could not be allocated: {}",
                                 vk::vk_result_name(created));
                return vk::to_result(created);
            }

            if (desc.data != nullptr) {
                std::memcpy(mapped.pMappedData, desc.data, desc.size);
            }

            std::uint32_t index = 0;
            if (!device.free_buffers.empty()) {
                index = device.free_buffers.back();
                device.free_buffers.pop_back();
            } else {
                index = static_cast<std::uint32_t>(device.buffers.size());
                device.buffers.emplace_back();
            }

            BufferEntry& entry = device.buffers[index];
            entry.buffer = buffer;
            entry.allocation = buffer_allocation;
            entry.mapped = mapped.pMappedData;
            entry.size = desc.size;
            entry.alive = true;
            *out_buffer = BufferHandle::make(index, entry.generation);
            return Result::Success;
        }

    } // namespace

    Result create_buffer(Device* device, const BufferDesc& desc, BufferHandle* out_buffer) {
        ENGINE_CHECK(device != nullptr, "create_buffer needs a device.");
        ENGINE_CHECK(out_buffer != nullptr, "create_buffer needs somewhere to put the handle.");
        *out_buffer = BufferHandle{};

        if (desc.size == 0) {
            ENGINE_LOG_ERROR("create_buffer needs a size.");
            return Result::ErrorInit;
        }
        if (desc.data == nullptr && desc.usage != BufferUsage::Uniform) {
            ENGINE_LOG_ERROR("create_buffer needs data for a vertex or an index buffer.");
            return Result::ErrorInit;
        }

        // A uniform buffer is small and it is written again every time something
        // it holds changes, so it lives in host-visible memory and stays mapped.
        // Staging it into device-local memory would cost a copy and a queue wait
        // for the sake of a few dozen bytes.
        if (desc.usage == BufferUsage::Uniform) {
            return create_mapped_buffer(*device, desc, out_buffer);
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
        info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | to_vk_buffer_usage(desc.usage);

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

        // VMA unmaps a mapped allocation as part of destroying it, so the
        // pointer only needs forgetting.
        vmaDestroyBuffer(device->allocator, entry->buffer, entry->allocation);
        entry->buffer = VK_NULL_HANDLE;
        entry->allocation = VK_NULL_HANDLE;
        entry->mapped = nullptr;
        entry->size = 0;
        entry->alive = false;
        ++entry->generation;
        device->free_buffers.push_back(buffer.index());
    }

    Result create_texture(Device* device, const TextureDesc& desc, TextureHandle* out_texture) {
        ENGINE_CHECK(device != nullptr, "create_texture needs a device.");
        ENGINE_CHECK(out_texture != nullptr, "create_texture needs somewhere to put the handle.");
        *out_texture = TextureHandle{};

        std::size_t size = 0;
        const Result checked = check_texture_desc(desc, size);
        if (!succeeded(checked)) {
            return checked;
        }

        const VkFormat format = to_vk_format(desc.format);
        if (!can_sample(device->physical, format)) {
            ENGINE_LOG_ERROR("This GPU cannot sample {}. Cook the texture without compression.",
                             texture_format_name(desc.format));
            return Result::ErrorInit;
        }

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
        // An sRGB format makes the sampler convert to linear on read, which is
        // what DESIGN.md section 3 asks for. The cooker decided which one this
        // is, and recorded it in the file.
        image.format = format;
        image.extent = VkExtent3D{ desc.width, desc.height, 1 };
        image.mipLevels = desc.mip_count;
        image.arrayLayers = desc.face_count;
        if (desc.face_count == kCubeFaces) {
            // Without this flag the image is a plain array and no cube view can
            // be made from it. It has to be set at creation, not at view time.
            image.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
        }
        image.samples = VK_SAMPLE_COUNT_1_BIT;
        image.tiling = VK_IMAGE_TILING_OPTIMAL;
        image.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        built.width = desc.width;
        built.height = desc.height;

        VmaAllocationCreateInfo allocation{};
        allocation.usage = VMA_MEMORY_USAGE_AUTO;

        result = vk::to_result(vmaCreateImage(device->allocator, &image, &allocation, &built.image,
                                              &built.allocation, nullptr));

        if (succeeded(result)) {
            VkImage target = built.image;
            result = vk::immediate_submit(*device, [&](VkCommandBuffer commands) {
                record_texture_upload(commands, target, staging, desc);
            });
        }

        vmaDestroyBuffer(device->allocator, staging, staging_allocation);

        if (succeeded(result)) {
            VkImageViewCreateInfo view{};
            view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            view.image = built.image;
            view.viewType = desc.face_count == kCubeFaces ? VK_IMAGE_VIEW_TYPE_CUBE
                                                          : VK_IMAGE_VIEW_TYPE_2D;
            view.format = format;
            view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            view.subresourceRange.levelCount = desc.mip_count;
            view.subresourceRange.layerCount = desc.face_count;
            result = vk::to_result(
                vkCreateImageView(device->device, &view, nullptr, &built.view));
        }

        if (succeeded(result)) {
            // The cache owns this. The texture only points at it, so the failure
            // path below must not destroy it.
            result = vk::resolve_sampler(*device, desc.sampler, &built.sampler);
        }

        if (!succeeded(result)) {
            // built.sampler belongs to the cache. Leave it alone. The next
            // texture with the same state reuses it.
            if (built.view != VK_NULL_HANDLE) {
                vkDestroyImageView(device->device, built.view, nullptr);
            }
            if (built.image != VK_NULL_HANDLE) {
                vmaDestroyImage(device->allocator, built.image, built.allocation);
            }
            ENGINE_LOG_ERROR("The texture upload failed: {}", result_name(result));
            return result;
        }

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

    Result create_depth_target(Device* device, const DepthTargetDesc& desc,
                               TextureHandle* out_texture) {
        ENGINE_CHECK(device != nullptr, "create_depth_target needs a device.");
        ENGINE_CHECK(out_texture != nullptr,
                     "create_depth_target needs somewhere to put the handle.");
        *out_texture = TextureHandle{};

        if (desc.width == 0 || desc.height == 0 || desc.layer_count == 0) {
            ENGINE_LOG_ERROR("A depth target needs a size, and this one is {}x{} with {} layers.",
                             desc.width, desc.height, desc.layer_count);
            return Result::ErrorInit;
        }

        // The frame already chose a depth format this GPU can attach. Sampling
        // it is the new requirement, so check that separately rather than assume
        // the two features arrive together.
        VkFormatProperties properties{};
        vkGetPhysicalDeviceFormatProperties(device->physical, device->depth_format, &properties);
        if ((properties.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) == 0U) {
            ENGINE_LOG_ERROR("This GPU cannot sample its own depth format, so a shadow map "
                             "cannot be read.");
            return Result::ErrorInit;
        }

        // A linear filter on a depth format is a separate feature from sampling
        // one. With a comparison sampler Vulkan still allows the read without
        // it, and then the filtered result is implementation-dependent rather
        // than the four-tap average the caller asked for.
        //
        // So drop to a nearest filter instead of failing. A hard nearest shadow
        // is a worse picture and a correct one, and refusing to start would be a
        // worse answer than that on a GPU that can draw the rest of the scene.
        SamplerDesc sampler = desc.sampler;
        if (sampler.filter == Filter::Linear &&
            (properties.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) ==
                0U) {
            ENGINE_LOG_WARN("This GPU cannot filter its depth format, so the shadow map reads "
                            "with a nearest filter and its edges will be hard.");
            sampler.filter = Filter::Nearest;
        }

        TextureEntry built;

        VkImageCreateInfo image{};
        image.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        image.imageType = VK_IMAGE_TYPE_2D;
        image.format = device->depth_format;
        image.extent = VkExtent3D{ desc.width, desc.height, 1 };
        image.mipLevels = 1;
        image.arrayLayers = desc.layer_count;
        image.samples = VK_SAMPLE_COUNT_1_BIT;
        image.tiling = VK_IMAGE_TILING_OPTIMAL;
        // Both, because one pass renders into it and another reads it. That
        // pair is the whole reason this function exists.
        image.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        built.width = desc.width;
        built.height = desc.height;

        VmaAllocationCreateInfo allocation{};
        allocation.usage = VMA_MEMORY_USAGE_AUTO;
        allocation.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
        ENGINE_VK_TRY(vmaCreateImage(device->allocator, &image, &allocation, &built.image,
                                     &built.allocation, nullptr));

        // The view the shader samples covers every layer at once. An image with
        // one layer stays a plain 2D view, because a shader that declares
        // sampler2D cannot bind an array view even of length one.
        const bool layered = desc.layer_count > 1;
        VkImageViewCreateInfo view{};
        view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view.image = built.image;
        view.viewType = layered ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
        view.format = device->depth_format;
        view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        view.subresourceRange.levelCount = 1;
        view.subresourceRange.layerCount = desc.layer_count;
        if (vkCreateImageView(device->device, &view, nullptr, &built.view) != VK_SUCCESS) {
            vmaDestroyImage(device->allocator, built.image, built.allocation);
            ENGINE_LOG_ERROR("The depth target view did not build.");
            return Result::ErrorInit;
        }

        // One view for each layer, for rendering. An attachment is a single
        // layer even when the image holds several, so the sampling view above
        // cannot serve as one.
        const auto release = [&built, device]() {
            for (VkImageView each : built.layer_views) {
                vkDestroyImageView(device->device, each, nullptr);
            }
            vkDestroyImageView(device->device, built.view, nullptr);
            vmaDestroyImage(device->allocator, built.image, built.allocation);
        };

        built.layer_views.resize(desc.layer_count, VK_NULL_HANDLE);
        for (std::uint32_t layer = 0; layer < desc.layer_count; ++layer) {
            VkImageViewCreateInfo one{};
            one.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            one.image = built.image;
            one.viewType = VK_IMAGE_VIEW_TYPE_2D;
            one.format = device->depth_format;
            one.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
            one.subresourceRange.levelCount = 1;
            one.subresourceRange.baseArrayLayer = layer;
            one.subresourceRange.layerCount = 1;
            if (vkCreateImageView(device->device, &one, nullptr, &built.layer_views[layer]) !=
                VK_SUCCESS) {
                release();
                ENGINE_LOG_ERROR("A depth target layer view did not build.");
                return Result::ErrorInit;
            }
        }

        const Result sampled = vk::resolve_sampler(*device, sampler, &built.sampler);
        if (!succeeded(sampled)) {
            release();
            return sampled;
        }

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
        ENGINE_LOG_DEBUG("Created a {}x{} depth target.", desc.width, desc.height);
        return Result::Success;
    }

    Result create_color_target(Device* device, const ColorTargetDesc& desc,
                               TextureHandle* out_texture) {
        ENGINE_CHECK(device != nullptr, "create_color_target needs a device.");
        ENGINE_CHECK(out_texture != nullptr,
                     "create_color_target needs somewhere to put the handle.");
        *out_texture = TextureHandle{};

        if (desc.width == 0 || desc.height == 0) {
            ENGINE_LOG_ERROR("A color target needs a size, and this one is {}x{}.", desc.width,
                             desc.height);
            return Result::ErrorInit;
        }

        const VkFormat format = vk::color_target_format(*device, desc.format);

        // Attaching and sampling are separate features. A GPU that can render
        // into half float and not sample it would give a black picture with no
        // message, so ask for both by name.
        constexpr VkFormatFeatureFlags kNeeded =
            VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
        VkFormatProperties properties{};
        vkGetPhysicalDeviceFormatProperties(device->physical, format, &properties);
        if ((properties.optimalTilingFeatures & kNeeded) != kNeeded) {
            ENGINE_LOG_ERROR("This GPU cannot both render into and sample the color target "
                             "format, so a pass cannot hand its result to the next one.");
            return Result::ErrorInit;
        }

        TextureEntry built;

        VkImageCreateInfo image{};
        image.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        image.imageType = VK_IMAGE_TYPE_2D;
        image.format = format;
        image.extent = VkExtent3D{ desc.width, desc.height, 1 };
        image.mipLevels = 1;
        image.arrayLayers = 1;
        image.samples = VK_SAMPLE_COUNT_1_BIT;
        image.tiling = VK_IMAGE_TILING_OPTIMAL;
        // Both, for the same reason create_depth_target() asks for both: one
        // pass renders into it and another reads it.
        image.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        built.width = desc.width;
        built.height = desc.height;

        VmaAllocationCreateInfo allocation{};
        allocation.usage = VMA_MEMORY_USAGE_AUTO;
        allocation.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
        ENGINE_VK_TRY(vmaCreateImage(device->allocator, &image, &allocation, &built.image,
                                     &built.allocation, nullptr));

        VkImageViewCreateInfo view{};
        view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view.image = built.image;
        view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view.format = format;
        view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        view.subresourceRange.levelCount = 1;
        view.subresourceRange.layerCount = 1;
        if (vkCreateImageView(device->device, &view, nullptr, &built.view) != VK_SUCCESS) {
            vmaDestroyImage(device->allocator, built.image, built.allocation);
            ENGINE_LOG_ERROR("The color target view did not build.");
            return Result::ErrorInit;
        }

        // One layer, so the sampling view serves as the attachment as well and
        // no layer view is needed. create_depth_target() builds those only
        // because a cascade set has several layers.
        const Result sampled = vk::resolve_sampler(*device, desc.sampler, &built.sampler);
        if (!succeeded(sampled)) {
            vkDestroyImageView(device->device, built.view, nullptr);
            vmaDestroyImage(device->allocator, built.image, built.allocation);
            return sampled;
        }

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
        ENGINE_LOG_DEBUG("Created a {}x{} color target.", desc.width, desc.height);
        return Result::Success;
    }

    void update_buffer(Device* device, BufferHandle buffer, const void* data, std::size_t size) {
        if (device == nullptr || data == nullptr || size == 0) {
            return;
        }
        BufferEntry* entry = vk::resolve_buffer(*device, buffer);
        if (entry == nullptr) {
            ENGINE_LOG_ERROR("update_buffer received a stale or null handle.");
            return;
        }
        if (entry->mapped == nullptr) {
            ENGINE_LOG_ERROR("update_buffer works only on a uniform buffer. A vertex or an "
                             "index buffer lives in memory the host cannot reach.");
            return;
        }
        if (size > entry->size) {
            ENGINE_LOG_ERROR("update_buffer was given {} bytes and the buffer holds {}.", size,
                             entry->size);
            return;
        }
        std::memcpy(entry->mapped, data, size);
    }

    Result create_descriptor_set(Device* device, PipelineHandle pipeline, std::uint32_t set_index,
                                 const DescriptorWrite* writes, std::size_t write_count,
                                 DescriptorSetHandle* out_set) {
        ENGINE_CHECK(device != nullptr, "create_descriptor_set needs a device.");
        ENGINE_CHECK(out_set != nullptr, "create_descriptor_set needs somewhere to put the handle.");
        *out_set = DescriptorSetHandle{};

        const PipelineEntry* owner = vk::resolve_pipeline(*device, pipeline);
        if (owner == nullptr) {
            ENGINE_LOG_ERROR("create_descriptor_set received a stale or null pipeline.");
            return Result::ErrorInit;
        }
        if (set_index >= owner->set_layouts.size()) {
            ENGINE_LOG_ERROR("The pipeline has {} descriptor sets and set {} was asked for.",
                             owner->set_layouts.size(), set_index);
            return Result::ErrorInit;
        }

        VkDescriptorSetAllocateInfo allocate{};
        allocate.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocate.descriptorPool = device->descriptor_pool;
        allocate.descriptorSetCount = 1;
        allocate.pSetLayouts = &owner->set_layouts[set_index];

        VkDescriptorSet set = VK_NULL_HANDLE;
        const VkResult allocated = vkAllocateDescriptorSets(device->device, &allocate, &set);
        if (allocated != VK_SUCCESS) {
            ENGINE_LOG_ERROR("A descriptor set could not be allocated: {}. The pool serves a "
                             "fixed number, so a scene with many materials runs out.",
                             vk::vk_result_name(allocated));
            return vk::to_result(allocated);
        }

        // The infos must outlive the update call, so both vectors are built in
        // full before anything is written.
        std::vector<VkDescriptorImageInfo> images;
        std::vector<VkDescriptorBufferInfo> buffers;
        images.reserve(write_count);
        buffers.reserve(write_count);
        std::vector<VkWriteDescriptorSet> updates;
        updates.reserve(write_count);

        for (std::size_t i = 0; i < write_count; ++i) {
            // Rule 4.2 passes a pointer and a count, so index it directly.
            const DescriptorWrite& source = writes[i];
            VkWriteDescriptorSet update{};
            update.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            update.dstSet = set;
            update.dstBinding = source.binding;
            update.descriptorCount = 1;
            update.descriptorType = to_vk_descriptor_type(source.kind);

            if (source.kind == DescriptorKind::CombinedImageSampler) {
                const TextureEntry* entry = vk::resolve_texture(*device, source.texture);
                if (entry == nullptr) {
                    ENGINE_LOG_ERROR("Binding {} names a stale or null texture.", source.binding);
                    vkFreeDescriptorSets(device->device, device->descriptor_pool, 1, &set);
                    return Result::ErrorInit;
                }
                images.push_back(VkDescriptorImageInfo{
                    .sampler = entry->sampler,
                    .imageView = entry->view,
                    .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL });
                update.pImageInfo = &images.back();
            } else {
                const BufferEntry* entry = vk::resolve_buffer(*device, source.buffer);
                if (entry == nullptr) {
                    ENGINE_LOG_ERROR("Binding {} names a stale or null buffer.", source.binding);
                    vkFreeDescriptorSets(device->device, device->descriptor_pool, 1, &set);
                    return Result::ErrorInit;
                }
                buffers.push_back(VkDescriptorBufferInfo{
                    .buffer = entry->buffer, .offset = 0, .range = VK_WHOLE_SIZE });
                update.pBufferInfo = &buffers.back();
            }
            updates.push_back(update);
        }

        if (!updates.empty()) {
            vkUpdateDescriptorSets(device->device, static_cast<std::uint32_t>(updates.size()),
                                   updates.data(), 0, nullptr);
        }

        std::uint32_t index = 0;
        if (!device->free_descriptor_sets.empty()) {
            index = device->free_descriptor_sets.back();
            device->free_descriptor_sets.pop_back();
        } else {
            index = static_cast<std::uint32_t>(device->descriptor_sets.size());
            device->descriptor_sets.emplace_back();
        }

        DescriptorSetEntry& entry = device->descriptor_sets[index];
        entry.set = set;
        entry.alive = true;
        *out_set = DescriptorSetHandle::make(index, entry.generation);
        return Result::Success;
    }

    void destroy_descriptor_set(Device* device, DescriptorSetHandle set) {
        if (device == nullptr) {
            return;
        }
        DescriptorSetEntry* entry = vk::resolve_descriptor_set(*device, set);
        if (entry == nullptr) {
            return;
        }

        vkFreeDescriptorSets(device->device, device->descriptor_pool, 1, &entry->set);
        entry->set = VK_NULL_HANDLE;
        entry->alive = false;

        // Bumping the generation makes every existing handle to this slot stale.
        ++entry->generation;
        device->free_descriptor_sets.push_back(set.index());
    }

    void destroy_texture(Device* device, TextureHandle texture) {
        if (device == nullptr) {
            return;
        }
        TextureEntry* entry = vk::resolve_texture(*device, texture);
        if (entry == nullptr) {
            return;
        }

        // The sampler is shared, so it stays. destroy_samplers() releases the
        // whole cache when the device goes.
        //
        // A descriptor set that names this texture is not freed here, because a
        // set belongs to whoever built it. The caller drops the set first, which
        // MaterialCache does when a material reloads.
        for (VkImageView layer : entry->layer_views) {
            if (layer != VK_NULL_HANDLE) {
                vkDestroyImageView(device->device, layer, nullptr);
            }
        }
        entry->layer_views.clear();
        vkDestroyImageView(device->device, entry->view, nullptr);
        vmaDestroyImage(device->allocator, entry->image, entry->allocation);

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
