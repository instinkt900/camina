#pragma once

/**
 * @file
 * @brief Cooked textures, uploaded once and bound many times.
 *
 * A material names a texture by GUID, and several materials name the same one.
 * This turns that GUID into a device texture, and it uploads each one once
 * however many materials use it.
 *
 * It also holds the fallbacks. Every draw call has to bind something, because
 * the pipeline declares a sampler and a descriptor set that is never written is
 * undefined. So a material with no base color, and a material whose texture
 * will not load, both bind one of these.
 *
 * There are two of them, because a `sampler2D` and a `samplerCube` are not the
 * same binding. A flat texture falls back to a single white texel, which is the
 * identity for a color a factor then multiplies. A cubemap falls back to six
 * grey texels. Grey is not an identity, and there is none to be had, so it is a
 * choice: a scene that names no environment reads as a plain room rather than
 * as a black void.
 */

#include "assets/content.h"
#include "core/guid.h"
#include "gfx/device.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <utility>

namespace engine::render {

    /**
     * @brief What every RGB channel of the cube fallback texels holds.
     *
     * Alpha is not one of them. It stays opaque, because the value here is a
     * radiance and an environment has no transparency.
     *
     * About a quarter in linear light. Dim enough to read as a room with no
     * lamp in it, and bright enough that a metal shows its shape rather than
     * reading black.
     *
     * It is here rather than beside the texture it fills, because a scene with
     * no environment needs its irradiance as well as its radiance. The two are
     * the diffuse and the specular halves of one picture, so they have to come
     * from one number. See `render::MeshPass`.
     */
    inline constexpr std::uint8_t kFallbackCubeTexel = 64;

    /// @brief The radiance engine::render::kFallbackCubeTexel stands for, in linear light.
    inline constexpr float kFallbackCubeRadiance =
        static_cast<float>(kFallbackCubeTexel) / 255.0F;

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
         * @brief Builds the two fallback textures.
         *
         * Call this once, before the first get(). It is separate from the
         * constructor because it needs a device and it can fail.
         *
         * @param device The device that owns the textures.
         * @return True when both fallbacks were made.
         */
        [[nodiscard]] bool create(gfx::Device* device);

        /**
         * @brief Finds a flat texture, and uploads it the first time it is asked for.
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

        /**
         * @brief Finds a cubemap, and uploads it the first time it is asked for.
         *
         * This is get() for the other shape. A cooked texture that carries six
         * faces is a cubemap and a shader reads it as a `samplerCube`, and one
         * that carries a single face is not. Asking for the wrong one is a
         * failure rather than a handle the driver would refuse later, because
         * binding a flat texture where the layout declares a cube is undefined.
         *
         * @param device The device that owns the textures.
         * @param content The cooked content to read from.
         * @param guid The cubemap identity. A null GUID is not an error and
         * gives the cube fallback.
         * @return The cubemap, or the cube fallback when there is nothing to load.
         */
        [[nodiscard]] gfx::TextureHandle get_cube(gfx::Device* device,
                                                  const assets::Content& content, Guid guid);

        /// @brief The single white texel every unresolved flat reference binds.
        /// @return The fallback, which is null until create() has run.
        [[nodiscard]] gfx::TextureHandle fallback() const { return fallback_; }

        /// @brief The six grey texels every unresolved cubemap reference binds.
        /// @return The cube fallback, which is null until create() has run.
        [[nodiscard]] gfx::TextureHandle fallback_cube() const { return fallback_cube_; }

        /**
         * @brief Frees one texture, so the next get() uploads it again.
         *
         * This is what hot reload calls. It also forgets a remembered failure,
         * because the whole point of a reload is that a texture which would not
         * load before may load now.
         *
         * Neither fallback is ever dropped. They belong to no source file, so
         * nothing can change them.
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
         * @brief Frees every texture, both fallbacks included.
         * @param device The device that owns them.
         */
        void destroy(gfx::Device* device);

        /// @brief How many textures are loaded.
        /// @return The count, the fallbacks and the failures not included.
        [[nodiscard]] std::size_t size() const { return loaded_.size(); }

    private:
        /// The shared body of get() and get_cube(). @p faces is 1 or 6.
        [[nodiscard]] gfx::TextureHandle load(gfx::Device* device,
                                              const assets::Content& content, Guid guid,
                                              std::uint32_t faces);

        /**
         * @brief What a caller asked for: an identity and a shape.
         *
         * The shape belongs in the key. A GUID alone would let a texture loaded
         * as flat come back for a cubemap binding, because a cache hit answers
         * before the face count is read. It would also let one shape's failure
         * answer for the other, and a scene that names a flat texture as its
         * environment would then hide the material that names the same one.
         */
        using Request = std::pair<Guid, std::uint32_t>;

        std::map<Request, gfx::TextureHandle> loaded_;
        /// The requests that failed, so one bad reference reports once.
        std::map<Request, bool> failed_;
        gfx::TextureHandle fallback_;
        gfx::TextureHandle fallback_cube_;
    };

} // namespace engine::render
