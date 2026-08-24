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
     * @brief The largest face an environment sidecar may ask for.
     *
     * Six faces of a mip chain at half float cost about 64 times the square of
     * this, so 4096 is close to 1 GB and 8192 would pass the size a cooked
     * texture records in a 32-bit field. A cooker that took the larger number
     * would write a header whose payload size wrapped, and the reader would
     * then refuse a file that is on disk and correct.
     */
    inline constexpr std::uint32_t kMaxFaceSize = 4096;

    /// @brief How many rays each prefiltered texel averages when nothing says.
    inline constexpr std::uint32_t kDefaultSpecularSamples = 128;

    /**
     * @brief The most rays a sidecar may ask each prefiltered texel to average.
     *
     * Cook time grows with this and quality stops improving long before it. The
     * bound exists so that a typo in a sidecar fails rather than appearing to
     * hang the build.
     */
    inline constexpr std::uint32_t kMaxSpecularSamples = 16384;

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

        /**
         * @brief How many rays each texel of the prefiltered chain averages.
         *
         * A mip level below the first holds the environment blurred for one
         * roughness, and the cooker builds it by importance sampling the GGX
         * lobe. Too few rays leave bright pixels scattered through a rough
         * reflection, because a small very bright source is either hit or
         * missed. More rays cost cook time and nothing else.
         */
        std::uint32_t specular_samples = kDefaultSpecularSamples;
    };

    /// @brief The width and the height of the BRDF table when nothing says.
    inline constexpr std::uint32_t kDefaultBrdfSize = 256;

    /// @brief The largest BRDF table a sidecar may ask for.
    inline constexpr std::uint32_t kMaxBrdfSize = 2048;

    /// @brief How many rays each entry of the BRDF table averages when nothing says.
    inline constexpr std::uint32_t kDefaultBrdfSamples = 1024;

    /// @brief The most rays a sidecar may ask each BRDF entry to average.
    inline constexpr std::uint32_t kMaxBrdfSamples = 65536;

    /**
     * @brief How the cooker must build the split sum BRDF table.
     *
     * The table depends on no environment and on no material. It is the same
     * numbers in every project, which is why it has a source file of its own
     * rather than being cooked once for each environment that would share it.
     *
     * The source file carries no data. It exists so that the table has a path,
     * an identity from a sidecar, a manifest entry, and hot reload, the same as
     * every other asset. These two settings are the whole of its input.
     */
    struct BrdfImport {
        /**
         * @brief The width and the height of the table, in texels.
         *
         * One axis is the angle to the viewer and the other is roughness. The
         * function is smooth in both, so a small table costs little accuracy.
         */
        std::uint32_t size = kDefaultBrdfSize;

        /**
         * @brief How many rays each entry averages.
         *
         * This runs once for the whole project rather than once for each texel
         * of a cubemap, so it can afford far more rays than the environment
         * rule takes. Too few leave the table visibly banded at low roughness.
         */
        std::uint32_t samples = kDefaultBrdfSamples;
    };

    /**
     * @brief How the cooker must turn a sound file into a cooked sound.
     *
     * There is one decision here, and DESIGN.md section 10 M11 holds the
     * reasoning. A short effect decodes at cook time so that nothing decodes
     * when it plays. A long track keeps its encoded bytes, because decoding an
     * album at cook time fills the cooked tree for no gain.
     */
    struct SoundImport {
        /**
         * @brief Keep the encoded bytes and decode while playing.
         *
         * The cooker guesses this once, when it writes a new sidecar, and then
         * honors what the file says. The guess is that anything which is not a
         * WAV is streamed, because the cook-time decoder reads WAV alone. So a
         * short effect in another format is one edit away from being a short
         * effect, rather than a cook that fails.
         */
        bool stream = false;
    };

    /**
     * @brief What the engine keeps about a source asset, next to the file.
     */
    struct AssetMeta {
        Guid guid;                     ///< The identity every reference to this asset stores.
        TextureImport texture;         ///< Read by the texture rule. Ignored by every other rule.
        ShaderImport shader;           ///< Read by the shader rule. Ignored by every other rule.
        EnvironmentImport environment; ///< Read by the environment rule. Ignored by the rest.
        BrdfImport brdf;               ///< Read by the BRDF rule. Ignored by every other rule.
        SoundImport sound;             ///< Read by the sound rule. Ignored by every other rule.
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

/**
 * @brief Describes a color space, so the inspector draws a drop-down.
 *
 * The `sRGB` enumerator is named by hand rather than through ENGINE_ENUMERATOR,
 * because the C++ identifier is `Srgb` and every `.meta` sidecar in the tree
 * already holds the word `sRGB`. reflect/ compares an enumerator name exactly,
 * so the spelling here is what keeps those files reading with no change to
 * their bytes. See issue #235.
 */
template <>
struct engine::reflect::Describe<engine::assets::ColorSpace> {
    /// @brief The type name a document stores.
    static constexpr const char* name = "ColorSpace";

    /// @brief The two values.
    /// @return A tuple of enumerator descriptors.
    static constexpr auto enumerators() {
        return std::make_tuple(
            engine::reflect::enumerator("sRGB", engine::assets::ColorSpace::Srgb),
            ENGINE_ENUMERATOR(engine::assets::ColorSpace, Linear));
    }
};

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
        return std::make_tuple(
            ENGINE_FIELD(engine::assets::EnvironmentImport, face_size,
                         engine::reflect::Tooltip{ "The width of one cubemap face, in texels. "
                                                   "A face is square." }),
            // Version 2, so an environment sidecar written before the
            // prefiltered chain reads back with no warning about a field it
            // cannot have.
            ENGINE_FIELD(engine::assets::EnvironmentImport, specular_samples,
                         engine::reflect::Version{ 2 },
                         engine::reflect::Tooltip{ "Rays each prefiltered texel averages. "
                                                   "More costs cook time and nothing else." }));
    }
};

/// @brief Field descriptors for the BRDF table settings.
template <>
struct engine::reflect::Describe<engine::assets::BrdfImport> {
    /// @brief The type name a document stores.
    static constexpr const char* name = "BrdfImport";

    /// @brief The fields, in the order a document holds them.
    /// @return The field descriptors.
    static constexpr auto fields() {
        return std::make_tuple(
            ENGINE_FIELD(engine::assets::BrdfImport, size,
                         engine::reflect::Tooltip{ "Both axes of the table, in texels. One is "
                                                   "the view angle and one is roughness." }),
            ENGINE_FIELD(engine::assets::BrdfImport, samples,
                         engine::reflect::Tooltip{ "Rays each entry averages. This cooks once "
                                                   "for the whole project." }));
    }
};

/// @brief Field descriptors for the sound import settings.
template <>
struct engine::reflect::Describe<engine::assets::SoundImport> {
    /// @brief The type name a document stores.
    static constexpr const char* name = "SoundImport";

    /// @brief The fields, in the order a document holds them.
    /// @return The field descriptors.
    static constexpr auto fields() {
        return std::make_tuple(ENGINE_FIELD(
            engine::assets::SoundImport, stream,
            engine::reflect::Tooltip{ "Keep the encoded bytes and decode while playing. Off "
                                      "decodes at cook time, which is what a short effect "
                                      "wants." }));
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
                         engine::reflect::Tooltip{ "Read by the environment rule alone." }),
            // Version 5, so every sidecar written before the BRDF rule reads
            // back with no warning about a field it cannot have.
            ENGINE_FIELD(engine::assets::AssetMeta, brdf, engine::reflect::Version{ 5 },
                         engine::reflect::Tooltip{ "Read by the BRDF rule alone." }),
            // Version 6, so every sidecar written before the sound rule reads
            // back with no warning about a field it cannot have.
            ENGINE_FIELD(engine::assets::AssetMeta, sound, engine::reflect::Version{ 6 },
                         engine::reflect::Tooltip{ "Read by the sound rule alone." }));
    }
};
