#pragma once

/**
 * @file
 * @brief Cooked textures, uploaded once and bound many times.
 *
 * A material names a texture by GUID, and several materials name the same one.
 * This turns that GUID into a device texture, and it uploads each one once
 * however many materials use it.
 *
 * It also holds the fallback, which is a single white texel. Every draw call
 * has to bind something, because the pipeline declares a sampler and a
 * descriptor set that is never written is undefined. So a material with no base
 * color, and a material whose texture will not load, both bind this.
 */

#include "assets/content.h"
#include "core/guid.h"
#include "gfx/device.h"

#include <cstddef>
#include <map>

namespace engine::render {

    /**
     * @brief Holds every texture the materials asked for.
     *
     * @code
     * engine::render::TextureCache cache;
     * if (cache.create(device)) {
     *     const auto texture = cache.get(device, content, guid);
     * }
     * @endcode
     */
    class TextureCache {
    public:
        /// @brief Frees nothing. Call destroy() before this runs.
        ~TextureCache();

        TextureCache() = default;
        TextureCache(const TextureCache&) = delete;
        TextureCache& operator=(const TextureCache&) = delete;
        TextureCache(TextureCache&&) = delete;
        TextureCache& operator=(TextureCache&&) = delete;

        /**
         * @brief Builds the fallback texture.
         *
         * Call this once, before the first get(). It is separate from the
         * constructor because it needs a device and it can fail.
         *
         * @param device The device that owns the textures.
         * @return True when the fallback was made.
         */
        [[nodiscard]] bool create(gfx::Device* device);

        /**
         * @brief Finds a texture, and uploads it the first time it is asked for.
         *
         * A GUID that will not load is remembered as a failure, so a material
         * that names a missing texture reports once rather than on every frame.
         *
         * @param device The device that owns the textures.
         * @param content The cooked content to read from.
         * @param guid The texture identity, as a material stores it. A null GUID
         * is not an error and gives the fallback.
         * @return The texture, or the fallback when there is nothing to load.
         */
        [[nodiscard]] gfx::TextureHandle get(gfx::Device* device,
                                             const assets::Content& content, Guid guid);

        /// @brief The single white texel every unresolved reference binds.
        /// @return The fallback, which is null until create() has run.
        [[nodiscard]] gfx::TextureHandle fallback() const { return fallback_; }

        /**
         * @brief Frees one texture, so the next get() uploads it again.
         *
         * This is what hot reload calls. It also forgets a remembered failure,
         * because the whole point of a reload is that a texture which would not
         * load before may load now.
         *
         * The fallback is never dropped. It belongs to no source file, so
         * nothing can change it.
         *
         * @param device The device that owns the textures.
         * @param guid The texture to let go of. One that is not loaded is not
         * an error and does nothing.
         *
         * @warning The texture goes straight back to the device. The caller
         * must already have waited for the frames in flight, because a frame
         * the GPU has not finished may still read it.
         */
        void drop(gfx::Device* device, Guid guid);

        /**
         * @brief Frees every texture, the fallback included.
         * @param device The device that owns them.
         */
        void destroy(gfx::Device* device);

        /// @brief How many textures are loaded.
        /// @return The count, the fallback and the failures not included.
        [[nodiscard]] std::size_t size() const { return loaded_.size(); }

    private:
        std::map<Guid, gfx::TextureHandle> loaded_;
        /// The GUIDs that failed, so one bad reference reports once.
        std::map<Guid, bool> failed_;
        gfx::TextureHandle fallback_;
    };

} // namespace engine::render
