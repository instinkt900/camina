#include "ui/font_factory.h"

#include "core/assert.h"
#include "core/log.h"

#include <utility>

namespace engine::ui {

    FontFactory::~FontFactory() {
        // engine::ui::ImageFactory does the same. Without it, every atlas
        // texture leaks and nothing reports it.
        destroy();
    }

    bool FontFactory::create(gfx::Device* device, const assets::Content* content) {
        device_ = device;
        content_ = content;
        if (!library_.create()) {
            device_ = nullptr;
            content_ = nullptr;
            return false;
        }
        return true;
    }

    void FontFactory::destroy() {
        for (auto& [key, font] : loaded_) {
            if (font) {
                font->destroy(device_);
            }
        }
        loaded_.clear();
        paths_.clear();
        // The library goes last. FreeType owns the faces, and a face outliving
        // its library is undefined.
        library_.destroy();
        device_ = nullptr;
        content_ = nullptr;
    }

    void FontFactory::AddFont(const std::string& name, const std::filesystem::path& path) {
        // generic_string() is what makes a layout authored on Windows find the
        // same asset on Linux, the same way engine::ui::ImageFactory does.
        paths_[name] = path.generic_string();
    }

    void FontFactory::RemoveFont(const std::string& name) {
        paths_.erase(name);

        // Every size loaded under this name goes with it. Leaving them behind
        // would keep an atlas alive that nothing can name any more.
        for (auto it = loaded_.begin(); it != loaded_.end();) {
            if (it->first.name == name) {
                if (it->second) {
                    it->second->destroy(device_);
                }
                it = loaded_.erase(it);
            } else {
                ++it;
            }
        }
    }

    void FontFactory::ClearFonts() {
        for (auto& [key, font] : loaded_) {
            if (font) {
                font->destroy(device_);
            }
        }
        loaded_.clear();
        paths_.clear();
    }

    std::vector<std::string> FontFactory::GetFontNameList() const {
        std::vector<std::string> names;
        names.reserve(paths_.size());
        for (const auto& [name, path] : paths_) {
            names.push_back(name);
        }
        return names;
    }

    std::filesystem::path FontFactory::GetFontPath(const std::string& name) const {
        const auto found = paths_.find(name);
        return (found == paths_.end()) ? std::filesystem::path{} : found->second;
    }

    std::shared_ptr<moth_ui::IFont> FontFactory::GetFont(const std::string& name, int size) {
        if (device_ == nullptr || content_ == nullptr) {
            ENGINE_LOG_ERROR("A layout asked for a font before the factory was created.");
            return nullptr;
        }
        if (size <= 0) {
            ENGINE_LOG_ERROR("A layout asked for the font {} at {} pixels.", name, size);
            return nullptr;
        }

        const Key key{ .name = name, .size = size };
        const auto cached = loaded_.find(key);
        if (cached != loaded_.end()) {
            return cached->second;
        }

        const auto path = paths_.find(name);
        if (path == paths_.end()) {
            ENGINE_LOG_ERROR("A layout names the font {}, which nothing registered. Call "
                             "AddFont before a layout loads.",
                             name);
            return nullptr;
        }

        // The cooker has no rule for a font file, so it copies one unchanged.
        // The manifest still records it, which is what turns the source path a
        // layout stores into the file on disk.
        const std::string source = path->second.generic_string();
        const assets::ManifestEntry* entry = content_->find(source);
        if (entry == nullptr) {
            ENGINE_LOG_ERROR("The font {} names {}, which the cooked content tree does not "
                             "hold. The path is relative to the game content root.",
                             name, source);
            return nullptr;
        }
        if (entry->outputs.size() != 1) {
            // A copied file gives one output. Several would mean a rule started
            // cooking fonts, and then this needs to say which output it wants.
            ENGINE_LOG_ERROR("The font {} cooked to {} outputs, and this expects one.", source,
                             entry->outputs.size());
            return nullptr;
        }

        // FreeType reads the file itself rather than taking the bytes through
        // Content. FT_New_Memory_Face does not copy what it is given, so the
        // caller would have to keep the whole file alive beside the atlas for
        // as long as HarfBuzz holds the face.
        auto font = std::make_shared<Font>();
        if (!font->load(library_, content_->root() / entry->outputs.front().cooked, size)) {
            return nullptr;
        }
        if (!font->upload(device_)) {
            return nullptr;
        }

        // Cache the failure-free result only. A font that would not load is not
        // remembered, so a later cook that fixes it is picked up.
        loaded_.emplace(key, font);
        return font;
    }

    std::shared_ptr<moth_ui::IFont> FontFactory::GetDefaultFont(int size) {
        if (paths_.empty()) {
            ENGINE_LOG_ERROR("A layout asked for the default font and nothing is registered.");
            return nullptr;
        }
        // The first name in order. moth_ui::FontFactory picks the same one, and
        // there is nothing better to pick: the interface carries no way to say
        // which font is the default.
        return GetFont(paths_.begin()->first, size);
    }

    void FontFactory::LoadProject(const std::filesystem::path& path) {
        (void)path;
        ENGINE_ASSERT(false, "A runtime font factory has no project file. It takes its fonts "
                             "from the cooked content tree, and a project file would be a "
                             "second source of truth. See issue #200.");
    }

    void FontFactory::SaveProject(const std::filesystem::path& path) {
        (void)path;
        ENGINE_ASSERT(false, "A runtime font factory has no project file to save. See issue "
                             "#200.");
    }

    std::filesystem::path FontFactory::GetCurrentProjectPath() const {
        ENGINE_ASSERT(false, "A runtime font factory has no project file. See issue #200.");
        return {};
    }

}
