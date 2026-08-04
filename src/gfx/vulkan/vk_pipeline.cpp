#include "gfx/vulkan/vk_internal.h"

#include "core/assert.h"
#include "core/log.h"

#include <array>
#include <vector>

namespace engine::gfx {

    namespace {

        Result create_module(Device& device, const ShaderCode& code, VkShaderModule* out) {
            VkShaderModuleCreateInfo info{};
            info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
            info.codeSize = code.word_count * sizeof(std::uint32_t);
            info.pCode = code.spirv;
            ENGINE_VK_TRY(vkCreateShaderModule(device.device, &info, nullptr, out));
            return Result::Success;
        }

        /// Fills the fixed-function state that every M1 pipeline shares.
        struct FixedState {
            VkPipelineVertexInputStateCreateInfo vertex_input{};
            VkPipelineInputAssemblyStateCreateInfo assembly{};
            VkPipelineViewportStateCreateInfo viewport{};
            VkPipelineRasterizationStateCreateInfo raster{};
            VkPipelineMultisampleStateCreateInfo multisample{};
            VkPipelineDepthStencilStateCreateInfo depth{};
            VkPipelineColorBlendAttachmentState blend_attachment{};
            VkPipelineColorBlendStateCreateInfo blend{};
        };

        void fill_fixed_state(FixedState& state, const GraphicsPipelineDesc& desc) {
            state.vertex_input.sType =
                VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

            state.assembly.sType =
                VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
            state.assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

            // The counts must be set even though both are dynamic.
            state.viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
            state.viewport.viewportCount = 1;
            state.viewport.scissorCount = 1;

            state.raster.sType =
                VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
            state.raster.polygonMode = VK_POLYGON_MODE_FILL;
            state.raster.cullMode = desc.cull_back ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE;
            // Vulkan clip space already puts +Y down. The projection in
            // math/conventions.h negates the Y row, which cancels that, so the
            // winding the rasterizer sees matches the winding in world space.
            // Counter-clockwise stays front facing, as glTF supplies it.
            state.raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
            state.raster.lineWidth = 1.0F;

            state.multisample.sType =
                VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
            state.multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

            // Reverse-Z puts the near plane at 1 and the far plane at 0, so a
            // nearer fragment has the greater value. See DESIGN.md section 3.
            state.depth.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
            state.depth.depthTestEnable = desc.depth_test ? VK_TRUE : VK_FALSE;
            state.depth.depthWriteEnable = desc.depth_test ? VK_TRUE : VK_FALSE;
            state.depth.depthCompareOp = VK_COMPARE_OP_GREATER;
            state.depth.maxDepthBounds = 1.0F;

            state.blend_attachment.colorWriteMask =
                VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

            state.blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
            state.blend.attachmentCount = 1;
            state.blend.pAttachments = &state.blend_attachment;
        }

        /// Owns the storage that the vertex input state points at.
        struct VertexInput {
            VkVertexInputBindingDescription binding{};
            std::vector<VkVertexInputAttributeDescription> attributes;
        };

        VkFormat to_vk_format(VertexFormat format) {
            switch (format) {
            case VertexFormat::Float2:
                return VK_FORMAT_R32G32_SFLOAT;
            case VertexFormat::Float3:
                return VK_FORMAT_R32G32B32_SFLOAT;
            case VertexFormat::Float4:
                return VK_FORMAT_R32G32B32A32_SFLOAT;
            }
            return VK_FORMAT_R32G32B32_SFLOAT;
        }

        /// Leaves the state empty when the pipeline builds its positions from
        /// the vertex index instead of reading a vertex buffer.
        void fill_vertex_input(VertexInput& input, FixedState& state,
                               const GraphicsPipelineDesc& desc) {
            if (desc.attributes == nullptr || desc.attribute_count == 0) {
                return;
            }

            input.binding.binding = 0;
            input.binding.stride = desc.vertex_stride;
            input.binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

            input.attributes.reserve(desc.attribute_count);
            for (std::size_t i = 0; i < desc.attribute_count; ++i) {
                // Rule 4.2 passes a pointer and a count, so index it directly.
                const VertexAttribute& source = desc.attributes[i];
                VkVertexInputAttributeDescription attribute{};
                attribute.location = source.location;
                attribute.binding = 0;
                attribute.offset = source.offset;
                attribute.format = to_vk_format(source.format);
                input.attributes.push_back(attribute);
            }

            state.vertex_input.vertexBindingDescriptionCount = 1;
            state.vertex_input.pVertexBindingDescriptions = &input.binding;
            state.vertex_input.vertexAttributeDescriptionCount =
                static_cast<std::uint32_t>(input.attributes.size());
            state.vertex_input.pVertexAttributeDescriptions = input.attributes.data();
        }

        /// The camera matrix travels as a push constant, and the texture is the one
        /// descriptor set. Both stay optional.
        Result create_layout(Device& device, const GraphicsPipelineDesc& desc,
                             VkPipelineLayout* out) {
            VkPushConstantRange push{};
            push.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
            push.size = desc.push_constant_size;

            VkPipelineLayoutCreateInfo info{};
            info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
            if (desc.sample_texture) {
                info.setLayoutCount = 1;
                info.pSetLayouts = &device.texture_layout;
            }
            if (desc.push_constant_size > 0) {
                info.pushConstantRangeCount = 1;
                info.pPushConstantRanges = &push;
            }

            ENGINE_VK_TRY(vkCreatePipelineLayout(device.device, &info, nullptr, out));
            return Result::Success;
        }

        /// Claims a free slot, or grows the pool. Returns the handle for the slot.
        PipelineHandle claim_slot(Device& device, VkPipeline pipeline, VkPipelineLayout layout,
                                  std::uint32_t push_constant_size) {
            std::uint32_t index = 0;
            if (!device.free_pipelines.empty()) {
                index = device.free_pipelines.back();
                device.free_pipelines.pop_back();
            } else {
                index = static_cast<std::uint32_t>(device.pipelines.size());
                device.pipelines.emplace_back();
            }

            PipelineEntry& entry = device.pipelines[index];
            entry.pipeline = pipeline;
            entry.layout = layout;
            entry.push_constant_size = push_constant_size;
            entry.alive = true;
            return PipelineHandle::make(index, entry.generation);
        }

    } // namespace

    namespace vk {

        PipelineEntry* resolve_pipeline(Device& device, PipelineHandle handle) {
            if (!handle.valid() || handle.index() >= device.pipelines.size()) {
                return nullptr;
            }
            PipelineEntry& entry = device.pipelines[handle.index()];
            if (!entry.alive || entry.generation != handle.generation()) {
                return nullptr;
            }
            return &entry;
        }

        void destroy_pipelines(Device& device) {
            for (PipelineEntry& entry : device.pipelines) {
                if (entry.pipeline != VK_NULL_HANDLE) {
                    vkDestroyPipeline(device.device, entry.pipeline, nullptr);
                }
                if (entry.layout != VK_NULL_HANDLE) {
                    vkDestroyPipelineLayout(device.device, entry.layout, nullptr);
                }
            }
            device.pipelines.clear();
            device.free_pipelines.clear();
        }

    } // namespace vk

    Result create_graphics_pipeline(Device* device, const GraphicsPipelineDesc& desc,
                                    PipelineHandle* out_pipeline) {
        ENGINE_CHECK(device != nullptr, "create_graphics_pipeline needs a device.");
        ENGINE_CHECK(out_pipeline != nullptr,
                     "create_graphics_pipeline needs somewhere to put the handle.");
        *out_pipeline = PipelineHandle{};

        if (desc.vertex.spirv == nullptr || desc.vertex.word_count == 0 ||
            desc.fragment.spirv == nullptr || desc.fragment.word_count == 0) {
            ENGINE_LOG_ERROR("A graphics pipeline needs both a vertex and a fragment module.");
            return Result::ErrorInit;
        }

        VkShaderModule vertex = VK_NULL_HANDLE;
        VkShaderModule fragment = VK_NULL_HANDLE;
        Result result = create_module(*device, desc.vertex, &vertex);
        if (succeeded(result)) {
            result = create_module(*device, desc.fragment, &fragment);
        }

        VkPipelineLayout layout = VK_NULL_HANDLE;
        if (succeeded(result)) {
            result = create_layout(*device, desc, &layout);
        }

        VkPipeline pipeline = VK_NULL_HANDLE;
        if (succeeded(result)) {
            const std::array<VkPipelineShaderStageCreateInfo, 2> stages{
                VkPipelineShaderStageCreateInfo{
                    .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                    .pNext = nullptr,
                    .flags = 0,
                    .stage = VK_SHADER_STAGE_VERTEX_BIT,
                    .module = vertex,
                    .pName = "main",
                    .pSpecializationInfo = nullptr,
                },
                VkPipelineShaderStageCreateInfo{
                    .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                    .pNext = nullptr,
                    .flags = 0,
                    .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
                    .module = fragment,
                    .pName = "main",
                    .pSpecializationInfo = nullptr,
                },
            };

            FixedState state;
            fill_fixed_state(state, desc);

            VertexInput vertex_input;
            fill_vertex_input(vertex_input, state, desc);

            const std::array<VkDynamicState, 3> dynamic_states{ VK_DYNAMIC_STATE_VIEWPORT,
                                                                VK_DYNAMIC_STATE_SCISSOR,
                                                                VK_DYNAMIC_STATE_CULL_MODE };
            VkPipelineDynamicStateCreateInfo dynamic{};
            dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
            dynamic.dynamicStateCount = static_cast<std::uint32_t>(dynamic_states.size());
            dynamic.pDynamicStates = dynamic_states.data();

            // Dynamic rendering replaces the render pass, so the formats come from
            // here. See DESIGN.md section 2.
            VkPipelineRenderingCreateInfo rendering{};
            rendering.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
            rendering.colorAttachmentCount = 1;
            rendering.pColorAttachmentFormats = &device->swapchain_format;
            // Every frame attaches the depth image, so every pipeline must name
            // its format or the draw is invalid. depth_test decides only whether
            // the pipeline reads and writes depth, not whether it is attached.
            rendering.depthAttachmentFormat = device->depth_format;

            VkGraphicsPipelineCreateInfo info{};
            info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
            info.pNext = &rendering;
            info.stageCount = static_cast<std::uint32_t>(stages.size());
            info.pStages = stages.data();
            info.pVertexInputState = &state.vertex_input;
            info.pInputAssemblyState = &state.assembly;
            info.pViewportState = &state.viewport;
            info.pRasterizationState = &state.raster;
            info.pMultisampleState = &state.multisample;
            info.pDepthStencilState = &state.depth;
            info.pColorBlendState = &state.blend;
            info.pDynamicState = &dynamic;
            info.layout = layout;

            const VkResult created = vkCreateGraphicsPipelines(device->device, VK_NULL_HANDLE, 1,
                                                               &info, nullptr, &pipeline);
            if (created != VK_SUCCESS) {
                ENGINE_LOG_ERROR("vkCreateGraphicsPipelines failed with {} ({})",
                                 vk::vk_result_name(created), static_cast<std::int32_t>(created));
                result = vk::to_result(created);
            }
        }

        // The modules are only needed while the pipeline builds.
        if (fragment != VK_NULL_HANDLE) {
            vkDestroyShaderModule(device->device, fragment, nullptr);
        }
        if (vertex != VK_NULL_HANDLE) {
            vkDestroyShaderModule(device->device, vertex, nullptr);
        }

        if (!succeeded(result)) {
            if (layout != VK_NULL_HANDLE) {
                vkDestroyPipelineLayout(device->device, layout, nullptr);
            }
            return result;
        }

        *out_pipeline = claim_slot(*device, pipeline, layout, desc.push_constant_size);
        return Result::Success;
    }

    void destroy_pipeline(Device* device, PipelineHandle pipeline) {
        if (device == nullptr) {
            return;
        }
        PipelineEntry* entry = vk::resolve_pipeline(*device, pipeline);
        if (entry == nullptr) {
            return;
        }

        vkDestroyPipeline(device->device, entry->pipeline, nullptr);
        vkDestroyPipelineLayout(device->device, entry->layout, nullptr);
        entry->pipeline = VK_NULL_HANDLE;
        entry->layout = VK_NULL_HANDLE;
        entry->alive = false;

        // Bumping the generation makes every existing handle to this slot stale.
        ++entry->generation;
        device->free_pipelines.push_back(pipeline.index());
    }

    void cmd_bind_pipeline(CommandList* commands, PipelineHandle pipeline) {
        ENGINE_CHECK(commands != nullptr, "cmd_bind_pipeline needs a command list.");
        ENGINE_CHECK(commands->owner != nullptr, "The command list has no device.");

        const PipelineEntry* entry = vk::resolve_pipeline(*commands->owner, pipeline);
        if (entry == nullptr) {
            ENGINE_LOG_ERROR("cmd_bind_pipeline received a stale or null pipeline handle.");
            return;
        }

        vkCmdBindPipeline(commands->buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, entry->pipeline);
    }

    void cmd_set_cull_mode(CommandList* commands, bool cull_back) {
        ENGINE_CHECK(commands != nullptr, "cmd_set_cull_mode needs a command list.");
        vkCmdSetCullMode(commands->buffer,
                         cull_back ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE);
    }

    void cmd_bind_texture(CommandList* commands, PipelineHandle pipeline,
                          TextureHandle texture) {
        ENGINE_CHECK(commands != nullptr, "cmd_bind_texture needs a command list.");

        const PipelineEntry* entry = vk::resolve_pipeline(*commands->owner, pipeline);
        const TextureEntry* bound = vk::resolve_texture(*commands->owner, texture);
        if (entry == nullptr || bound == nullptr) {
            ENGINE_LOG_ERROR("cmd_bind_texture received a stale or null handle.");
            return;
        }

        vkCmdBindDescriptorSets(commands->buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, entry->layout,
                                0, 1, &bound->set, 0, nullptr);
    }

    void cmd_push_constants(CommandList* commands, PipelineHandle pipeline, const void* data,
                            std::uint32_t size) {
        ENGINE_CHECK(commands != nullptr, "cmd_push_constants needs a command list.");
        ENGINE_CHECK(data != nullptr, "cmd_push_constants needs data.");

        const PipelineEntry* entry = vk::resolve_pipeline(*commands->owner, pipeline);
        if (entry == nullptr) {
            ENGINE_LOG_ERROR("cmd_push_constants received a stale or null pipeline handle.");
            return;
        }
        if (size != entry->push_constant_size) {
            ENGINE_LOG_ERROR("cmd_push_constants got {} bytes but the pipeline declared {}.", size,
                             entry->push_constant_size);
            return;
        }

        vkCmdPushConstants(commands->buffer, entry->layout, VK_SHADER_STAGE_VERTEX_BIT, 0, size,
                           data);
    }

    void cmd_draw(CommandList* commands, std::uint32_t vertex_count,
                  std::uint32_t instance_count, std::uint32_t first_vertex,
                  std::uint32_t first_instance) {
        ENGINE_CHECK(commands != nullptr, "cmd_draw needs a command list.");
        vkCmdDraw(commands->buffer, vertex_count, instance_count, first_vertex, first_instance);
    }

} // namespace engine::gfx
