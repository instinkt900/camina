#include "ui/ui_pass.h"

#include "assets/shader.h"
#include "core/assert.h"
#include "core/log.h"
#include "render/shader_bindings.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace engine::ui {

    namespace {

        /// What the vertex shader reads out of the push constant block.
        struct Push {
            float inv_logical_width = 0.0F;
            float inv_logical_height = 0.0F;
        };

    } // namespace

    bool UiPass::create(gfx::Device* device, const assets::AssetSource& content) {
        ENGINE_ASSERT(device != nullptr, "UiPass::create needs a device.");
        // Every failure below clears this again. draw() takes a null device to
        // mean the pass is not ready, so leaving it set after a failed build
        // would let a draw bind a pipeline handle that was never created.
        device_ = device;

        assets::Shader vertex;
        assets::Shader fragment;
        if (!render::read_one_shader(content, "ui.vert", vertex) ||
            !render::read_one_shader(content, "ui.frag", fragment)) {
            device_ = nullptr;
            return false;
        }

        std::vector<gfx::DescriptorBinding> bindings;
        if (!render::merge_bindings(vertex, fragment, bindings)) {
            device_ = nullptr;
            return false;
        }

        // Position, texture coordinate, and colour, in the order
        // engine::ui::Vertex holds them.
        static constexpr std::array<gfx::VertexAttribute, 3> kAttributes{ {
            { .location = 0, .offset = 0, .format = gfx::VertexFormat::Float2 },
            { .location = 1,
              .offset = sizeof(float) * 2,
              .format = gfx::VertexFormat::Float2 },
            { .location = 2,
              .offset = sizeof(float) * 4,
              .format = gfx::VertexFormat::Float4 },
        } };

        gfx::GraphicsPipelineDesc desc{
            .vertex = { .spirv = vertex.spirv.data(), .word_count = vertex.spirv.size() },
            .fragment = { .spirv = fragment.spirv.data(), .word_count = fragment.spirv.size() },
            .attributes = kAttributes.data(),
            .attribute_count = kAttributes.size(),
            .vertex_stride = sizeof(Vertex),
            .push_constant_size = sizeof(Push),
            .push_constant_stages = gfx::kStageBitVertex,
            .bindings = bindings.empty() ? nullptr : bindings.data(),
            .binding_count = bindings.size(),
            // The tonemap scope attaches no depth, and this draws inside it.
            // Vulkan calls a pipeline that disagrees with its scope undefined
            // rather than an error, so this must match cmd_begin_rendering.
            .depth_attachment = false,
            .depth_test = false,
            .depth_write = false,
            .blend = false,
            // A quad's winding is whatever the index order gives, and this
            // records one order for every quad. Culling nothing keeps that from
            // mattering, which is what issue #188 was about.
            .cull_back = false,
            .depth_only = false,
            .color_format = gfx::ColorTargetFormat::Swapchain,
        };

        // One pipeline for each blend mode. They differ in nothing but the
        // blend state, so the descriptor above is built once and only those two
        // fields change. Issue #206.
        for (std::size_t slot = 0; slot < kBlendModeCount; ++slot) {
            const BlendPipeline wanted = blend_pipeline_for(slot);
            desc.blend = wanted.blend;
            desc.blend_state = wanted.state;
            if (!gfx::succeeded(
                    gfx::create_graphics_pipeline(device_, desc, &pipelines_[slot]))) {
                ENGINE_LOG_ERROR("UI blend pipeline {} did not build.", slot);
                destroy_pipelines();
                device_ = nullptr;
                return false;
            }
        }

        // One white texel for every run that draws no image. The fragment stage
        // multiplies by it, so white leaves the vertex colour alone. sRGB
        // because the swapchain is, and 255 decodes to 1.0 either way.
        constexpr std::array<std::uint8_t, 4> kWhite{ 255, 255, 255, 255 };
        const gfx::TextureDesc white_desc{
            .pixels = kWhite.data(),
            .size = kWhite.size(),
            .width = 1,
            .height = 1,
            .mip_count = 1,
            .format = gfx::TextureFormat::RGBA8Srgb,
            .sampler = { .filter = gfx::Filter::Nearest },
        };
        if (!gfx::succeeded(gfx::create_texture(device_, white_desc, &white_))) {
            ENGINE_LOG_ERROR("The white UI texel was not created.");
            destroy_pipelines();
            device_ = nullptr;
            return false;
        }
        return true;
    }

    void UiPass::report_filter_gap(moth_ui::TextureFilter filter) {
        // A gfx sampler belongs to the texture it was uploaded with, so the
        // filter a run was recorded under cannot be applied at bind time. Issue
        // #209 holds it. Say so once rather than draw a nearest filtered image
        // blurred and report nothing.
        if (filter != moth_ui::TextureFilter::Nearest || reported_filter_) {
            return;
        }
        ENGINE_LOG_WARN("A layout asked for a nearest filter and the image draws linear. "
                        "gfx binds a sampler with its texture. See issue #209.");
        reported_filter_ = true;
    }

    gfx::DescriptorSetHandle UiPass::set_for(gfx::TextureHandle texture) {
        const std::uint64_t key = texture.value;
        if (const auto found = sets_.find(key); found != sets_.end()) {
            return found->second;
        }

        const std::array<gfx::DescriptorWrite, 1> writes{ {
            { .binding = 0,
              .kind = gfx::DescriptorKind::CombinedImageSampler,
              .texture = texture.valid() ? texture : white_,
              .buffer = {} },
        } };

        // Every pipeline comes from the same two shaders, so their set layouts
        // are identical and a set built against one binds with any of them.
        gfx::DescriptorSetHandle set;
        if (!gfx::succeeded(gfx::create_descriptor_set(device_, pipelines_[0], 0, writes.data(),
                                                       writes.size(), &set))) {
            ENGINE_LOG_ERROR("A UI descriptor set was not built. The pool serves a fixed "
                             "number, so a layout with many images can run out.");
        }
        // A failure is remembered as a null set. Retrying every frame would
        // fill the log and would not succeed, because the pool does not grow.
        sets_.emplace(key, set);
        return set;
    }

    void UiPass::forget_sets() {
        if (device_ == nullptr) {
            return;
        }
        for (const auto& [key, set] : sets_) {
            gfx::destroy_descriptor_set(device_, set);
        }
        sets_.clear();
    }

    void UiPass::destroy() {
        if (device_ == nullptr) {
            return;
        }
        for (const auto& [key, set] : sets_) {
            gfx::destroy_descriptor_set(device_, set);
        }
        sets_.clear();
        gfx::destroy_texture(device_, white_);
        white_ = gfx::TextureHandle{};
        for (gfx::BufferHandle& buffer : vertices_) {
            gfx::destroy_buffer(device_, buffer);
            buffer = gfx::BufferHandle{};
        }
        for (gfx::BufferHandle& buffer : indices_) {
            gfx::destroy_buffer(device_, buffer);
            buffer = gfx::BufferHandle{};
        }
        vertex_capacity_.fill(0);
        index_capacity_.fill(0);
        destroy_pipelines();
        device_ = nullptr;
    }

    void UiPass::destroy_pipelines() {
        // Every failure path in create() runs this, and a handle that was never
        // built is null. destroy_pipeline takes a null handle, so the loop needs
        // no count of how many were built before the one that failed.
        for (gfx::PipelineHandle& pipeline : pipelines_) {
            gfx::destroy_pipeline(device_, pipeline);
            pipeline = gfx::PipelineHandle{};
        }
    }

    bool UiPass::upload(gfx::BufferHandle& buffer, std::size_t& capacity, const void* data,
                        std::size_t bytes, gfx::BufferUsage usage) {
        // A host-visible vertex or index buffer, which closed issue #204.
        // Before it, gfx refused a vertex buffer with no data and refused to
        // update one, so the only way to put new geometry on the GPU each frame
        // was to destroy the buffer and build another from the recording. That
        // was an allocation and a free for each of two buffers on every frame,
        // and it was a correctness trap besides: destroying the buffer the
        // previous frame is still reading is a real error the validation layer
        // reports.
        //
        // The slot ring is still what answers the frames in flight, and it has
        // to be. update_buffer() writes straight into memory the GPU may be
        // reading, so the buffer written here must be one no frame in flight
        // still names. gfx says whose problem that is on BufferMemory.
        if (bytes > capacity) {
            // Grown rather than resized, because a mapped allocation cannot
            // change size. Half again on top of what is asked for, so a
            // recording that creeps upward does not reallocate every frame.
            const std::size_t wanted = bytes + (bytes / 2);
            gfx::destroy_buffer(device_, buffer);
            buffer = gfx::BufferHandle{};
            capacity = 0;

            const gfx::BufferDesc desc{
                .data = nullptr,
                .size = wanted,
                .usage = usage,
                .memory = gfx::BufferMemory::HostVisible,
            };
            if (!gfx::succeeded(gfx::create_buffer(device_, desc, &buffer))) {
                ENGINE_LOG_ERROR("A UI buffer of {} bytes was not created.", wanted);
                return false;
            }
            capacity = wanted;
            ENGINE_LOG_DEBUG("A UI buffer grew to {} bytes for a recording of {}.", wanted,
                             bytes);
        }

        gfx::update_buffer(device_, buffer, data, bytes);
        return true;
    }

    void UiPass::draw(gfx::CommandList* commands, const Renderer& renderer,
                      gfx::Extent2D extent) {
        ENGINE_ASSERT(commands != nullptr, "UiPass::draw needs a command list.");
        if (device_ == nullptr || renderer.batches().empty()) {
            return;
        }

        const std::size_t vertex_bytes = renderer.vertices().size() * sizeof(Vertex);
        const std::size_t index_bytes = renderer.indices().size() * sizeof(std::uint32_t);
        if (vertex_bytes == 0 || index_bytes == 0) {
            return;
        }

        // Move to the next slot first. The buffers there were last used three
        // frames ago, so nothing in flight still reads them and destroying
        // them is safe. Reusing one slot would destroy a buffer the frame
        // before is still reading, which synchronization validation reports.
        slot_ = (slot_ + 1) % kSlots;
        gfx::BufferHandle& vertex_buffer = vertices_.at(slot_);
        gfx::BufferHandle& index_buffer = indices_.at(slot_);

        if (!upload(vertex_buffer, vertex_capacity_.at(slot_), renderer.vertices().data(),
                    vertex_bytes, gfx::BufferUsage::Vertex) ||
            !upload(index_buffer, index_capacity_.at(slot_), renderer.indices().data(),
                    index_bytes, gfx::BufferUsage::Index)) {
            return;
        }

        const Push push{
            .inv_logical_width = 1.0F / static_cast<float>(renderer.logical_width()),
            .inv_logical_height = 1.0F / static_cast<float>(renderer.logical_height()),
        };

        gfx::cmd_bind_vertex_buffer(commands, vertex_buffer);
        gfx::cmd_bind_index_buffer(commands, index_buffer);

        gfx::PipelineHandle bound{};
        gfx::DescriptorSetHandle bound_set{};
        for (const Batch& batch : renderer.batches()) {
            if (batch.index_count == 0) {
                continue;
            }

            const gfx::PipelineHandle wanted = pipelines_[blend_mode_index(batch.blend)];
            if (wanted.value != bound.value) {
                gfx::cmd_bind_pipeline(commands, wanted);
                gfx::cmd_push_constants(commands, wanted, &push, sizeof(push));
                bound = wanted;
                // A set survives a pipeline change here, because both layouts
                // are the same. Rebinding it costs nothing and does not rely on
                // that, so the pipeline change forgets what was bound.
                bound_set = gfx::DescriptorSetHandle{};
            }

            // Only a run that reads a real image can show the gap. A run of
            // shapes binds one white texel, where every filter gives the same
            // answer, so reporting there would name a problem the frame does
            // not have.
            if (batch.texture.valid()) {
                report_filter_gap(batch.filter);
            }

            const gfx::DescriptorSetHandle set = set_for(batch.texture);
            if (!set.valid()) {
                // Nothing to bind means the fragment stage would read an
                // undefined descriptor, which is worse than a missing run.
                continue;
            }
            if (set.value != bound_set.value) {
                gfx::cmd_bind_descriptor_set(commands, wanted, 0, set);
                bound_set = set;
            }

            if (batch.clipped) {
                gfx::cmd_set_scissor(commands, batch.clip_x, batch.clip_y, batch.clip_width,
                                     batch.clip_height);
            } else {
                gfx::cmd_set_scissor(commands, 0, 0, extent.width, extent.height);
            }

            gfx::cmd_draw_indexed(commands, batch.index_count, 1, batch.first_index, 0);
        }

        // The scissor is dynamic state and it carries past this pass. Each
        // cmd_begin_*_rendering resets it, so the next frame is safe either
        // way, but leaving a clip behind is the shape of issue #188 and it is
        // not worth repeating.
        gfx::cmd_set_scissor(commands, 0, 0, extent.width, extent.height);
    }

}
