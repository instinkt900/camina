#include "render/mesh_pass.h"

#include "assets/mesh.h"
#include "core/log.h"
#include "scene/components.h"

#include <array>
#include <cstdint>
#include <vector>

namespace engine::render {

    namespace {

        /// What this pass reads out of the engine content tree, by source path.
        constexpr const char* kVertexShaderSource = "mesh.vert";
        constexpr const char* kFragmentShaderSource = "mesh.frag";

        /// The push constant block, which must match mesh.vert exactly.
        struct Push {
            Mat4 view_projection;
            Mat4 model;
        };

        // 128 bytes is the smallest push constant block Vulkan guarantees, and
        // this is exactly that. A third matrix would need a uniform buffer.
        constexpr std::size_t kPushLimit = 128;
        static_assert(sizeof(Push) == kPushLimit,
                      "The push block must fit the size every Vulkan device promises.");

    } // namespace

    MeshPass::~MeshPass() {
        destroy();
    }

    bool MeshPass::create(gfx::Device* device, const assets::Content& content) {
        if (device == nullptr) {
            ENGINE_LOG_ERROR("MeshPass::create needs a device.");
            return false;
        }
        device_ = device;

        std::vector<std::uint32_t> vertex_words;
        std::vector<std::uint32_t> fragment_words;
        if (!content.read_words(kVertexShaderSource, vertex_words) ||
            !content.read_words(kFragmentShaderSource, fragment_words)) {
            return false;
        }

        // The layout of assets::MeshVertex. The offsets come from the struct
        // rather than from constants, so the two cannot drift apart.
        using assets::MeshVertex;
        // Three of the four. The tangent is in the vertex and no shader reads
        // it yet, and an attribute nothing consumes is a validation warning.
        // M4.4b declares it when the normal mapping needs it.
        const std::array<gfx::VertexAttribute, 3> attributes{ {
            { .location = 0,
              .offset = offsetof(MeshVertex, position),
              .format = gfx::VertexFormat::Float3 },
            { .location = 1,
              .offset = offsetof(MeshVertex, normal),
              .format = gfx::VertexFormat::Float3 },
            { .location = 3,
              .offset = offsetof(MeshVertex, uv),
              .format = gfx::VertexFormat::Float2 },
        } };

        const gfx::GraphicsPipelineDesc desc{
            .vertex = { .spirv = vertex_words.data(), .word_count = vertex_words.size() },
            .fragment = { .spirv = fragment_words.data(), .word_count = fragment_words.size() },
            .attributes = attributes.data(),
            .attribute_count = attributes.size(),
            .vertex_stride = sizeof(MeshVertex),
            .push_constant_size = sizeof(Push),
            // No texture yet. A submesh carries a material GUID and it is null
            // until M4.4b writes one.
            .sample_texture = false,
            .depth_test = true,
            .cull_back = true,
        };

        const gfx::Result result = gfx::create_graphics_pipeline(device, desc, &pipeline_);
        if (!gfx::succeeded(result)) {
            ENGINE_LOG_ERROR("The mesh pipeline did not build: {}", gfx::result_name(result));
            return false;
        }
        return true;
    }

    void MeshPass::destroy() {
        if (device_ == nullptr) {
            return;
        }
        meshes_.destroy(device_);
        gfx::destroy_pipeline(device_, pipeline_);
        pipeline_ = gfx::PipelineHandle{};
        device_ = nullptr;
    }

    void MeshPass::draw(gfx::CommandList* commands, const scene::World& world,
                        const assets::Content& content, const Mat4& view_projection) {
        draw_count_ = 0;
        if (!pipeline_.valid()) {
            return;
        }

        gfx::cmd_bind_pipeline(commands, pipeline_);

        const auto view =
            world.registry().view<const scene::WorldTransform, const scene::MeshRenderer>();
        for (const auto [entity, transform, renderer] : view.each()) {
            if (!renderer.mesh.valid()) {
                continue;
            }
            const GpuMesh* mesh = meshes_.get(device_, content, renderer.mesh);
            if (mesh == nullptr) {
                continue;
            }

            const Push push{ .view_projection = view_projection, .model = transform.matrix };
            gfx::cmd_push_constants(commands, pipeline_, &push, sizeof(push));
            gfx::cmd_bind_vertex_buffer(commands, mesh->vertices);
            gfx::cmd_bind_index_buffer(commands, mesh->indices);

            // One draw call for each submesh. They share the two buffers, so
            // only the index range changes between them. M4.4b binds a material
            // here as well, which is the reason they are separate calls.
            for (const assets::MeshSubmesh& submesh : mesh->submeshes) {
                gfx::cmd_draw_indexed(commands, submesh.index_count, 1, submesh.first_index, 0);
                ++draw_count_;
            }
        }
    }

} // namespace engine::render
