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
 * other described type does.
 *
 * Import settings live in a nested struct for each asset kind, and only the
 * cooker rule for that kind reads its own. A shader sidecar therefore carries a
 * texture block it ignores. That costs a few lines in a file nobody reads often,
 * and it buys one sidecar type rather than one for each asset kind.
 */

#include "assets/texture.h"
#include "core/guid.h"
#include "reflect/reflect.h"

#include <filesystem>
#include <string>
#include <vector>

namespace engine::assets {

    /// @brief The name a sidecar adds to the source file name.
    inline constexpr const char* kMetaExtension = ".meta";

    /**
     * @brief How the cooker must turn an image file into a cooked texture.
     *
     * The cooker guesses these once, when it writes a new sidecar, and then
     * honors what the file says forever. Change the file and cook again to
     * change a texture.
     */
    struct TextureImport {
        /**
         * @brief How a shader must read the texels.
         *
         * This is the setting that goes wrong quietly. A normal map read as
         * sRGB gives lighting that is subtly wrong everywhere, and a base color
         * read as linear washes out. See DESIGN.md section 3.
         */
        ColorSpace color_space = ColorSpace::Srgb;

        /**
         * @brief Whether to compress to BC7.
         *
         * BC7 loses a little quality and saves three quarters of the memory.
         * Turn it off for an image whose exact texel values matter, such as a
         * lookup table.
         */
        bool compress = true;

        /// @brief Whether to build a mip chain. Off gives one level.
        bool mips = true;
    };

    /**
     * @brief One compiled form of a shader source.
     *
     * A shader that reads five optional maps would branch on all of them, and a
     * branch the whole draw takes the same way is work every pixel pays for
     * nothing. A variant compiles the source with a set of defines instead, so
     * the branch is gone before the module exists.
     *
     * The defines are written here rather than worked out by the cooker. A
     * cross product of every toggle grows by powers of two, and most of the
     * combinations no material ever asks for.
     */
    struct ShaderVariant {
        /// @brief A name for this form, for the log and for a person reading the file.
        std::string name;
        /// @brief The defines to compile with. An empty list is the base form.
        std::vector<std::string> defines;
    };

    /**
     * @brief How the cooker must compile one GLSL source.
     *
     * An empty variant list cooks the source once, with no defines, which is
     * what every shader did before permutations. A list cooks one module for
     * each entry.
     *
     * @warning The first variant must define nothing. It is the base form, and
     * it keeps the identity of the source asset itself. The rest derive theirs.
     */
    struct ShaderImport {
        std::vector<ShaderVariant> variants; ///< One for each module to compile.
    };

    /// @brief The face size an environment sidecar gets when it does not say.
    inline constexpr std::uint32_t kDefaultFaceSize = 256;

    /**
     * @brief How the cooker must turn an HDR image into an environment cubemap.
     *
     * There is no color space here, and that is deliberate. An `.hdr` file is
     * linear by definition, so the guess the texture rule makes from a file name
     * has nothing to decide. See DESIGN.md section 3.
     */
    struct EnvironmentImport {
        /**
         * @brief The width of one cubemap face, in texels.
         *
         * A face is square, so this is both axes. The whole cubemap costs six
         * faces and a mip chain, so 512 is already 8 MB at half float. The
         * default is a size that shows a sky and a sun without dominating the
         * memory a small scene uses.
         */
        std::uint32_t face_size = kDefaultFaceSize;
    };

    /**
     * @brief What the engine keeps about a source asset, next to the file.
     */
    struct AssetMeta {
        Guid guid;                     ///< The identity every reference to this asset stores.
        TextureImport texture;         ///< Read by the texture rule. Ignored by every other rule.
        ShaderImport shader;           ///< Read by the shader rule. Ignored by every other rule.
        EnvironmentImport environment; ///< Read by the environment rule. Ignored by the rest.
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
     * @param created Set to true when this call wrote the sidecar, and to false
     * when it read one that was already there. Pass null when you do not care.
     * A cooker rule reads this to know when it may fill in a guess.
     * @return True when @p out holds a valid GUID.
     */
    [[nodiscard]] bool meta_for(const std::filesystem::path& source, AssetMeta& out,
                                bool* created = nullptr);

} // namespace engine::assets

/// @brief Field descriptors for the texture import settings.
template <>
struct engine::reflect::Describe<engine::assets::TextureImport> {
    /// @brief The type name a document stores.
    static constexpr const char* name = "TextureImport";

    /// @brief The fields, in the order a document holds them.
    /// @return The field descriptors.
    static constexpr auto fields() {
        return std::make_tuple(
            ENGINE_FIELD(engine::assets::TextureImport, color_space,
                         engine::reflect::Tooltip{
                             "sRGB for base color and emissive. Linear for normal, "
                             "roughness, metallic, and occlusion." }),
            ENGINE_FIELD(engine::assets::TextureImport, compress,
                         engine::reflect::Tooltip{ "BC7. Turn it off when the exact texel "
                                                   "values matter." }),
            ENGINE_FIELD(engine::assets::TextureImport, mips,
                         engine::reflect::Tooltip{ "Build a mip chain." }));
    }
};

/// @brief Field descriptors for one shader variant.
template <>
struct engine::reflect::Describe<engine::assets::ShaderVariant> {
    /// @brief The type name a document stores.
    static constexpr const char* name = "ShaderVariant";

    /// @brief The fields, in the order a document holds them.
    /// @return The field descriptors.
    static constexpr auto fields() {
        return std::make_tuple(
            ENGINE_FIELD(engine::assets::ShaderVariant, name,
                         engine::reflect::Tooltip{ "A name for the log and for a reader." }),
            ENGINE_FIELD(engine::assets::ShaderVariant, defines,
                         engine::reflect::Tooltip{
                             "The defines to compile with. Empty is the base form." }));
    }
};

/// @brief Field descriptors for the shader import settings.
template <>
struct engine::reflect::Describe<engine::assets::ShaderImport> {
    /// @brief The type name a document stores.
    static constexpr const char* name = "ShaderImport";

    /// @brief The fields, in the order a document holds them.
    /// @return The field descriptors.
    static constexpr auto fields() {
        return std::make_tuple(ENGINE_FIELD(
            engine::assets::ShaderImport, variants,
            engine::reflect::Tooltip{ "One module for each. Empty cooks the source once." }));
    }
};

/// @brief Field descriptors for the environment import settings.
template <>
struct engine::reflect::Describe<engine::assets::EnvironmentImport> {
    /// @brief The type name a document stores.
    static constexpr const char* name = "EnvironmentImport";

    /// @brief The fields, in the order a document holds them.
    /// @return The field descriptors.
    static constexpr auto fields() {
        return std::make_tuple(ENGINE_FIELD(
            engine::assets::EnvironmentImport, face_size,
            engine::reflect::Tooltip{ "The width of one cubemap face, in texels. A face "
                                      "is square." }));
    }
};

/// @brief Field descriptors for the sidecar, so reflect/ reads and writes it.
template <>
struct engine::reflect::Describe<engine::assets::AssetMeta> {
    /// @brief The type name a document stores.
    static constexpr const char* name = "AssetMeta";

    /// @brief The fields, in the order a document holds them.
    /// @return The field descriptors.
    static constexpr auto fields() {
        return std::make_tuple(
            ENGINE_FIELD(engine::assets::AssetMeta, guid, engine::reflect::ReadOnly{},
                         engine::reflect::Tooltip{ "The identity every reference stores. "
                                                   "Changing it breaks them all." }),
            // Version 2, so a sidecar written before M4.3 reads back without a
            // warning about a field it cannot have.
            ENGINE_FIELD(engine::assets::AssetMeta, texture, engine::reflect::Version{ 2 },
                         engine::reflect::Tooltip{ "Read by the texture rule alone." }),
            // Version 3, so every sidecar written before permutations reads back
            // with no warning about a field it cannot have.
            ENGINE_FIELD(engine::assets::AssetMeta, shader, engine::reflect::Version{ 3 },
                         engine::reflect::Tooltip{ "Read by the shader rule alone." }),
            // Version 4, so every sidecar written before the environment rule
            // reads back with no warning about a field it cannot have.
            ENGINE_FIELD(engine::assets::AssetMeta, environment, engine::reflect::Version{ 4 },
                         engine::reflect::Tooltip{ "Read by the environment rule alone." }));
    }
};
