#include "render/cube_pass.h"

#include "core/log.h"

#include <array>
#include <cstdint>

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
        // outside, which is what glTF supplies. The backend treats clockwise as
        // front facing to absorb the Y flip in the projection. See DESIGN.md
        // section 3.
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

        constexpr std::uint32_t kTextureSize = 8;
        constexpr std::uint32_t kCheckerSize = 2;
        constexpr std::size_t kBytesPerTexel = 4;

        /// Builds a checkerboard, because the asset pipeline arrives at M4.
        std::array<std::uint8_t, kTextureSize * kTextureSize * kBytesPerTexel> build_texture() {
            constexpr std::uint8_t kLight = 0xE0;
            constexpr std::uint8_t kDark = 0x40;
            constexpr std::uint8_t kOpaque = 0xFF;

            std::array<std::uint8_t, kTextureSize * kTextureSize * kBytesPerTexel> pixels{};
            for (std::uint32_t y = 0; y < kTextureSize; ++y) {
                for (std::uint32_t x = 0; x < kTextureSize; ++x) {
                    const bool light = ((x / kCheckerSize) + (y / kCheckerSize)) % 2 == 0;
                    const std::size_t offset =
                        (static_cast<std::size_t>(y) * kTextureSize + x) * kBytesPerTexel;
                    const std::uint8_t value = light ? kLight : kDark;
                    pixels[offset + 0] = value;
                    pixels[offset + 1] = value;
                    pixels[offset + 2] = kLight;
                    pixels[offset + 3] = kOpaque;
                }
            }
            return pixels;
        }

        constexpr auto kCubeVertexShader = std::to_array<std::uint32_t>(
#include "shaders/cube.vert.spv.inc"
        );

        constexpr auto kCubeFragmentShader = std::to_array<std::uint32_t>(
#include "shaders/cube.frag.spv.inc"
        );

    } // namespace

    CubePass::~CubePass() {
        destroy();
    }

    bool CubePass::create(gfx::Device* device) {
        if (device == nullptr) {
            ENGINE_LOG_ERROR("CubePass::create needs a device.");
            return false;
        }
        device_ = device;

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

        const auto pixels = build_texture();
        const gfx::TextureDesc texture_desc{
            .pixels = pixels.data(),
            .width = kTextureSize,
            .height = kTextureSize,
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
            .vertex = { .spirv = kCubeVertexShader.data(),
                        .word_count = kCubeVertexShader.size() },
            .fragment = { .spirv = kCubeFragmentShader.data(),
                          .word_count = kCubeFragmentShader.size() },
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
