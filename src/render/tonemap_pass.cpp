#include "render/tonemap_pass.h"

#include "assets/shader.h"
#include "core/assert.h"
#include "core/log.h"
#include "render/shader_bindings.h"

#include <array>
#include <vector>

namespace engine::render {

    namespace {

        /// The set the scene image binds at, which is what tonemap.frag declares.
        constexpr std::uint32_t kSceneSet = 0;

        /// A full screen triangle. See tonemap.vert for why it is not two.
        constexpr std::uint32_t kFullScreenVertices = 3;

        /**
         * Reads one cooked shader that has a single form and no variants.
         *
         * @param content The engine content tree.
         * @param source The source path the manifest lists, such as "tonemap.vert".
         * @param out Receives the module.
         * @return False when the file is missing or will not read.
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

    } // namespace

    PassDesc TonemapPass::declare() {
        static constexpr std::array<ResourceRead, 1> kReads{ {
            { kSceneColor, gfx::ResourceState::ShaderRead },
        } };
        // The swapchain image, which this pass is the only writer of.
        //
        // The depth image is here because the rendering scope attaches it and
        // clears it, not because anything draws depth-tested. Declaring it is
        // what orders that clear against the mesh pass writing the same image.
        // Issue #143 takes the attachment away, which takes this line with it.
        static constexpr std::array<ResourceWrite, 2> kWrites{ {
            { kFrameColor, gfx::ResourceState::ColorTarget },
            { kFrameDepth, gfx::ResourceState::DepthTarget },
        } };
        return PassDesc{ .name = "tonemap", .reads = kReads, .writes = kWrites };
    }

    bool TonemapPass::build_target(gfx::Extent2D extent) {
        const gfx::ColorTargetDesc desc{
            .width = extent.width,
            .height = extent.height,
            .format = gfx::ColorTargetFormat::RGBA16F,
            .sampler = { .filter = gfx::Filter::Linear,
                         .address = gfx::AddressMode::ClampToEdge },
        };
        if (!gfx::succeeded(gfx::create_color_target(device_, desc, &target_))) {
            ENGINE_LOG_ERROR("The scene color target was not created at {}x{}.", extent.width,
                             extent.height);
            return false;
        }
        return true;
    }

    bool TonemapPass::build_pipeline(const assets::Content& content, gfx::PipelineHandle& out) {
        assets::Shader vertex;
        assets::Shader fragment;
        if (!read_one_shader(content, "tonemap.vert", vertex) ||
            !read_one_shader(content, "tonemap.frag", fragment)) {
            return false;
        }

        std::vector<gfx::DescriptorBinding> bindings;
        if (!merge_bindings(vertex, fragment, bindings)) {
            return false;
        }

        const gfx::GraphicsPipelineDesc desc{
            .vertex = { .spirv = vertex.spirv.data(), .word_count = vertex.spirv.size() },
            .fragment = { .spirv = fragment.spirv.data(), .word_count = fragment.spirv.size() },
            // No vertex buffer. tonemap.vert builds its positions from the
            // vertex index, so there is nothing to bind and nothing to upload.
            .attributes = nullptr,
            .attribute_count = 0,
            .vertex_stride = 0,
            .push_constant_size = 0,
            .bindings = bindings.data(),
            .binding_count = bindings.size(),
            // The triangle covers everything, so there is nothing to test
            // against and nothing worth recording.
            .depth_test = false,
            .depth_write = false,
            .blend = false,
            // A full screen triangle has one winding and it is whichever the
            // index arithmetic gives. Culling nothing is what keeps that from
            // mattering.
            .cull_back = false,
            .depth_only = false,
            // The swapchain, because this is the final write of the frame.
            .color_format = gfx::ColorTargetFormat::Swapchain,
        };

        const gfx::Result result = gfx::create_graphics_pipeline(device_, desc, &out);
        if (!gfx::succeeded(result)) {
            ENGINE_LOG_ERROR("The tonemap pipeline did not build: {}", gfx::result_name(result));
            return false;
        }
        return true;
    }

    bool TonemapPass::build_set() {
        const std::array<gfx::DescriptorWrite, 1> writes{ {
            { .binding = 0,
              .kind = gfx::DescriptorKind::CombinedImageSampler,
              .texture = target_,
              .buffer = {} },
        } };
        if (!gfx::succeeded(gfx::create_descriptor_set(device_, pipeline_, kSceneSet,
                                                       writes.data(), writes.size(), &set_))) {
            ENGINE_LOG_ERROR("The tonemap descriptor set could not be built.");
            return false;
        }
        return true;
    }

    bool TonemapPass::create(gfx::Device* device, const assets::Content& content,
                             gfx::Extent2D extent) {
        ENGINE_ASSERT(device != nullptr, "TonemapPass::create needs a device.");
        device_ = device;

        if (!build_target(extent)) {
            return false;
        }
        if (!build_pipeline(content, pipeline_)) {
            ENGINE_LOG_ERROR("The tonemap pipeline did not build.");
            return false;
        }
        return build_set();
    }

    void TonemapPass::destroy() {
        if (device_ == nullptr) {
            return;
        }
        gfx::destroy_descriptor_set(device_, set_);
        set_ = gfx::DescriptorSetHandle{};
        gfx::destroy_pipeline(device_, pipeline_);
        pipeline_ = gfx::PipelineHandle{};
        gfx::destroy_texture(device_, target_);
        target_ = gfx::TextureHandle{};
        device_ = nullptr;
    }

    bool TonemapPass::resize(gfx::Extent2D extent) {
        if (device_ == nullptr) {
            return false;
        }

        // A frame in flight may still be reading the old image or the set that
        // names it. A resize already stalls, because the swapchain is rebuilt in
        // the same breath, so one more wait costs nothing anybody can see.
        gfx::device_wait_idle(device_);
        gfx::destroy_descriptor_set(device_, set_);
        set_ = gfx::DescriptorSetHandle{};
        gfx::destroy_texture(device_, target_);
        target_ = gfx::TextureHandle{};

        return build_target(extent) && build_set();
    }

    bool TonemapPass::reload_shaders(const assets::Content& content) {
        if (device_ == nullptr) {
            return false;
        }

        gfx::PipelineHandle rebuilt;
        if (!build_pipeline(content, rebuilt)) {
            ENGINE_LOG_WARN("The tonemap shader would not build. Keeping the old pipeline.");
            return false;
        }

        gfx::device_wait_idle(device_);
        gfx::destroy_pipeline(device_, pipeline_);
        pipeline_ = rebuilt;

        // A set is allocated against the layout of a pipeline, so the old one
        // cannot be bound to the new pipeline even though it names the same
        // image.
        gfx::destroy_descriptor_set(device_, set_);
        set_ = gfx::DescriptorSetHandle{};
        if (!build_set()) {
            ENGINE_LOG_ERROR("The tonemap set did not rebuild, so the frame cannot be written.");
            return false;
        }
        ENGINE_LOG_INFO("The tonemap pipeline was rebuilt.");
        return true;
    }

    void TonemapPass::draw(gfx::CommandList* commands) {
        if (!pipeline_.valid() || !set_.valid()) {
            return;
        }
        gfx::cmd_bind_pipeline(commands, pipeline_);
        gfx::cmd_bind_descriptor_set(commands, pipeline_, kSceneSet, set_);
        gfx::cmd_draw(commands, kFullScreenVertices, 1, 0, 0);
    }

} // namespace engine::render
