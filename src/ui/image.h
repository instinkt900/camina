/**
 * @file image.h
 * @brief The moth_ui image, backed by a cooked engine texture.
 */

#pragma once

#include "assets/content.h"
#include "gfx/device.h"
#include "render/texture_cache.h"

#include <moth_ui/asset_id.h>
#include <moth_ui/graphics/iimage.h>
#include <moth_ui/iimage_factory.h>

#include <filesystem>
#include <memory>
#include <span>

namespace engine::ui {

    /**
     * @brief Turns the identity a layout stored into the key the manifest holds.
     *
     * A layout names an image with a `moth_ui::AssetId`, and moth_ui never
     * reads it. This engine reads it as a source path relative to the game
     * content root, which is the string `assets::Content::find` takes.
     *
     * The identity arrives exactly as somebody authored it, because moth_ui no
     * longer rewrites a stored path. So this normalizes the separators and
     * refuses a path that climbs out of the content tree.
     *
     * `DESIGN.md` section 8.4 records why the identity is an `AssetId` rather
     * than a path, and section 10 M10 records what it cost.
     *
     * @param id The identity a layout stored.
     * @return A path with forward slashes, relative to the content root. Empty
     * when the identity is empty or climbs out of the tree.
     */
    [[nodiscard]] std::string manifest_key_for(const moth_ui::AssetId& id);

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
     * @brief Turns the identity in a moth_ui layout into a cooked engine texture.
     *
     * **A layout carries an identity, and this engine reads it as a source
     * path.** `moth_ui::AssetId` holds whatever the consumer put in it and
     * moth_ui never looks inside, so the meaning lives here. `DESIGN.md`
     * section 8.4 records why moth_ui learned an identity type rather than
     * keeping a path.
     *
     * The identity a layout stores is a source path, relative to the game
     * content root, with forward slashes. It is the same string
     * `assets::Content::find` takes, so `ui/panel.png` names the file at
     * `sandbox/content/ui/panel.png`. moth_ui carries that string and never
     * reads it, so the meaning is this engine's alone.
     *
     * **This owns a texture cache of its own rather than sharing the one inside
     * `render::MeshPass`, and M10.4 is what settled that.** A UI image and a
     * material texture upload twice when a scene names one file for both, which
     * no scene does today. That cost is paid on purpose.
     *
     * Sharing one cache would mean a mesh reload frees a texture a layout still
     * points at. Before M10.4 nothing dropped, so the separation cost a
     * duplicate upload and bought nothing measurable. Now both sides drop, and
     * each knows only its own holders: `MeshPass::reload` cannot tell `UiPass`
     * to forget a descriptor set, and it cannot tell a `moth_ui::NodeImage` to
     * ask again. One shared cache would need one reload path that knows every
     * holder on both sides, which is a larger thing than a second upload of an
     * image no scene shares.
     *
     * @code
     * engine::ui::ImageFactory factory;
     * if (factory.create(device, &content)) {
     *     auto image = factory.GetImage(moth_ui::AssetId{ "ui/panel.png" });
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

        /**
         * @brief Frees every image an identity in @p changed names.
         *
         * M10.4. A UI image used to load once and keep its texture for the
         * whole run, so editing the source showed nothing until a restart. This
         * is what hot reload calls.
         *
         * It waits for the frames in flight before it frees anything, the way
         * `render::MeshPass::reload` does. A frame the GPU has not finished may
         * still read the texture about to go.
         *
         * **Dropping the texture is only half of it.** A `moth_ui::NodeImage`
         * holds the image it was given, with the handle inside, and
         * `engine::ui::UiPass` holds a descriptor set naming that handle. Both
         * have to be told, which is why this reports whether anything was
         * really let go rather than returning void.
         *
         * @param changed The identities that were cooked again.
         * @return True when at least one image this held was freed. The caller
         * must then call `UiPass::forget_sets` and reload its node tree.
         */
        [[nodiscard]] bool reload(std::span<const Guid> changed);

        /// @cond
        // This implements moth_ui::IImageFactory. See the note on Image above
        // for why the override carries no Doxygen block.
        std::unique_ptr<moth_ui::IImage> GetImage(const moth_ui::AssetId& id) override;
        /// @endcond

    private:
        gfx::Device* device_ = nullptr;
        const assets::Content* content_ = nullptr;
        render::TextureCache textures_;
    };

}
