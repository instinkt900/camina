#include "ui/ui_pass.h"

#include "assets/shader.h"
#include "core/assert.h"
#include "core/log.h"
#include "render/shader_bindings.h"

#include <array>
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
            // Replace is the only one the opaque pipeline serves. Add,
            // Multiply and Modulate each want their own blend state, and this
            // increment draws them with straight alpha instead. Issue #197
            // records that, because a wrong blend is visible rather than
            // silent.
            return mode != moth_ui::BlendMode::Replace;
        }

    } // namespace

    render::PassDesc UiPass::declare() {
        static constexpr std::array<render::ResourceWrite, 2> kWrites{ {
            { render::kFrameColor, gfx::ResourceState::ColorTarget },
            { render::kFrameDepth, gfx::ResourceState::DepthTarget },
        } };
        return render::PassDesc{ .name = "ui", .reads = {}, .writes = kWrites };
    }

    bool UiPass::create(gfx::Device* device, const assets::Content& content) {
        ENGINE_ASSERT(device != nullptr, "UiPass::create needs a device.");
        device_ = device;

        assets::Shader vertex;
        assets::Shader fragment;
        if (!read_one_shader(content, "ui.vert", vertex) ||
            !read_one_shader(content, "ui.frag", fragment)) {
            return false;
        }

        std::vector<gfx::DescriptorBinding> bindings;
        if (!render::merge_bindings(vertex, fragment, bindings)) {
            return false;
        }

        // Position and colour. There is no uv, because this increment draws no
        // texture. Issue #198 adds one.
        static constexpr std::array<gfx::VertexAttribute, 2> kAttributes{ {
            { .location = 0, .offset = 0, .format = gfx::VertexFormat::Float2 },
            { .location = 1,
              .offset = sizeof(float) * 2,
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
            // The UI draws over a finished frame and owes nothing to depth.
            // The scope still attaches the depth image, which declare() says.
            .depth_attachment = true,
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
            return false;
        }

        desc.blend = true;
        if (!gfx::succeeded(gfx::create_graphics_pipeline(device_, desc, &blended_))) {
            ENGINE_LOG_ERROR("The blended UI pipeline did not build.");
            return false;
        }
        return true;
    }

    void UiPass::destroy() {
        if (device_ == nullptr) {
            return;
        }
        gfx::destroy_buffer(device_, vertices_);
        vertices_ = gfx::BufferHandle{};
        vertex_capacity_ = 0;
        gfx::destroy_buffer(device_, indices_);
        indices_ = gfx::BufferHandle{};
        index_capacity_ = 0;
        gfx::destroy_pipeline(device_, blended_);
        blended_ = gfx::PipelineHandle{};
        gfx::destroy_pipeline(device_, opaque_);
        opaque_ = gfx::PipelineHandle{};
        device_ = nullptr;
    }

    bool UiPass::ensure_capacity(gfx::BufferHandle& buffer, std::size_t& capacity,
                                 std::size_t bytes, gfx::BufferUsage usage) {
        if (capacity >= bytes && capacity > 0) {
            return true;
        }

        // Double rather than fit exactly, so a UI that grows by one quad each
        // frame does not reallocate each frame.
        constexpr std::size_t kFirstCapacity = 1024;
        std::size_t next = capacity == 0 ? kFirstCapacity : capacity;
        while (next < bytes) {
            next *= 2;
        }

        gfx::destroy_buffer(device_, buffer);
        buffer = gfx::BufferHandle{};

        const gfx::BufferDesc desc{
            .data = nullptr,
            .size = next,
            .usage = usage,
            .device_only = false,
        };
        if (!gfx::succeeded(gfx::create_buffer(device_, desc, &buffer))) {
            ENGINE_LOG_ERROR("A UI buffer of {} bytes was not created.", next);
            capacity = 0;
            return false;
        }
        capacity = next;
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

        if (!ensure_capacity(vertices_, vertex_capacity_, vertex_bytes, gfx::BufferUsage::Vertex) ||
            !ensure_capacity(indices_, index_capacity_, index_bytes, gfx::BufferUsage::Index)) {
            return;
        }

        gfx::update_buffer(device_, vertices_, renderer.vertices().data(), vertex_bytes);
        gfx::update_buffer(device_, indices_, renderer.indices().data(), index_bytes);

        const Push push{
            .inv_logical_width = 1.0F / static_cast<float>(renderer.logical_width()),
            .inv_logical_height = 1.0F / static_cast<float>(renderer.logical_height()),
        };

        gfx::cmd_bind_vertex_buffer(commands, vertices_);
        gfx::cmd_bind_index_buffer(commands, indices_);

        gfx::PipelineHandle bound{};
        for (const Batch& batch : renderer.batches()) {
            if (batch.index_count == 0) {
                continue;
            }

            const gfx::PipelineHandle wanted = needs_blending(batch.blend) ? blended_ : opaque_;
            if (wanted.value != bound.value) {
                gfx::cmd_bind_pipeline(commands, wanted);
                gfx::cmd_push_constants(commands, wanted, &push, sizeof(push));
                bound = wanted;
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
