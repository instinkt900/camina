#pragma once

/**
 * @file
 * @brief Cooked materials, read once and bound many times.
 *
 * A submesh names a material by GUID, and several submeshes name the same one.
 * This reads that material once and resolves the textures it names through a
 * TextureCache.
 *
 * The renderer binds the base color today. The cooked material carries the
 * whole glTF metallic-roughness set, and M5 is the milestone that shades with
 * the rest. See DESIGN.md section 10.
 */

#include "assets/content.h"
#include "assets/material.h"
#include "core/guid.h"
#include "gfx/device.h"
#include "render/texture_cache.h"

#include <cstddef>
#include <map>

namespace engine::render {

    /// @brief One cooked material, with its textures resolved.
    struct GpuMaterial {
        assets::Material source;       ///< Every field the cooked file held.
        gfx::TextureHandle base_color; ///< The base color, or the fallback texel.
    };

    /**
     * @brief Holds every material the submeshes asked for.
     *
     * This owns nothing on the device. The textures belong to the TextureCache
     * that resolved them, so destroy() only drops what it remembers.
     */
    class MaterialCache {
    public:
        MaterialCache() = default;
        MaterialCache(const MaterialCache&) = delete;
        MaterialCache& operator=(const MaterialCache&) = delete;
        MaterialCache(MaterialCache&&) = delete;
        MaterialCache& operator=(MaterialCache&&) = delete;
        ~MaterialCache() = default;

        /**
         * @brief Finds a material, and reads it the first time it is asked for.
         *
         * A GUID that will not load is remembered as a failure, so a submesh
         * that names a missing material reports once rather than on every frame.
         * Such a submesh still draws, with the fallback texture.
         *
         * @param device The device that owns the textures.
         * @param content The cooked content to read from.
         * @param textures Where the textures this material names come from.
         * @param guid The material identity, as a submesh stores it. A null GUID
         * is not an error and gives the defaults.
         * @return The material. It is never null, so a draw call needs no check.
         */
        [[nodiscard]] const GpuMaterial& get(gfx::Device* device,
                                             const assets::Content& content,
                                             TextureCache& textures, Guid guid);

        /**
         * @brief Forgets one identity, so the next get() reads it again.
         *
         * This drops two things, and it has to drop both. The first is the
         * material with that identity. The second is every material that names
         * that identity as one of its textures, because a material holds the
         * texture handle it resolved and a reloaded texture is a new handle.
         * Keeping the old one would bind a texture that was freed.
         *
         * @param guid The identity to let go of, of either kind.
         */
        void drop(Guid guid);

        /// @brief Forgets every material. The textures belong elsewhere.
        void destroy();

        /// @brief How many materials are loaded.
        /// @return The count, the default and the failures not included.
        [[nodiscard]] std::size_t size() const { return loaded_.size(); }

        /**
         * @brief Whether the cache holds a material for this identity.
         * @param guid The identity to look for. A null GUID returns false.
         * @return True when the material was loaded and is still here.
         */
        [[nodiscard]] bool has(Guid guid) const { return loaded_.contains(guid); }

        /**
         * @brief Inserts a material that was built by hand.
         *
         * A test that cannot open a content tree calls this. The caller must
         * keep the cache whole: a duplicate GUID hides the first one.
         *
         * @param guid The identity the material goes by.
         * @param material The material to insert, moved in.
         */
        void inject(Guid guid, GpuMaterial material) {
            loaded_.emplace(guid, std::move(material));
        }

    private:
        std::map<Guid, GpuMaterial> loaded_;
        /// The GUIDs that failed, so one bad reference reports once.
        std::map<Guid, bool> failed_;
        /// What a null GUID and a broken one both get.
        GpuMaterial fallback_;
    };

} // namespace engine::render
