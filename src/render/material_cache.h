#pragma once

/**
 * @file
 * @brief Cooked materials, read once and bound many times.
 *
 * A submesh names a material by GUID, and several submeshes name the same one.
 * This reads that material once and resolves the textures it names through a
 * TextureCache.
 *
 * A material owns two things on the device: the parameter block that carries its
 * factors, and the descriptor set that names those factors and all five of its
 * textures together. One bind of that set serves a whole submesh.
 *
 * The textures themselves belong to the TextureCache that resolved them. See
 * DESIGN.md section 9 "Materials".
 */

#include "assets/content.h"
#include "assets/material.h"
#include "assets/shader.h"
#include "core/guid.h"
#include "gfx/device.h"
#include "render/texture_cache.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <span>
#include <string_view>

namespace engine::render {

    /// @brief Which descriptor set a material binds to. Set 0 is the frame.
    inline constexpr std::uint32_t kMaterialSet = 1;

    /// @brief The bit in MaterialUniforms::has_maps for each optional map.
    enum class MaterialMap : std::uint32_t {
        BaseColor = 1U << 0U,         ///< The base color texture.
        MetallicRoughness = 1U << 1U, ///< Metallic in blue, roughness in green.
        Normal = 1U << 2U,            ///< A tangent space normal map.
        Occlusion = 1U << 3U,         ///< Ambient occlusion, in red.
        Emissive = 1U << 4U,          ///< The emissive texture.
    };

    /**
     * @brief The parameter block a material binds, as the shader reads it.
     *
     * The layout must match the `Material` block in `mesh.frag` exactly. It is
     * std140, so every vec4 sits on a 16-byte boundary and the trailing scalars
     * pack together.
     *
     * A vec4 holds the emissive factor rather than a vec3, because std140 pads a
     * vec3 to 16 bytes anyway and a written padding word is easier to read than
     * an implied one.
     */
    struct MaterialUniforms {
        std::array<float, 4> base_color_factor{}; ///< Multiplies the base color. Linear.
        std::array<float, 4> emissive_factor{};   ///< Multiplies the emissive. w unused.
        float metallic_factor = 1.0F;             ///< Multiplies the metallic channel.
        float roughness_factor = 1.0F;            ///< Multiplies the roughness channel.
        float normal_scale = 1.0F;                ///< Scales the x and y of the normal map.
        float occlusion_strength = 1.0F;          ///< How far the occlusion darkens.
        /// @brief The threshold AlphaMode::Mask uses.
        float alpha_cutoff = assets::kDefaultAlphaCutoff;
        std::uint32_t alpha_mode = 0; ///< An assets::AlphaMode value.
        std::uint32_t has_maps = 0;   ///< Which maps the material really named.
        std::uint32_t padding = 0;    ///< Keeps the block a multiple of 16.
    };

    /// @brief How many bytes the material parameter block holds.
    inline constexpr std::size_t kMaterialUniformsSize = 64;
    static_assert(sizeof(MaterialUniforms) == kMaterialUniformsSize,
                  "The block goes to the GPU as raw bytes, so its size and its "
                  "layout are part of the contract with mesh.frag.");

    /**
     * @brief One member of MaterialUniforms, as `mesh.frag` must declare it.
     *
     * The offset comes from `offsetof` on the struct above, so this table cannot
     * drift from the C++ side. The shader side comes from what the cooker
     * reflected. Comparing the two is the whole point.
     */
    struct MaterialUniformMember {
        const char* name = "";    ///< The member name, which must match the GLSL one.
        std::uint32_t offset = 0; ///< Bytes from the start of the block.
        assets::ParamType type{}; ///< What the shader must declare it as.
    };

    /**
     * @brief What the `Material` block in `mesh.frag` has to declare.
     *
     * @return The members, in offset order. The storage is static.
     */
    [[nodiscard]] std::span<const MaterialUniformMember> material_uniform_layout();

    /**
     * @brief Checks a reflected shader against material_uniform_layout().
     *
     * The block is written once in GLSL and once as MaterialUniforms, and the
     * GPU reads raw bytes, so a disagreement between the two is invisible. A
     * renamed member reads the neighbouring field, and a member the shader moved
     * reads whatever now sits at that offset. Neither one fails, and neither one
     * looks obviously wrong on screen.
     *
     * The cooker already reflects every member of every uniform block. This is
     * what reads them.
     *
     * @param fragment The cooked fragment shader, with its reflected params.
     * @param where The source path, for the message.
     * @return True when every member matches by name, offset, and type.
     */
    [[nodiscard]] bool check_material_block(const assets::Shader& fragment,
                                            std::string_view where);

    /**
     * @brief Which optional maps a cooked material really named.
     *
     * This is the value MaterialUniforms::has_maps carries, and it is also what
     * decides which compiled form of `mesh.frag` a submesh draws with. The two
     * read the same function so that a shader variant cannot disagree with the
     * block it binds beside.
     *
     * @param material The cooked material.
     * @return A mask of MaterialMap bits.
     */
    [[nodiscard]] std::uint32_t material_maps(const assets::Material& material);

    /**
     * @brief Packs a cooked material into the block the shader reads.
     *
     * This is where a material stops being a file and starts being 64 bytes the
     * GPU understands. It is a free function so that a test can check it without
     * a graphics device, because the mask it builds decides which maps the
     * shader reads and a wrong bit is invisible until something looks wrong.
     *
     * @param material The cooked material.
     * @return The block, ready to upload.
     */
    [[nodiscard]] MaterialUniforms pack_material_uniforms(const assets::Material& material);

    /**
     * @brief One cooked material, with its textures and its descriptor set.
     *
     * The set is what a draw call binds. It names all five textures and the
     * parameter block together, so one bind serves a whole submesh.
     */
    struct GpuMaterial {
        assets::Material source;               ///< Every field the cooked file held.
        gfx::TextureHandle base_color;         ///< The base color, or the fallback texel.
        gfx::TextureHandle metallic_roughness; ///< Metallic and roughness, or the fallback.
        gfx::TextureHandle normal;             ///< The normal map, or the fallback.
        gfx::TextureHandle occlusion;          ///< The occlusion map, or the fallback.
        gfx::TextureHandle emissive;           ///< The emissive map, or the fallback.
        gfx::BufferHandle uniforms;            ///< The parameter block on the device.
        gfx::DescriptorSetHandle set;          ///< What a draw call binds. Null until built.
    };

    /**
     * @brief Holds every material the submeshes asked for.
     *
     * This owns a parameter block and a descriptor set for each material, so
     * destroy() takes a device and frees them. The textures belong to the
     * TextureCache that resolved them and are only referred to here.
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
         * @param pipeline The pipeline whose layout the descriptor set must
         * match. A rebuilt pipeline means every set has to be built again.
         * @param guid The material identity, as a submesh stores it. A null GUID
         * is not an error and gives the defaults.
         * @return The material. It is never null, so a draw call needs no check.
         */
        [[nodiscard]] const GpuMaterial& get(gfx::Device* device,
                                             const assets::Content& content,
                                             TextureCache& textures, gfx::PipelineHandle pipeline,
                                             Guid guid);

        /**
         * @brief Forgets one identity, so the next get() reads it again.
         *
         * This drops two things, and it has to drop both. The first is the
         * material with that identity. The second is every material that names
         * that identity as one of its textures, because a material holds the
         * texture handle it resolved and a reloaded texture is a new handle.
         * Keeping the old one would bind a texture that was freed.
         *
         * @param device The device that owns the buffers and the sets.
         * @param guid The identity to let go of, of either kind.
         *
         * @warning Call this between frames. It frees a descriptor set, which a
         * frame in flight may still read.
         */
        void drop(gfx::Device* device, Guid guid);

        /**
         * @brief Forgets every material and frees what it owns.
         *
         * The textures belong to the TextureCache. The parameter blocks and the
         * descriptor sets belong here, so those go.
         *
         * @param device The device that owns the buffers and the sets.
         */
        void destroy(gfx::Device* device);

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
        /// Uploads the parameter block and builds the descriptor set.
        [[nodiscard]] static bool build(gfx::Device* device, gfx::PipelineHandle pipeline,
                                        const assets::Material& material, GpuMaterial& out);

        /// Frees the parameter block and the descriptor set one material owns.
        static void release(gfx::Device* device, GpuMaterial& material);

        std::map<Guid, GpuMaterial> loaded_;
        /// The GUIDs that failed, so one bad reference reports once.
        std::map<Guid, bool> failed_;
        /// What a null GUID and a broken one both get.
        GpuMaterial fallback_;
    };

} // namespace engine::render
