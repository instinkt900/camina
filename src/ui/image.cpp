#include "ui/image.h"

#include "core/guid.h"
#include "core/log.h"

#include <string>
#include <system_error>

namespace engine::ui {

    std::string manifest_key_for(const moth_ui::AssetId& id) {
        // A path that climbs out of the content tree is not a cooked asset, and
        // returning it would send "../.." to the manifest as if it were a key.
        //
        // The test is the first component and not the first two characters. A
        // file may legitimately be called "..panel.png", and a string compare
        // would refuse it.
        const auto escapes = [](const std::filesystem::path& candidate) {
            return !candidate.empty() && *candidate.begin() == std::filesystem::path{ ".." };
        };

        // Normalize first, so "ui/../panel.png" reaches the manifest as
        // "panel.png". The manifest holds the path the cooker walked, which
        // never has a "." or a ".." in it.
        const std::filesystem::path normalized = id.path().lexically_normal();

        // An absolute identity names a file outside the content tree, and the
        // manifest is keyed on a relative source path. moth_ui used to make
        // every stored path absolute, and undoing that here was the whole job
        // of the function this replaced. It does not any more, so an absolute
        // identity now means somebody authored one.
        if (normalized.empty() || normalized.is_absolute() || escapes(normalized)) {
            return {};
        }

        // generic_string() is what makes a layout authored on Windows find the
        // same asset on Linux.
        return normalized.generic_string();
    }

    Image::Image(gfx::TextureHandle texture, int width, int height)
        : texture_(texture)
        , width_(width)
        , height_(height) {
    }

    moth_ui::IntVec2 Image::GetDimensions() const {
        return moth_ui::IntVec2{ width_, height_ };
    }

    ImageFactory::~ImageFactory() {
        // render::MeshPass does the same. Without it, the render::TextureCache
        // inside asserts that somebody forgot, which reports the mistake and
        // still leaks the textures.
        destroy();
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

    std::unique_ptr<moth_ui::IImage> ImageFactory::GetImage(const moth_ui::AssetId& id) {
        if (device_ == nullptr || content_ == nullptr) {
            ENGINE_LOG_ERROR("A layout asked for an image before the factory was created.");
            return nullptr;
        }

        // A cooked layout names an image by GUID, because the layout rule
        // resolved the authored path when it cooked. A caller inside the engine
        // names one by source path, because a person writing C++ cannot know a
        // derived identity. Both forms arrive here, so both are read.
        Guid guid;
        if (!Guid::parse(id.str(), guid)) {
            const std::string source = manifest_key_for(id);
            if (source.empty()) {
                ENGINE_LOG_ERROR("The layout names the image '{}', which is neither an "
                                 "identity nor a source path inside the game content tree "
                                 "at {}.",
                                 id.str(), content_->root().generic_string());
                return nullptr;
            }
            const assets::ManifestEntry* entry = content_->find(source);
            if (entry == nullptr) {
                ENGINE_LOG_ERROR("The layout names the image {}, which the cooked content "
                                 "tree does not hold. The path is relative to the game "
                                 "content root.",
                                 source);
                return nullptr;
            }
            guid = entry->guid;
        }

        const render::TextureInfo info = textures_.get_info(device_, *content_, guid);

        // get_info() answers with the fallback rather than a failure, because a
        // material that names a missing texture must still draw. A layout must
        // not: moth_ui reads the size to lay the node out, and a single white
        // texel would place it at one pixel square. So a miss is nullptr here,
        // which is what IImageFactory documents and what NodeImage checks.
        if (info.texture.value == textures_.fallback().value) {
            ENGINE_LOG_ERROR("The image {} is in the manifest but it would not load.",
                             id.str());
            return nullptr;
        }

        return std::make_unique<Image>(info.texture, static_cast<int>(info.width),
                                       static_cast<int>(info.height));
    }

}
