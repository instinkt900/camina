#include "render/sky_pass.h"

#include "assets/shader.h"
#include "core/assert.h"
#include "core/log.h"
#include "render/shader_bindings.h"

#include <array>
#include <vector>

namespace engine::render {

    namespace {

        /// The set the cubemap binds at, which is what sky.frag declares.
        constexpr std::uint32_t kEnvironmentSet = 0;

        /// A full-screen triangle. See tonemap.vert for why it is not two.
        constexpr std::uint32_t kFullScreenVertices = 3;

        /// The push block, which must match sky.vert exactly.
        struct Push {
            Mat4 world_from_clip{ 1.0F };
            Vec4 camera_position{ 0.0F };
        };

    } // namespace

    bool SkyPass::build_pipeline(const assets::AssetSource& content, gfx::PipelineHandle& out) {
        assets::Shader vertex;
        assets::Shader fragment;
        if (!read_one_shader(content, "sky.vert", vertex) ||
            !read_one_shader(content, "sky.frag", fragment)) {
            return false;
        }

        std::vector<gfx::DescriptorBinding> bindings;
        if (!merge_bindings(vertex, fragment, bindings)) {
            return false;
        }

        const gfx::GraphicsPipelineDesc desc{
            .vertex = { .spirv = vertex.spirv.data(), .word_count = vertex.spirv.size() },
            .fragment = { .spirv = fragment.spirv.data(), .word_count = fragment.spirv.size() },
            // No vertex buffer. sky.vert builds its positions from the vertex
            // index, the same way tonemap.vert does.
            .attributes = nullptr,
            .attribute_count = 0,
            .vertex_stride = 0,
            .push_constant_size = sizeof(Push),
            // The vertex stage alone. The ray is unprojected there and the
            // fragment stage only normalizes what it was handed.
            .push_constant_stages = gfx::kStageBitVertex,
            .bindings = bindings.data(),
            .binding_count = bindings.size(),
            // Depth is what makes this cost only the pixels nothing covered.
            .depth_attachment = true,
            .depth_test = true,
            // The sky is behind everything, so there is nothing it should hide
            // from a later draw. The blended geometry that follows tests depth
            // against what the opaque draws left, and the sky must not join it.
            .depth_write = false,
            // The triangle sits at the far plane, which is what the depth image
            // still holds wherever no geometry drew. A greater-than test would
            // reject it there and the sky would never appear.
            .depth_equal = true,
            .blend = false,
            // One winding, whichever the index arithmetic gives. Culling
            // nothing is what keeps that from mattering.
            .cull_back = false,
            .depth_only = false,
            // The half float scene image, not the swapchain. The tonemap pass
            // exposes and curves the sky with everything else.
            .color_format = gfx::ColorTargetFormat::RGBA16F,
        };

        const gfx::Result result = gfx::create_graphics_pipeline(device_, desc, &out);
        if (!gfx::succeeded(result)) {
            ENGINE_LOG_ERROR("The sky pipeline did not build: {}", gfx::result_name(result));
            return false;
        }
        return true;
    }

    bool SkyPass::build_set(gfx::PipelineHandle pipeline, gfx::TextureHandle environment,
                            gfx::DescriptorSetHandle& out) {
        const std::array<gfx::DescriptorWrite, 1> writes{ {
            { .binding = 0,
              .kind = gfx::DescriptorKind::CombinedImageSampler,
              .texture = environment,
              .buffer = {} },
        } };
        gfx::DescriptorSetHandle built;
        if (!gfx::succeeded(gfx::create_descriptor_set(device_, pipeline, kEnvironmentSet,
                                                       writes.data(), writes.size(), &built))) {
            ENGINE_LOG_ERROR("The sky descriptor set could not be built.");
            return false;
        }
        out = built;
        return true;
    }

    bool SkyPass::create(gfx::Device* device, const assets::AssetSource& content) {
        ENGINE_ASSERT(device != nullptr, "SkyPass::create needs a device.");
        device_ = device;
        return build_pipeline(content, pipeline_);
    }

    void SkyPass::destroy() {
        if (device_ == nullptr) {
            return;
        }
        gfx::destroy_descriptor_set(device_, set_);
        set_ = gfx::DescriptorSetHandle{};
        bound_ = gfx::TextureHandle{};
        gfx::destroy_pipeline(device_, pipeline_);
        pipeline_ = gfx::PipelineHandle{};
        device_ = nullptr;
    }

    bool SkyPass::reload_shaders(const assets::AssetSource& content) {
        if (device_ == nullptr) {
            return false;
        }

        gfx::PipelineHandle rebuilt;
        if (!build_pipeline(content, rebuilt)) {
            ENGINE_LOG_WARN("The sky shader would not build. Keeping the old pipeline.");
            return false;
        }

        // A set is allocated against the layout of a pipeline, so the old set
        // cannot be bound to the new one. The next draw() builds the
        // replacement, because it is the caller that says which cubemap to
        // name and this function is not told.
        gfx::device_wait_idle(device_);
        gfx::destroy_descriptor_set(device_, set_);
        set_ = gfx::DescriptorSetHandle{};
        bound_ = gfx::TextureHandle{};
        gfx::destroy_pipeline(device_, pipeline_);
        pipeline_ = rebuilt;
        ENGINE_LOG_INFO("The sky pipeline was rebuilt.");
        return true;
    }

    void SkyPass::draw(gfx::CommandList* commands, gfx::TextureHandle environment,
                       const Mat4& world_from_clip, const Vec3& camera_position) {
        // A scene that names no environment draws no sky, and the clear color
        // stays where it was. That is what keeps a closed room the picture it
        // has always been, and it is the answer for a project with no cubemap
        // at all rather than a fallback grey sky nobody asked for.
        if (!pipeline_.valid() || !environment.valid()) {
            return;
        }

        // The world decides which cubemap this is, and a scene can name
        // another one while the program runs. The set names an image, so it is
        // rebuilt when that image changes and on no other frame.
        if (!(environment == bound_) || !set_.valid()) {
            gfx::DescriptorSetHandle rebuilt;
            if (!build_set(pipeline_, environment, rebuilt)) {
                return;
            }
            // The old set may still be read by a frame in flight, and a
            // cubemap change already stalls in MeshPass::update_environment for
            // the same reason, so this costs nothing extra.
            gfx::device_wait_idle(device_);
            gfx::destroy_descriptor_set(device_, set_);
            set_ = rebuilt;
            bound_ = environment;
        }

        const Push push{
            .world_from_clip = world_from_clip,
            .camera_position = Vec4{ camera_position, 1.0F },
        };

        gfx::cmd_bind_pipeline(commands, pipeline_);
        gfx::cmd_bind_descriptor_set(commands, pipeline_, kEnvironmentSet, set_);
        gfx::cmd_push_constants(commands, pipeline_, &push, sizeof(push));
        gfx::cmd_draw(commands, kFullScreenVertices, 1, 0, 0);
        ++draw_count_;
    }

} // namespace engine::render
