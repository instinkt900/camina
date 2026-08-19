#include "render/debug_line_pass.h"

#include "assets/shader.h"
#include "core/assert.h"
#include "core/log.h"
#include "core/profile.h"
#include "render/shader_bindings.h"

#include <array>
#include <cstdint>
#include <string_view>
#include <vector>

namespace engine::render {

    namespace {

        /// What the vertex stage reads out of the push constant block.
        struct Push {
            Mat4 clip_from_world{ 1.0F };
        };

        /// Reads one cooked shader out of the engine content tree. The same
        /// helper TonemapPass and UiPass each keep privately. Issue #197.
        [[nodiscard]] bool read_one_shader(const assets::AssetSource& content, std::string_view source,
                                           assets::Shader& out) {
            // assets_for() says which source it could not find, so there is no
            // message here.
            std::vector<assets::AssetRecord> forms;
            if (!content.assets_for(source, forms)) {
                return false;
            }
            std::vector<std::byte> bytes;
            if (!content.read(forms.front().guid, bytes)) {
                ENGINE_LOG_ERROR("{} would not read.", source);
                return false;
            }
            return assets::read_shader(bytes, out, forms.front().name);
        }

    } // namespace

    bool DebugLinePass::create(gfx::Device* device, const assets::AssetSource& content) {
        ENGINE_ASSERT(device != nullptr, "DebugLinePass::create needs a device.");
        device_ = device;

        assets::Shader vertex;
        assets::Shader fragment;
        if (!read_one_shader(content, "debug_line.vert", vertex) ||
            !read_one_shader(content, "debug_line.frag", fragment)) {
            device_ = nullptr;
            return false;
        }

        std::vector<gfx::DescriptorBinding> bindings;
        if (!merge_bindings(vertex, fragment, bindings)) {
            device_ = nullptr;
            return false;
        }

        // Position then color, in the order Vertex holds them.
        static constexpr std::array<gfx::VertexAttribute, 2> kAttributes{ {
            { .location = 0, .offset = 0, .format = gfx::VertexFormat::Float3 },
            { .location = 1, .offset = sizeof(float) * 3, .format = gfx::VertexFormat::Float3 },
        } };

        const gfx::GraphicsPipelineDesc desc{
            .vertex = { .spirv = vertex.spirv.data(), .word_count = vertex.spirv.size() },
            .fragment = { .spirv = fragment.spirv.data(), .word_count = fragment.spirv.size() },
            .attributes = kAttributes.data(),
            .attribute_count = kAttributes.size(),
            .vertex_stride = sizeof(Vertex),
            .push_constant_size = sizeof(Push),
            .push_constant_stages = gfx::kStageBitVertex,
            .bindings = bindings.empty() ? nullptr : bindings.data(),
            .binding_count = bindings.size(),
            .topology = gfx::PrimitiveTopology::LineList,
            // The tonemap scope attaches no depth, and this draws inside it.
            // Vulkan calls a pipeline that disagrees with its scope undefined
            // rather than an error.
            .depth_attachment = false,
            .depth_test = false,
            .depth_write = false,
            .blend = false,
            // A line has no facing to cull by, and culling is dynamic state that
            // carries from whatever drew last. Saying so here is what stops the
            // wireframe from vanishing because another pass left culling on.
            // That is issue #188.
            .cull_back = false,
            .depth_only = false,
            .color_format = gfx::ColorTargetFormat::Swapchain,
        };

        if (!gfx::succeeded(gfx::create_graphics_pipeline(device_, desc, &pipeline_))) {
            ENGINE_LOG_ERROR("The debug line pipeline did not build.");
            device_ = nullptr;
            return false;
        }
        return true;
    }

    void DebugLinePass::destroy() {
        if (device_ == nullptr) {
            return;
        }
        for (gfx::BufferHandle& buffer : vertices_) {
            gfx::destroy_buffer(device_, buffer);
            buffer = gfx::BufferHandle{};
        }
        gfx::destroy_pipeline(device_, pipeline_);
        pipeline_ = gfx::PipelineHandle{};
        device_ = nullptr;
    }

    void DebugLinePass::draw(gfx::CommandList* commands, const Mat4& clip_from_world,
                             std::span<const physics::DebugLine> lines) {
        line_count_ = 0;
        if (device_ == nullptr || lines.empty()) {
            // Nothing uploaded and nothing recorded, so a frame with the toggle
            // off pays for the branch and nothing else.
            return;
        }

        ENGINE_PROFILE_ZONE_N("debug line pass");

        // Two ends for each line, flattened into what the vertex stage reads.
        std::vector<Vertex> vertices;
        vertices.reserve(lines.size() * 2);
        for (const physics::DebugLine& line : lines) {
            vertices.push_back(Vertex{ .position = line.from, .color = line.color });
            vertices.push_back(Vertex{ .position = line.to, .color = line.color });
        }

        // Move to the next slot first. The buffer there was last used three
        // frames ago, so nothing in flight still reads it. Reusing one slot
        // would destroy a buffer the frame before is still reading, which
        // synchronization validation reports.
        slot_ = (slot_ + 1) % kSlots;
        gfx::BufferHandle& buffer = vertices_.at(slot_);

        // gfx has no dynamic vertex buffer, so new geometry means a new buffer
        // each frame. That is a real cost and it is issue #204, which the M6
        // spike raised for the same reason.
        gfx::destroy_buffer(device_, buffer);
        buffer = gfx::BufferHandle{};

        const gfx::BufferDesc buffer_desc{
            .data = vertices.data(),
            .size = vertices.size() * sizeof(Vertex),
            .usage = gfx::BufferUsage::Vertex,
        };
        if (!gfx::succeeded(gfx::create_buffer(device_, buffer_desc, &buffer))) {
            ENGINE_LOG_ERROR("The debug line buffer of {} lines was not created.", lines.size());
            return;
        }

        const Push push{ .clip_from_world = clip_from_world };
        gfx::cmd_bind_pipeline(commands, pipeline_);
        gfx::cmd_push_constants(commands, pipeline_, &push, sizeof(push));
        gfx::cmd_bind_vertex_buffer(commands, buffer);
        gfx::cmd_draw(commands, static_cast<std::uint32_t>(vertices.size()), 1, 0, 0);

        line_count_ = lines.size();
    }

} // namespace engine::render
