#include "render/cube_pass.h"

#include "assets/texture.h"
#include "core/log.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace engine::render {

    namespace {

        /// One cube vertex. The stride must match the attributes below.
        struct Vertex {
            float x; ///< Position on the X axis, in meters.
            float y; ///< Position on the Y axis, in meters.
            float z; ///< Position on the Z axis, in meters.
            float u; ///< Texture coordinate across.
            float v; ///< Texture coordinate down. The origin is top-left.
        };

        constexpr std::size_t kFaceCount = 6;
        constexpr std::size_t kVerticesPerFace = 4;
        constexpr std::size_t kIndicesPerFace = 6;
        constexpr std::size_t kVertexCount = kFaceCount * kVerticesPerFace;
        constexpr std::size_t kIndexCount = kFaceCount * kIndicesPerFace;

        // Each face carries its own four vertices, so the texture coordinates can
        // differ at a shared corner. Every face winds counter-clockwise seen from
        // outside, which is what glTF supplies and what the backend treats as
        // front facing. See DESIGN.md section 3.
        //
        // A literal table reads better here than a generator. The values are cube
        // corners and texture corners, and naming each one would only add noise.
        // NOLINTBEGIN(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)
        constexpr std::array<Vertex, kVertexCount> kVertices{ {
            // +Z
            { -0.5F, -0.5F, 0.5F, 0.0F, 1.0F },
            { 0.5F, -0.5F, 0.5F, 1.0F, 1.0F },
            { 0.5F, 0.5F, 0.5F, 1.0F, 0.0F },
            { -0.5F, 0.5F, 0.5F, 0.0F, 0.0F },
            // -Z
            { 0.5F, -0.5F, -0.5F, 0.0F, 1.0F },
            { -0.5F, -0.5F, -0.5F, 1.0F, 1.0F },
            { -0.5F, 0.5F, -0.5F, 1.0F, 0.0F },
            { 0.5F, 0.5F, -0.5F, 0.0F, 0.0F },
            // +X
            { 0.5F, -0.5F, 0.5F, 0.0F, 1.0F },
            { 0.5F, -0.5F, -0.5F, 1.0F, 1.0F },
            { 0.5F, 0.5F, -0.5F, 1.0F, 0.0F },
            { 0.5F, 0.5F, 0.5F, 0.0F, 0.0F },
            // -X
            { -0.5F, -0.5F, -0.5F, 0.0F, 1.0F },
            { -0.5F, -0.5F, 0.5F, 1.0F, 1.0F },
            { -0.5F, 0.5F, 0.5F, 1.0F, 0.0F },
            { -0.5F, 0.5F, -0.5F, 0.0F, 0.0F },
            // +Y
            { -0.5F, 0.5F, 0.5F, 0.0F, 1.0F },
            { 0.5F, 0.5F, 0.5F, 1.0F, 1.0F },
            { 0.5F, 0.5F, -0.5F, 1.0F, 0.0F },
            { -0.5F, 0.5F, -0.5F, 0.0F, 0.0F },
            // -Y
            { -0.5F, -0.5F, -0.5F, 0.0F, 1.0F },
            { 0.5F, -0.5F, -0.5F, 1.0F, 1.0F },
            { 0.5F, -0.5F, 0.5F, 1.0F, 0.0F },
            { -0.5F, -0.5F, 0.5F, 0.0F, 0.0F },
        } };
        // NOLINTEND(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)

        /// Two triangles for each face, both counter-clockwise seen from outside.
        constexpr std::array<std::uint32_t, kIndicesPerFace> kFaceOrder{ 0, 1, 2, 0, 2, 3 };

        constexpr std::array<std::uint32_t, kIndexCount> build_indices() {
            std::array<std::uint32_t, kIndexCount> indices{};
            for (std::size_t face = 0; face < kFaceCount; ++face) {
                const auto base = static_cast<std::uint32_t>(face * kVerticesPerFace);
                const std::size_t out = face * kIndicesPerFace;
                for (std::size_t i = 0; i < kIndicesPerFace; ++i) {
                    indices[out + i] = base + kFaceOrder[i];
                }
            }
            return indices;
        }

        constexpr auto kIndices = build_indices();

        /// What this pass reads out of the engine content tree, by source path.
        constexpr const char* kVertexShaderSource = "cube.vert";
        constexpr const char* kFragmentShaderSource = "cube.frag";
        constexpr const char* kTextureSource = "cube.png";

        /// The gfx format that matches a cooked format and a cooked color space.
        [[nodiscard]] gfx::TextureFormat to_gfx_format(assets::TextureFormat format,
                                                       assets::ColorSpace space) {
            const bool srgb = space == assets::ColorSpace::Srgb;
            if (format == assets::TextureFormat::BC7) {
                return srgb ? gfx::TextureFormat::BC7Srgb : gfx::TextureFormat::BC7Unorm;
            }
            return srgb ? gfx::TextureFormat::RGBA8Srgb : gfx::TextureFormat::RGBA8Unorm;
        }

    } // namespace

    CubePass::~CubePass() {
        destroy();
    }

    bool CubePass::create(gfx::Device* device, const assets::Content& content) {
        if (device == nullptr) {
            ENGINE_LOG_ERROR("CubePass::create needs a device.");
            return false;
        }
        device_ = device;

        // The shaders come out of the cooked content tree now, rather than out
        // of a header the build generated. So a missing or unbuilt content
        // directory reports itself here, by name, rather than at draw time.
        std::vector<std::uint32_t> vertex_words;
        std::vector<std::uint32_t> fragment_words;
        if (!content.read_words(kVertexShaderSource, vertex_words) ||
            !content.read_words(kFragmentShaderSource, fragment_words)) {
            return false;
        }

        const gfx::BufferDesc vertex_desc{
            .data = kVertices.data(),
            .size = kVertices.size() * sizeof(Vertex),
            .usage = gfx::BufferUsage::Vertex,
        };
        gfx::Result result = gfx::create_buffer(device, vertex_desc, &vertices_);
        if (!gfx::succeeded(result)) {
            ENGINE_LOG_ERROR("The cube vertex buffer failed: {}", gfx::result_name(result));
            return false;
        }

        const gfx::BufferDesc index_desc{
            .data = kIndices.data(),
            .size = kIndices.size() * sizeof(std::uint32_t),
            .usage = gfx::BufferUsage::Index,
        };
        result = gfx::create_buffer(device, index_desc, &indices_);
        if (!gfx::succeeded(result)) {
            ENGINE_LOG_ERROR("The cube index buffer failed: {}", gfx::result_name(result));
            return false;
        }

        // The texture is a cooked file now, not a checkerboard this file builds.
        // It arrives with its mip chain and its color space already decided, so
        // nothing here has to know which of the two it is.
        std::vector<std::byte> texture_bytes;
        if (!content.read_bytes(kTextureSource, texture_bytes)) {
            return false;
        }
        assets::TextureView texture_view;
        if (!assets::read_texture(texture_bytes, texture_view, kTextureSource)) {
            return false;
        }

        const gfx::TextureDesc texture_desc{
            .pixels = texture_view.payload.data(),
            .size = texture_view.payload.size(),
            .width = texture_view.width,
            .height = texture_view.height,
            .mip_count = texture_view.mip_count,
            .format = to_gfx_format(texture_view.format, texture_view.color_space),
            // Linear, now that the texture carries a mip chain. Without mips a
            // checkerboard at a distance aliases badly, which is what M1 lived
            // with and what this milestone fixes.
            .sampler = { .filter = gfx::Filter::Linear },
        };
        result = gfx::create_texture(device, texture_desc, &texture_);
        if (!gfx::succeeded(result)) {
            ENGINE_LOG_ERROR("The cube texture failed: {}", gfx::result_name(result));
            return false;
        }

        const std::array<gfx::VertexAttribute, 2> attributes{ {
            { .location = 0, .offset = offsetof(Vertex, x), .format = gfx::VertexFormat::Float3 },
            { .location = 1, .offset = offsetof(Vertex, u), .format = gfx::VertexFormat::Float2 },
        } };

        const gfx::GraphicsPipelineDesc pipeline_desc{
            .vertex = { .spirv = vertex_words.data(), .word_count = vertex_words.size() },
            .fragment = { .spirv = fragment_words.data(),
                          .word_count = fragment_words.size() },
            .attributes = attributes.data(),
            .attribute_count = attributes.size(),
            .vertex_stride = sizeof(Vertex),
            .push_constant_size = sizeof(Mat4),
            .sample_texture = true,
            .depth_test = true,
            .cull_back = true,
        };

        result = gfx::create_graphics_pipeline(device, pipeline_desc, &pipeline_);
        if (!gfx::succeeded(result)) {
            ENGINE_LOG_ERROR("The cube pipeline did not build: {}", gfx::result_name(result));
            return false;
        }

        return true;
    }

    void CubePass::destroy() {
        if (device_ == nullptr) {
            return;
        }

        gfx::destroy_pipeline(device_, pipeline_);
        gfx::destroy_texture(device_, texture_);
        gfx::destroy_buffer(device_, indices_);
        gfx::destroy_buffer(device_, vertices_);

        pipeline_ = gfx::PipelineHandle{};
        texture_ = gfx::TextureHandle{};
        indices_ = gfx::BufferHandle{};
        vertices_ = gfx::BufferHandle{};
        device_ = nullptr;
    }

    void CubePass::draw(gfx::CommandList* commands, const Mat4& mvp) const {
        if (!pipeline_.valid()) {
            return;
        }

        gfx::cmd_bind_pipeline(commands, pipeline_);
        gfx::cmd_bind_texture(commands, pipeline_, texture_);
        gfx::cmd_push_constants(commands, pipeline_, &mvp, sizeof(Mat4));
        gfx::cmd_bind_vertex_buffer(commands, vertices_);
        gfx::cmd_bind_index_buffer(commands, indices_);
        gfx::cmd_draw_indexed(commands, static_cast<std::uint32_t>(kIndices.size()), 1, 0, 0);
    }

} // namespace engine::render
