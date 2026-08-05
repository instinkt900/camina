#include "gfx/vulkan/vk_internal.h"

#include "core/assert.h"
#include "core/log.h"

#include <array>
#include <vector>

namespace engine::gfx {

    namespace {

        /// Whether a stage carries a module at all.
        bool has_module(const ShaderCode& code) {
            return code.spirv != nullptr && code.word_count != 0;
        }

        /**
         * Reports whether the stages a caller supplied can build a pipeline.
         *
         * A vertex stage is always required. A fragment stage is required only
         * when the pipeline writes color, because a depth-only pass consumes
         * nothing a fragment shader would produce.
         */
        bool stages_are_valid(const GraphicsPipelineDesc& desc, bool has_fragment) {
            if (!has_module(desc.vertex)) {
                ENGINE_LOG_ERROR("A graphics pipeline needs a vertex module.");
                return false;
            }
            if (!has_fragment && !desc.depth_only) {
                ENGINE_LOG_ERROR("A graphics pipeline that writes color needs a fragment module.");
                return false;
            }
            return true;
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
            state.depth.depthWriteEnable =
                (desc.depth_test && desc.depth_write) ? VK_TRUE : VK_FALSE;
            state.depth.depthCompareOp = VK_COMPARE_OP_GREATER;
            state.depth.maxDepthBounds = 1.0F;

            state.blend_attachment.colorWriteMask =
                VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

            // The "over" operator. The color arrives multiplied by nothing, so
            // the source factor is its own alpha. The alpha channel takes what
            // is left rather than the same pair, so blending twice into the same
            // attachment builds up the coverage instead of scaling it down.
            state.blend_attachment.blendEnable = desc.blend ? VK_TRUE : VK_FALSE;
            state.blend_attachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
            state.blend_attachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            state.blend_attachment.colorBlendOp = VK_BLEND_OP_ADD;
            state.blend_attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            state.blend_attachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            state.blend_attachment.alphaBlendOp = VK_BLEND_OP_ADD;

            state.blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
            // A depth-only pipeline has no color attachment to blend into, and
            // the blend state must agree with that or the pipeline is invalid.
            state.blend.attachmentCount = desc.depth_only ? 0U : 1U;
            state.blend.pAttachments = desc.depth_only ? nullptr : &state.blend_attachment;
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

        /// Turns a described kind into the Vulkan one.
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

        /// Turns the described stage bits into the Vulkan ones.
        [[nodiscard]] VkShaderStageFlags to_vk_stage_flags(std::uint32_t stages) {
            VkShaderStageFlags flags = 0;
            if ((stages & kStageBitVertex) != 0) {
                flags |= VK_SHADER_STAGE_VERTEX_BIT;
            }
            if ((stages & kStageBitFragment) != 0) {
                flags |= VK_SHADER_STAGE_FRAGMENT_BIT;
            }
            if ((stages & kStageBitCompute) != 0) {
                flags |= VK_SHADER_STAGE_COMPUTE_BIT;
            }
            return flags;
        }

        /**
         * Builds one descriptor set layout for each set the shader declares.
         *
         * The bindings arrive sorted by set, so a run of equal sets is one
         * layout. A set the shader skips would shift every later set, so this
         * reports rather than building a layout the module does not match.
         */
        Result create_set_layouts(Device& device, const GraphicsPipelineDesc& desc,
                                  std::vector<VkDescriptorSetLayout>& out) {
            std::size_t at = 0;
            while (at < desc.binding_count) {
                const std::uint32_t set = desc.bindings[at].set;
                if (set != out.size()) {
                    ENGINE_LOG_ERROR("The shader declares set {} with set {} missing. Vulkan "
                                     "numbers set layouts by position, so no set may be skipped.",
                                     set, out.size());
                    return Result::ErrorInit;
                }

                std::vector<VkDescriptorSetLayoutBinding> bindings;
                while (at < desc.binding_count && desc.bindings[at].set == set) {
                    // Rule 4.2 passes a pointer and a count, so index it directly.
                    const DescriptorBinding& source = desc.bindings[at];
                    VkDescriptorSetLayoutBinding binding{};
                    binding.binding = source.binding;
                    binding.descriptorType = to_vk_descriptor_type(source.kind);
                    binding.descriptorCount = source.count;
                    binding.stageFlags = to_vk_stage_flags(source.stages);
                    bindings.push_back(binding);
                    ++at;
                }

                VkDescriptorSetLayoutCreateInfo info{};
                info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
                info.bindingCount = static_cast<std::uint32_t>(bindings.size());
                info.pBindings = bindings.data();

                VkDescriptorSetLayout layout = VK_NULL_HANDLE;
                ENGINE_VK_TRY(
                    vkCreateDescriptorSetLayout(device.device, &info, nullptr, &layout));
                out.push_back(layout);
            }
            return Result::Success;
        }

        /**
         * The camera matrix travels as a push constant, and the descriptor sets
         * come from the shader. Both stay optional.
         *
         * A texture descriptor set is allocated against device.texture_layout
         * rather than against the layout built here. Vulkan calls two set
         * layouts compatible when their bindings match, and a shader that reads
         * one combined image sampler at set 0 binding 0 reflects to exactly that
         * shape, so the set binds correctly. M5.2 gives a material its own set
         * and this stops mattering.
         */
        Result create_layout(Device& device, const GraphicsPipelineDesc& desc,
                             std::vector<VkDescriptorSetLayout>& set_layouts,
                             VkPipelineLayout* out) {
            const Result made = create_set_layouts(device, desc, set_layouts);
            if (!succeeded(made)) {
                return made;
            }

            VkPushConstantRange push{};
            push.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
            push.size = desc.push_constant_size;

            VkPipelineLayoutCreateInfo info{};
            info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
            info.setLayoutCount = static_cast<std::uint32_t>(set_layouts.size());
            info.pSetLayouts = set_layouts.data();
            if (desc.push_constant_size > 0) {
                info.pushConstantRangeCount = 1;
                info.pPushConstantRanges = &push;
            }

            ENGINE_VK_TRY(vkCreatePipelineLayout(device.device, &info, nullptr, out));
            return Result::Success;
        }

        /// Claims a free slot, or grows the pool. Returns the handle for the slot.
        PipelineHandle claim_slot(Device& device, VkPipeline pipeline, VkPipelineLayout layout,
                                  std::vector<VkDescriptorSetLayout>&& set_layouts,
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
            entry.set_layouts = std::move(set_layouts);
            entry.push_constant_size = push_constant_size;
            entry.alive = true;
            return PipelineHandle::make(index, entry.generation);
        }

        /// Frees the set layouts a pipeline owns, and empties the list.
        void destroy_set_layouts(Device& device, std::vector<VkDescriptorSetLayout>& layouts) {
            for (const VkDescriptorSetLayout layout : layouts) {
                if (layout != VK_NULL_HANDLE) {
                    vkDestroyDescriptorSetLayout(device.device, layout, nullptr);
                }
            }
            layouts.clear();
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
                destroy_set_layouts(device, entry.set_layouts);
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

        // A depth-only pipeline writes no color, so it needs no fragment stage.
        // Leaving it out is what makes a shadow pass cheap.
        const bool has_fragment = has_module(desc.fragment);
        if (!stages_are_valid(desc, has_fragment)) {
            return Result::ErrorInit;
        }

        VkShaderModule vertex = VK_NULL_HANDLE;
        VkShaderModule fragment = VK_NULL_HANDLE;
        Result result = create_module(*device, desc.vertex, &vertex);
        if (succeeded(result) && has_fragment) {
            result = create_module(*device, desc.fragment, &fragment);
        }

        VkPipelineLayout layout = VK_NULL_HANDLE;
        std::vector<VkDescriptorSetLayout> set_layouts;
        if (succeeded(result)) {
            result = create_layout(*device, desc, set_layouts, &layout);
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
            // One stage when there is no fragment module. The array holds two
            // entries either way, and the count is what leaves the second out.
            const std::uint32_t stage_count = has_fragment ? 2U : 1U;

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
            // A depth-only pass attaches no color image, and a pipeline whose
            // attachments disagree with the scope is undefined rather than an
            // error. See GraphicsPipelineDesc::depth_only.
            rendering.colorAttachmentCount = desc.depth_only ? 0 : 1;
            rendering.pColorAttachmentFormats =
                desc.depth_only ? nullptr : &device->swapchain_format;
            // Every frame attaches the depth image, so every pipeline must name
            // its format or the draw is invalid. depth_test decides only whether
            // the pipeline reads and writes depth, not whether it is attached.
            rendering.depthAttachmentFormat = device->depth_format;

            VkGraphicsPipelineCreateInfo info{};
            info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
            info.pNext = &rendering;
            info.stageCount = stage_count;
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
            // create_set_layouts() may have built some before it failed, and a
            // pipeline that never existed cannot free them later.
            destroy_set_layouts(*device, set_layouts);
            return result;
        }

        *out_pipeline = claim_slot(*device, pipeline, layout, std::move(set_layouts),
                                   desc.push_constant_size);
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
        destroy_set_layouts(*device, entry->set_layouts);
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

    void cmd_bind_descriptor_set(CommandList* commands, PipelineHandle pipeline,
                                 std::uint32_t set_index, DescriptorSetHandle set) {
        ENGINE_CHECK(commands != nullptr, "cmd_bind_descriptor_set needs a command list.");

        const PipelineEntry* entry = vk::resolve_pipeline(*commands->owner, pipeline);
        const DescriptorSetEntry* bound = vk::resolve_descriptor_set(*commands->owner, set);
        if (entry == nullptr || bound == nullptr) {
            ENGINE_LOG_ERROR("cmd_bind_descriptor_set received a stale or null handle.");
            return;
        }

        vkCmdBindDescriptorSets(commands->buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, entry->layout,
                                set_index, 1, &bound->set, 0, nullptr);
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
