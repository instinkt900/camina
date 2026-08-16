#include "editor/viewport.h"

#include "core/assert.h"
#include "core/log.h"

#include <algorithm>

namespace engine::editor {

    namespace {

        /// Clamps a panel size to what the frame depth image can cover, and to
        /// at least one texel in each direction.
        [[nodiscard]] gfx::Extent2D clamp_to(gfx::Extent2D wanted, gfx::Extent2D limit) {
            return gfx::Extent2D{
                std::clamp(wanted.width, 1U, std::max(limit.width, 1U)),
                std::clamp(wanted.height, 1U, std::max(limit.height, 1U)),
            };
        }

    } // namespace

    bool Viewport::build(gfx::Extent2D extent) {
        const gfx::ColorTargetDesc desc{
            .width = extent.width,
            .height = extent.height,
            // The swapchain format, because the tonemap pass writes this and
            // its pipeline declares that format. It is 8-bit sRGB, which is
            // right: the curve has already run by the time anything lands here.
            .format = gfx::ColorTargetFormat::Swapchain,
            .sampler = { .filter = gfx::Filter::Linear,
                         .address = gfx::AddressMode::ClampToEdge },
        };
        if (!gfx::succeeded(gfx::create_color_target(device_, desc, &target_))) {
            ENGINE_LOG_ERROR("The viewport target was not created at {}x{}.", extent.width,
                             extent.height);
            return false;
        }

        picture_ = gfx::imgui_texture_id(device_, target_);
        if (picture_ == gfx::kInvalidImGuiTexture) {
            // The image is real and nothing can show it, which is worse than no
            // image at all: the frame would render into something invisible.
            gfx::destroy_texture(device_, target_);
            target_ = gfx::TextureHandle{};
            return false;
        }

        extent_ = extent;
        return true;
    }

    void Viewport::release() {
        // The binding first. It names the image, and the overlay holds the
        // descriptor until this returns it.
        gfx::imgui_release_texture(picture_);
        picture_ = gfx::kInvalidImGuiTexture;
        gfx::destroy_texture(device_, target_);
        target_ = gfx::TextureHandle{};
        extent_ = gfx::Extent2D{};
    }

    bool Viewport::create(gfx::Device* device, gfx::Extent2D extent, gfx::Extent2D limit) {
        ENGINE_CHECK(device != nullptr, "Viewport::create needs a device.");
        device_ = device;
        return build(clamp_to(extent, limit));
    }

    void Viewport::destroy() {
        if (device_ == nullptr) {
            return;
        }
        gfx::device_wait_idle(device_);
        release();
        device_ = nullptr;
    }

    ViewportChange Viewport::ensure(gfx::Extent2D wanted, gfx::Extent2D limit) {
        if (device_ == nullptr || wanted.width == 0 || wanted.height == 0) {
            return ViewportChange::None;
        }

        const gfx::Extent2D target = clamp_to(wanted, limit);
        if (ready() && target.width == extent_.width && target.height == extent_.height) {
            return ViewportChange::None;
        }

        // A frame in flight may still be reading the old image, and the overlay
        // may still be holding its binding, so both have to be idle before
        // either goes. This is why a rebuild belongs at the top of a frame
        // rather than in the middle of one.
        gfx::device_wait_idle(device_);
        release();

        if (!build(target)) {
            ENGINE_LOG_ERROR("The viewport target did not rebuild at {}x{}, so the panel has "
                             "nothing to show.",
                             target.width, target.height);
            return ViewportChange::Failed;
        }
        return ViewportChange::Rebuilt;
    }

} // namespace engine::editor
