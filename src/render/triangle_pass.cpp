#include "render/triangle_pass.h"

#include "core/log.h"

#include <array>
#include <cstdint>

namespace engine::render {

    namespace {

        // cmake/Shaders.cmake compiles each file with glslc and writes a braced
        // list of 32-bit words. std::to_array deduces the length, so nothing here
        // has to state the module size.
        constexpr auto kTriangleVertex = std::to_array<std::uint32_t>(
#include "shaders/triangle.vert.spv.inc"
        );

        constexpr auto kTriangleFragment = std::to_array<std::uint32_t>(
#include "shaders/triangle.frag.spv.inc"
        );

        /// The vertex shader builds all three positions from gl_VertexIndex.
        constexpr std::uint32_t kTriangleVertexCount = 3;

    } // namespace

    TrianglePass::~TrianglePass() {
        destroy();
    }

    bool TrianglePass::create(gfx::Device* device) {
        if (device == nullptr) {
            ENGINE_LOG_ERROR("TrianglePass::create needs a device.");
            return false;
        }

        const gfx::GraphicsPipelineDesc desc{
            .vertex = { .spirv = kTriangleVertex.data(), .word_count = kTriangleVertex.size() },
            .fragment = { .spirv = kTriangleFragment.data(),
                          .word_count = kTriangleFragment.size() },
        };

        const gfx::Result result = gfx::create_graphics_pipeline(device, desc, &pipeline_);
        if (!gfx::succeeded(result)) {
            ENGINE_LOG_ERROR("The triangle pipeline did not build: {}",
                             gfx::result_name(result));
            return false;
        }

        device_ = device;
        return true;
    }

    void TrianglePass::destroy() {
        if (device_ != nullptr && pipeline_.valid()) {
            gfx::destroy_pipeline(device_, pipeline_);
        }
        pipeline_ = gfx::PipelineHandle{};
        device_ = nullptr;
    }

    void TrianglePass::draw(gfx::CommandList* commands) const {
        if (!pipeline_.valid()) {
            return;
        }
        gfx::cmd_bind_pipeline(commands, pipeline_);
        gfx::cmd_draw(commands, kTriangleVertexCount, 1, 0, 0);
    }

} // namespace engine::render
