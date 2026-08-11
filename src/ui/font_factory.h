#pragma once

/**
 * @file
 * @brief Turns a font name in a moth_ui layout into a loaded engine::ui::Font.
 */

#include "assets/content.h"
#include "gfx/device.h"
#include "ui/font.h"

#include <moth_ui/ifont_factory.h>

#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace engine::ui {

    /**
     * @brief The fonts a layout can name, loaded from the cooked content tree.
     *
     * moth_ui names a font by a string and a size, and it expects the factory
     * to hold a name-to-file map. `AddFont` fills that map, and the path is a
     * source path relative to the game content root, with forward slashes. It
     * is the same string `assets::Content::find` takes, so
     * `ui/fonts/LiberationSans-Regular.ttf` names the file at
     * `sandbox/content/ui/fonts/LiberationSans-Regular.ttf`.
     *
     * That matches `engine::ui::ImageFactory`, and `DESIGN.md` section 8.4
     * records why the engine resolves the path rather than moth_ui carrying a
     * GUID.
     *
     * A font is loaded once for each name and size, and shared after that. A
     * size is part of the key because the atlas holds rasterized glyphs, so two
     * sizes are two atlases. Issue #214 holds what that costs.
     *
     * @code
     * engine::ui::FontFactory fonts;
     * if (fonts.create(device, &content)) {
     *     fonts.AddFont("body", "ui/fonts/LiberationSans-Regular.ttf");
     *     std::shared_ptr<moth_ui::IFont> font = fonts.GetFont("body", 32);
     * }
     * @endcode
     *
     * @warning Call destroy() before the device goes away. Each atlas is a
     *          device resource and nothing else frees it.
     */
    class FontFactory final : public moth_ui::IFontFactory {
    public:
        FontFactory() = default;

        FontFactory(const FontFactory&) = delete;
        FontFactory& operator=(const FontFactory&) = delete;
        FontFactory(FontFactory&&) = delete;
        FontFactory& operator=(FontFactory&&) = delete;
        /// @brief Frees every atlas, the way engine::ui::ImageFactory does.
        ~FontFactory() final;

        /**
         * @brief Opens the factory on a device and a cooked content tree.
         *
         * @param device The device that holds each atlas. Held, not owned.
         * @param content The cooked game content. Held, not owned, and it must
         * outlive this.
         * @return False when FreeType would not start.
         */
        [[nodiscard]] bool create(gfx::Device* device, const assets::Content* content);

        /// @brief Frees every font this loaded. Safe to call twice.
        void destroy();

        /// @cond
        // These implement moth_ui::IFontFactory, and moth_ui documents the
        // contract. Doxygen reads src/ only, so it cannot see that base class
        // and reports every override as undocumented.
        void AddFont(const std::string& name, const std::filesystem::path& path) override;
        void RemoveFont(const std::string& name) override;
        void ClearFonts() override;
        [[nodiscard]] std::vector<std::string> GetFontNameList() const override;
        [[nodiscard]] std::filesystem::path GetFontPath(const std::string& name) const override;
        [[nodiscard]] std::shared_ptr<moth_ui::IFont> GetFont(const std::string& name,
                                                              int size) override;
        [[nodiscard]] std::shared_ptr<moth_ui::IFont> GetDefaultFont(int size) override;

        // These three serve moth_editor, which keeps its font list in a project
        // file. A runtime takes its fonts from the cooked content tree, so
        // reading or writing a project file here would be a second source of
        // truth. They assert rather than fail quietly, because a caller that
        // reaches one has made a mistake this cannot repair. Issue #200 records
        // that an interface forcing a runtime to implement editor project files
        // is a seam in the wrong place.
        void LoadProject(const std::filesystem::path& path) override;
        void SaveProject(const std::filesystem::path& path) override;
        [[nodiscard]] std::filesystem::path GetCurrentProjectPath() const override;
        /// @endcond

    private:
        // One loaded face at one size. The name and the size together are the
        // key, because an atlas holds glyphs rasterized at one size.
        struct Key {
            std::string name;
            int size = 0;

            bool operator<(const Key& other) const {
                return (name != other.name) ? (name < other.name) : (size < other.size);
            }
        };

        gfx::Device* device_ = nullptr;
        const assets::Content* content_ = nullptr;
        FontLibrary library_;

        std::map<std::string, std::filesystem::path> paths_;
        std::map<Key, std::shared_ptr<Font>> loaded_;
    };

}
