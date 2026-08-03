#pragma once

/**
 * @file
 * @brief The sidecar file that gives a source asset its GUID.
 *
 * A source file carries no place to keep an identity. A `.png` holds pixels and
 * a `.gltf` holds a scene, and neither one has a field the engine owns. So the
 * identity lives next to the file: `crate.png` gets `crate.png.meta`.
 *
 * The sidecar belongs in version control next to the asset. Move both together
 * and every reference still resolves. Delete it and the next cook writes a new
 * GUID, which breaks every reference. That is the cost of the approach, and
 * every engine that uses sidecars pays it.
 *
 * The file is JSON, written through reflect/, so it grows the same way every
 * other described type does. M4.3 adds the texture color space here.
 */

#include "core/guid.h"
#include "reflect/reflect.h"

#include <filesystem>

namespace engine::assets {

    /// @brief The name a sidecar adds to the source file name.
    inline constexpr const char* kMetaExtension = ".meta";

    /**
     * @brief What the engine keeps about a source asset, next to the file.
     */
    struct AssetMeta {
        Guid guid; ///< The identity every reference to this asset stores.
    };

    /**
     * @brief The sidecar path for a source file.
     *
     * The name keeps the full source name, so `crate.png` and `crate.gltf` get
     * two sidecars rather than one that they fight over.
     *
     * @param source The source asset path.
     * @return The sidecar path.
     */
    [[nodiscard]] std::filesystem::path meta_path(const std::filesystem::path& source);

    /**
     * @brief Reads the sidecar for a source file.
     * @param source The source asset path.
     * @param out The metadata to fill.
     * @return True when the sidecar was there and it parsed.
     */
    [[nodiscard]] bool load_meta(const std::filesystem::path& source, AssetMeta& out);

    /**
     * @brief Writes the sidecar for a source file.
     * @param source The source asset path.
     * @param meta The metadata to write.
     * @return True when the file was written.
     */
    [[nodiscard]] bool save_meta(const std::filesystem::path& source, const AssetMeta& meta);

    /**
     * @brief Reads the sidecar for a source file, and writes one when there is none.
     *
     * This is the call a cooker makes for each file it finds. An asset keeps
     * the GUID it got the first time, so a rename of the pair changes nothing.
     *
     * A sidecar that holds a null GUID counts as broken, and this replaces it.
     * That covers a file somebody truncated or merged badly.
     *
     * @param source The source asset path. The file must exist.
     * @param out The metadata to fill.
     * @return True when @p out holds a valid GUID.
     */
    [[nodiscard]] bool meta_for(const std::filesystem::path& source, AssetMeta& out);

} // namespace engine::assets

/// @brief Field descriptors for the sidecar, so reflect/ reads and writes it.
template <>
struct engine::reflect::Describe<engine::assets::AssetMeta> {
    /// @brief The type name a document stores.
    static constexpr const char* name = "AssetMeta";

    /// @brief The fields, in the order a document holds them.
    /// @return The field descriptors.
    static constexpr auto fields() {
        return std::make_tuple(ENGINE_FIELD(engine::assets::AssetMeta, guid,
                                            engine::reflect::ReadOnly{},
                                            engine::reflect::Tooltip{
                                                "The identity every reference stores. "
                                                "Changing it breaks them all." }));
    }
};
