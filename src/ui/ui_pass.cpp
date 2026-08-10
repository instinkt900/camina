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

        /**
         * @brief Reads one cooked shader out of the engine content tree.
         *
         * The same helper TonemapPass keeps privately. It is duplicated rather
         * than shared because src/ui is a separate target and the render one is
         * not public. Issue #197 records it.
         */
        [[nodiscard]] bool read_one_shader(const assets::Content& content,
                                           std::string_view source, assets::Shader& out) {
            const assets::ManifestEntry* entry = content.find(source);
            if (entry == nullptr || entry->outputs.empty()) {
                ENGINE_LOG_ERROR("{} is not in the cooked content tree.", source);
                return false;
            }
            std::vector<std::byte> bytes;
            if (!content.read_bytes(entry->outputs.front(), bytes)) {
                ENGINE_LOG_ERROR("{} would not read.", source);
                return false;
            }
            return assets::read_shader(bytes, out, entry->outputs.front().cooked);
        }

        /// Whether a moth_ui blend mode needs the blending pipeline.
        [[nodiscard]] bool needs_blending(moth_ui::BlendMode mode) {
            // Replace takes the opaque pipeline and Alpha takes the blending
            // one, and both are correct. Add, Multiply and Modulate each want
            // blend state of their own and draw as straight alpha here, which
            // is wrong rather than missing. Issue #206 holds it. No layout
            // asks for one yet, because nothing loads a layout.
            return mode != moth_ui::BlendMode::Replace;
        }

    } // namespace

    bool UiPass::create(gfx::Device* device, const assets::Content& content) {
        ENGINE_ASSERT(device != nullptr, "UiPass::create needs a device.");
        // Every failure below clears this again. draw() takes a null device to
        // mean the pass is not ready, so leaving it set after a failed build
        // would let a draw bind a pipeline handle that was never created.
        device_ = device;

        assets::Shader vertex;
        assets::Shader fragment;
        if (!read_one_shader(content, "ui.vert", vertex) ||
            !read_one_shader(content, "ui.frag", fragment)) {
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

        if (!gfx::succeeded(gfx::create_graphics_pipeline(device_, desc, &opaque_))) {
            ENGINE_LOG_ERROR("The opaque UI pipeline did not build.");
            device_ = nullptr;
            return false;
        }

        desc.blend = true;
        if (!gfx::succeeded(gfx::create_graphics_pipeline(device_, desc, &blended_))) {
            ENGINE_LOG_ERROR("The blended UI pipeline did not build.");
            gfx::destroy_pipeline(device_, opaque_);
            opaque_ = gfx::PipelineHandle{};
            device_ = nullptr;
            return false;
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
            gfx::destroy_pipeline(device_, blended_);
            blended_ = gfx::PipelineHandle{};
            gfx::destroy_pipeline(device_, opaque_);
            opaque_ = gfx::PipelineHandle{};
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

        // Both pipelines come from the same two shaders, so their set layouts
        // are identical and a set built against one binds with the other.
        gfx::DescriptorSetHandle set;
        if (!gfx::succeeded(gfx::create_descriptor_set(device_, opaque_, 0, writes.data(),
                                                       writes.size(), &set))) {
            ENGINE_LOG_ERROR("A UI descriptor set was not built. The pool serves a fixed "
                             "number, so a layout with many images can run out.");
        }
        // A failure is remembered as a null set. Retrying every frame would
        // fill the log and would not succeed, because the pool does not grow.
        sets_.emplace(key, set);
        return set;
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
        gfx::destroy_pipeline(device_, blended_);
        blended_ = gfx::PipelineHandle{};
        gfx::destroy_pipeline(device_, opaque_);
        opaque_ = gfx::PipelineHandle{};
        device_ = nullptr;
    }

    bool UiPass::upload(gfx::BufferHandle& buffer, const void* data, std::size_t bytes,
                        gfx::BufferUsage usage) {
        // gfx has no dynamic vertex or index buffer. create_buffer refuses one
        // with no data, and update_buffer refuses a vertex or an index buffer
        // outright, because both live in device-local memory the host cannot
        // reach. So the only way to put new geometry on the GPU each frame is
        // to build a new buffer from it.
        //
        // That is a real cost and it is not what this should do. It is the
        // largest gap the M6 spike found in gfx, and issue #204 holds it. A UI
        // is small, so the picture is right and the cost is bounded until then.
        gfx::destroy_buffer(device_, buffer);
        buffer = gfx::BufferHandle{};

        const gfx::BufferDesc desc{
            .data = data,
            .size = bytes,
            .usage = usage,
            .device_only = false,
        };
        if (!gfx::succeeded(gfx::create_buffer(device_, desc, &buffer))) {
            ENGINE_LOG_ERROR("A UI buffer of {} bytes was not created.", bytes);
            return false;
        }
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

        if (!upload(vertex_buffer, renderer.vertices().data(), vertex_bytes,
                    gfx::BufferUsage::Vertex) ||
            !upload(index_buffer, renderer.indices().data(), index_bytes,
                    gfx::BufferUsage::Index)) {
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

            const gfx::PipelineHandle wanted = needs_blending(batch.blend) ? blended_ : opaque_;
            if (wanted.value != bound.value) {
                gfx::cmd_bind_pipeline(commands, wanted);
                gfx::cmd_push_constants(commands, wanted, &push, sizeof(push));
                bound = wanted;
                // A set survives a pipeline change here, because both layouts
                // are the same. Rebinding it costs nothing and does not rely on
                // that, so the pipeline change forgets what was bound.
                bound_set = gfx::DescriptorSetHandle{};
            }

            report_filter_gap(batch.filter);

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
