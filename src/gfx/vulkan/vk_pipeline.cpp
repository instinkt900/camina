#include "gfx/vulkan/vk_internal.h"

#include "core/assert.h"
#include "core/log.h"

#include <array>

namespace engine::gfx {

    namespace {

        /// Returns the live entry a handle names, or nullptr when the handle is
        /// null, out of range, or stale.
        PipelineEntry* resolve(Device& device, PipelineHandle handle) {
            if (!handle.valid()) {
                return nullptr;
            }
            const std::uint32_t index = handle.index();
            if (index >= device.pipelines.size()) {
                return nullptr;
            }
            PipelineEntry& entry = device.pipelines[index];
            if (!entry.alive || entry.generation != handle.generation()) {
                return nullptr;
            }
            return &entry;
        }

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

        void fill_fixed_state(FixedState& state) {
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
            // Culling stays off until M1.3 brings real geometry. Vulkan clip space
            // puts +Y down, which flips the apparent winding, and a silently
            // culled first triangle is a hard failure to read.
            state.raster.cullMode = VK_CULL_MODE_NONE;
            state.raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
            state.raster.lineWidth = 1.0F;

            state.multisample.sType =
                VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
            state.multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

            // No depth attachment yet, so the test and the write stay off. M1.3
            // turns them on with VK_COMPARE_OP_GREATER for reverse-Z.
            state.depth.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
            state.depth.depthCompareOp = VK_COMPARE_OP_GREATER;

            state.blend_attachment.colorWriteMask =
                VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

            state.blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
            state.blend.attachmentCount = 1;
            state.blend.pAttachments = &state.blend_attachment;
        }

        /// Claims a free slot, or grows the pool. Returns the handle for the slot.
        PipelineHandle claim_slot(Device& device, VkPipeline pipeline, VkPipelineLayout layout) {
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
            entry.alive = true;
            return PipelineHandle::make(index, entry.generation);
        }

    } // namespace

    namespace vk {

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
            // Empty for now. M1.3 adds the descriptor set for the camera and the
            // texture.
            VkPipelineLayoutCreateInfo layout_info{};
            layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
            const VkResult created =
                vkCreatePipelineLayout(device->device, &layout_info, nullptr, &layout);
            if (created != VK_SUCCESS) {
                ENGINE_LOG_ERROR("vkCreatePipelineLayout failed with {} ({})",
                                 vk::vk_result_name(created), static_cast<std::int32_t>(created));
                result = vk::to_result(created);
            }
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
            fill_fixed_state(state);

            const std::array<VkDynamicState, 2> dynamic_states{ VK_DYNAMIC_STATE_VIEWPORT,
                                                                VK_DYNAMIC_STATE_SCISSOR };
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

        *out_pipeline = claim_slot(*device, pipeline, layout);
        return Result::Success;
    }

    void destroy_pipeline(Device* device, PipelineHandle pipeline) {
        if (device == nullptr) {
            return;
        }
        PipelineEntry* entry = resolve(*device, pipeline);
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

        const PipelineEntry* entry = resolve(*commands->owner, pipeline);
        if (entry == nullptr) {
            ENGINE_LOG_ERROR("cmd_bind_pipeline received a stale or null pipeline handle.");
            return;
        }

        vkCmdBindPipeline(commands->buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, entry->pipeline);
    }

    void cmd_draw(CommandList* commands, std::uint32_t vertex_count,
                  std::uint32_t instance_count, std::uint32_t first_vertex,
                  std::uint32_t first_instance) {
        ENGINE_CHECK(commands != nullptr, "cmd_draw needs a command list.");
        vkCmdDraw(commands->buffer, vertex_count, instance_count, first_vertex, first_instance);
    }

} // namespace engine::gfx
