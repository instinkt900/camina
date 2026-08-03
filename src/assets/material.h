#pragma once

/**
 * @file
 * @brief The cooked material format, shared by the cooker and the runtime.
 *
 * A cooked material is one fixed-size header and nothing else. It names its
 * textures by GUID and holds the numbers that go with them. There is no payload,
 * because a material is a handful of values and a list of references.
 *
 * `tools/cooker/material.cpp` writes it from a glTF material, and nothing else
 * writes it. This header holds only what both sides must agree on, so a change
 * here is a format change and it moves the format version below.
 *
 * The format carries the whole glTF metallic-roughness set, and the renderer
 * reads the base color today. glTF hands the rest over for free, and a field
 * added later would move the format version and cook every model again. So the
 * cooker writes what the source says, and M5 is the milestone that shades with
 * it. See DESIGN.md section 10.
 */

#include "core/guid.h"
#include "math/conventions.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace engine::assets {

    /// @brief The name a cooked material file carries after the source name.
    inline constexpr const char* kMaterialExtension = ".material";

    /**
     * @brief The first four bytes of a cooked material file.
     *
     * The value spells "CMAT" when a person opens the file in a hex viewer.
     */
    inline constexpr std::uint32_t kMaterialMagic = 0x54414D43U;

    /// @brief The format version this build writes and reads.
    inline constexpr std::uint32_t kMaterialVersion = 1;

    /**
     * @brief What a surface does with the alpha of its base color.
     *
     * The values match the three words glTF uses, in the order it lists them.
     */
    enum class AlphaMode : std::uint32_t {
        Opaque = 0, ///< Alpha is ignored and the surface is solid.
        Mask,       ///< A texel below the cutoff is not drawn at all.
        Blend,      ///< Alpha blends the surface over what is behind it.
    };

    /// @brief The largest ::AlphaMode value, so a reader can reject the rest.
    inline constexpr std::uint32_t kAlphaModeMax = static_cast<std::uint32_t>(AlphaMode::Blend);

    /// @brief What glTF uses for alphaCutoff when a material leaves it out.
    inline constexpr float kDefaultAlphaCutoff = 0.5F;

    /**
     * @brief The whole of a cooked material file.
     *
     * The size is a multiple of 8 and every member is 4 or 8 bytes wide, so the
     * layout is the same on both platforms and the struct needs no packing
     * attribute.
     *
     * A texture GUID that is null means the material has no texture of that
     * kind. The factor beside it still applies.
     */
    struct MaterialHeader {
        std::uint32_t magic = kMaterialMagic;     ///< ::kMaterialMagic. Checked first.
        std::uint32_t version = kMaterialVersion; ///< ::kMaterialVersion when written.
        Guid base_color;                          ///< The base color texture, in sRGB.
        Guid metallic_roughness;                  ///< Metallic in blue, roughness in green.
        Guid normal;                              ///< A tangent space normal map.
        Guid occlusion;                           ///< Ambient occlusion, in red.
        Guid emissive;                            ///< The emissive texture, in sRGB.
        std::array<float, 4> base_color_factor{}; ///< Multiplies the base color. Linear.
        std::array<float, 3> emissive_factor{};   ///< Multiplies the emissive. Linear.
        float metallic_factor = 0.0F;             ///< Multiplies the metallic channel.
        float roughness_factor = 0.0F;            ///< Multiplies the roughness channel.
        float normal_scale = 0.0F;                ///< Scales the x and y of the normal map.
        float occlusion_strength = 0.0F;          ///< How far the occlusion darkens.
        float alpha_cutoff = 0.0F;                ///< The threshold AlphaMode::Mask uses.
        std::uint32_t alpha_mode = 0;             ///< An ::AlphaMode value.
        std::uint32_t double_sided = 0;           ///< Nonzero when both faces are drawn.
    };

    /// @brief How many bytes a cooked material file holds.
    inline constexpr std::size_t kMaterialSize = 144;
    static_assert(sizeof(MaterialHeader) == kMaterialSize,
                  "The header is the whole file and it is written and read as raw "
                  "bytes, so its size is part of the file format.");

    /**
     * @brief A cooked material, read into memory.
     *
     * The defaults are the glTF ones, so a material that says nothing about a
     * field behaves the way the glTF specification asks.
     */
    struct Material {
        Guid base_color;                          ///< The base color texture, or null for none.
        Guid metallic_roughness;                  ///< Metallic in blue, roughness in green.
        Guid normal;                              ///< A tangent space normal map.
        Guid occlusion;                           ///< Ambient occlusion, in red.
        Guid emissive;                            ///< The emissive texture, in sRGB.
        Vec4 base_color_factor{ 1.0F };           ///< Multiplies the base color. Linear.
        Vec3 emissive_factor{ 0.0F };             ///< Multiplies the emissive. Linear.
        float metallic_factor = 1.0F;             ///< Multiplies the metallic channel.
        float roughness_factor = 1.0F;            ///< Multiplies the roughness channel.
        float normal_scale = 1.0F;                ///< Scales the x and y of the normal map.
        float occlusion_strength = 1.0F;          ///< How far the occlusion darkens.
        float alpha_cutoff = kDefaultAlphaCutoff; ///< The threshold AlphaMode::Mask uses.
        AlphaMode alpha_mode = AlphaMode::Opaque; ///< What alpha does to this surface.
        bool double_sided = false;                ///< True when both faces are drawn.
    };

    /**
     * @brief Reads a cooked material.
     *
     * This checks the magic, the version, that the file is exactly the size the
     * format calls for, and that every number is a number. A factor that is not
     * a number gives a surface that never appears, for no reason a person can
     * see, so it is refused here where the message names the file.
     *
     * @param bytes The whole file, as read from disk.
     * @param out The material to fill. It copies, so @p bytes may go away after.
     * @param where A name for the log, usually the asset path.
     * @return True when the file is a cooked material this build understands.
     */
    [[nodiscard]] bool read_material(std::span<const std::byte> bytes, Material& out,
                                     std::string_view where);

    /**
     * @brief Writes a material into the bytes a cooked file holds.
     *
     * The cooker calls this. It is here rather than in the cooker so that one
     * file decides the layout and the reader beside it can be tested against
     * the writer.
     *
     * @param material The material to write.
     * @return The header, ready to go to disk as raw bytes.
     */
    [[nodiscard]] MaterialHeader pack_material(const Material& material);

} // namespace engine::assets
