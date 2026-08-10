#include "ui/image.h"

#include "core/log.h"

#include <string>

namespace engine::ui {

    Image::Image(gfx::TextureHandle texture, int width, int height)
        : texture_(texture)
        , width_(width)
        , height_(height) {
    }

    moth_ui::IntVec2 Image::GetDimensions() const {
        return moth_ui::IntVec2{ width_, height_ };
    }

    bool ImageFactory::create(gfx::Device* device, const assets::Content* content) {
        device_ = device;
        content_ = content;
        if (!textures_.create(device)) {
            device_ = nullptr;
            content_ = nullptr;
            return false;
        }
        return true;
    }

    void ImageFactory::destroy() {
        textures_.destroy(device_);
        device_ = nullptr;
        content_ = nullptr;
    }

    std::unique_ptr<moth_ui::IImage> ImageFactory::GetImage(const std::filesystem::path& path) {
        if (device_ == nullptr || content_ == nullptr) {
            ENGINE_LOG_ERROR("A layout asked for an image before the factory was created.");
            return nullptr;
        }

        // The layout stores a source path and the manifest is keyed on one, so
        // this is the whole of the resolution. generic_string() is what makes a
        // layout authored on Windows find the same asset on Linux.
        const std::string source = path.generic_string();
        const assets::ManifestEntry* entry = content_->find(source);
        if (entry == nullptr) {
            ENGINE_LOG_ERROR("The layout names the image {}, which the cooked content tree "
                             "does not hold. The path is relative to the game content root.",
                             source);
            return nullptr;
        }

        const render::TextureInfo info = textures_.get_info(device_, *content_, entry->guid);

        // get_info() answers with the fallback rather than a failure, because a
        // material that names a missing texture must still draw. A layout must
        // not: moth_ui reads the size to lay the node out, and a single white
        // texel would place it at one pixel square. So a miss is nullptr here,
        // which is what IImageFactory documents and what NodeImage checks.
        if (info.texture.value == textures_.fallback().value) {
            ENGINE_LOG_ERROR("The image {} is in the manifest but it would not load.", source);
            return nullptr;
        }

        return std::make_unique<Image>(info.texture, static_cast<int>(info.width),
                                       static_cast<int>(info.height));
    }

}
