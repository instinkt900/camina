/**
 * @file image.h
 * @brief The moth_ui image, backed by a cooked engine texture.
 */

#pragma once

#include "assets/content.h"
#include "gfx/device.h"
#include "render/texture_cache.h"

#include <moth_ui/graphics/iimage.h>
#include <moth_ui/iimage_factory.h>

#include <filesystem>
#include <memory>

namespace engine::ui {

    /**
     * @brief Turns the path a layout gave into the source path the manifest holds.
     *
     * **moth_ui hands a backend an absolute path.** `LayoutEntityImage`
     * deserializes `imagePath` as
     * `std::filesystem::absolute(layout directory / stored path)`, so a layout
     * that stores `panel.png` produces an absolute path into the cooked tree.
     * The engine names an asset by a source path, and the manifest is keyed on
     * one, so an absolute path matches nothing.
     *
     * This undoes that. The cooked tree mirrors the source tree, so a cooked
     * path made relative to the cooked root is the source path again. That
     * holds even when the cooker renames the output, because the manifest is
     * keyed on the source and `ui/panel.png` cooks to `ui/panel.png.tex`.
     *
     * A path that is already relative passes through, which is what a caller
     * outside a layout gives.
     *
     * `DESIGN.md` section 8.4 records which side this belongs on and why the
     * fix landed here rather than in moth_ui.
     *
     * @param path The path from the layout. Absolute or relative.
     * @param cooked_root The directory `assets::Content` was opened on.
     * @return A path with forward slashes, relative to the content root. Empty
     * when an absolute path falls outside the cooked tree.
     */
    [[nodiscard]] std::string source_path_for(const std::filesystem::path& path,
                                              const std::filesystem::path& cooked_root);

    /**
     * @brief One cooked texture, as moth_ui sees it.
     *
     * moth_ui asks an image for its size and passes it back to the renderer.
     * It never learns what is behind it, so the texture handle is here for
     * `Renderer::RenderImage` to read after it casts back.
     *
     * The size is the size of the whole texture, not of any part of it. A
     * moth_ui source rectangle is in texels of this size, and that is what the
     * renderer divides by to get texture coordinates.
     */
    class Image final : public moth_ui::IImage {
    public:
        /**
         * @brief Wraps a texture the cache already loaded.
         *
         * @param texture The device texture. Held, not owned. The cache that
         * loaded it owns it and outlives this.
         * @param width Width in texels.
         * @param height Height in texels.
         */
        Image(gfx::TextureHandle texture, int width, int height);

        /// @cond
        // These implement moth_ui::IImage, and moth_ui documents the contract.
        // Doxygen reads src/ only, so it cannot see that base class and reports
        // every override as undocumented.
        [[nodiscard]] int GetWidth() const override { return width_; }
        [[nodiscard]] int GetHeight() const override { return height_; }
        [[nodiscard]] moth_ui::IntVec2 GetDimensions() const override;
        /// @endcond

        /**
         * @brief The texture the renderer binds for this image.
         *
         * @return The device texture. It stays valid while the factory that
         * made this image is alive.
         */
        [[nodiscard]] gfx::TextureHandle texture() const { return texture_; }

    private:
        gfx::TextureHandle texture_;
        int width_ = 0;
        int height_ = 0;
    };

    /**
     * @brief Turns a path in a moth_ui layout into a cooked engine texture.
     *
     * **An image identity is a source path, and the engine resolves it.** A
     * moth_ui layout names an image by `std::filesystem::path`, and the engine
     * names every asset by GUID. This resolves the path against the cooked
     * manifest, so moth_ui does not change. `DESIGN.md` section 8.4 records why
     * that side was picked and what would change it.
     *
     * The path is a source path, relative to the game content root, with
     * forward slashes. It is the same string `assets::Content::find` takes, so
     * `ui/panel.png` names the file at `sandbox/content/ui/panel.png`.
     *
     * This owns a texture cache of its own rather than sharing the one inside
     * `render::MeshPass`. A UI image and a material texture then upload twice
     * when a scene uses one file for both, which no scene does. In return, a
     * mesh reload cannot free a texture a layout still points at.
     *
     * @code
     * engine::ui::ImageFactory factory;
     * if (factory.create(device, &content)) {
     *     std::unique_ptr<moth_ui::IImage> image = factory.GetImage("ui/panel.png");
     * }
     * @endcode
     *
     * @warning Call destroy() before the device goes away. The textures are
     *          device resources and nothing else frees them.
     */
    class ImageFactory final : public moth_ui::IImageFactory {
    public:
        ImageFactory() = default;

        ImageFactory(const ImageFactory&) = delete;
        ImageFactory& operator=(const ImageFactory&) = delete;
        ImageFactory(ImageFactory&&) = delete;
        ImageFactory& operator=(ImageFactory&&) = delete;
        /// @brief Frees every texture, the way render::MeshPass does.
        ~ImageFactory() final;

        /**
         * @brief Opens the factory on a device and a cooked content tree.
         *
         * @param device The device that uploads the textures. Held, not owned.
         * @param content The cooked game content. Held, not owned, and it must
         * outlive this.
         * @return False when the texture cache could not build its fallback.
         */
        [[nodiscard]] bool create(gfx::Device* device, const assets::Content* content);

        /// @brief Frees every texture this loaded. Safe to call twice.
        void destroy();

        /// @cond
        // This implements moth_ui::IImageFactory. See the note on Image above
        // for why the override carries no Doxygen block.
        std::unique_ptr<moth_ui::IImage> GetImage(const std::filesystem::path& path) override;
        /// @endcond

    private:
        gfx::Device* device_ = nullptr;
        const assets::Content* content_ = nullptr;
        render::TextureCache textures_;
    };

}
