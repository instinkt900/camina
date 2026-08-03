#pragma once

/**
 * @file
 * @brief The cooker rule that turns a glTF material into a cooked material.
 *
 * This runs as part of the glTF rule rather than on its own, because a material
 * has no file of its own. It is a part of the glTF file, the way a mesh is, and
 * it gets a derived GUID for the same reason.
 *
 * A material names its textures by GUID. The glTF names them by URI, so this
 * reads the sidecar of each image it points at and stores the identity from
 * there. That is why an image sidecar is an input of the glTF file.
 */

#include "assets/manifest.h"
#include "core/guid.h"

#include <cstddef>
#include <filesystem>
#include <map>
#include <vector>

struct cgltf_data;

namespace cooker {

    /**
     * @brief The kind word Guid::derive uses for a material inside a glTF file.
     *
     * The word is part of the identity, so changing it changes every GUID
     * derived with it, and every reference to those breaks. Treat it the same
     * as a file format version.
     */
    inline constexpr const char* kMaterialPartKind = "material";

    /**
     * @brief The kind word Guid::derive uses for an image inside a glTF file.
     *
     * The word is part of the identity, so changing it changes every GUID
     * derived with it, and every reference to those breaks. Treat it the same
     * as a file format version.
     */
    inline constexpr const char* kTexturePartKind = "texture";

    /// @brief What cook_inline_images() made from one glTF file.
    struct InlineImages {
        /// @brief The identity of each cooked image, keyed by its glTF index.
        std::map<std::size_t, engine::Guid> guids;
        /// @brief One manifest output for each cooked file.
        std::vector<engine::assets::ManifestOutput> outputs;
    };

    /**
     * @brief Cooks every image a glTF carries inside itself.
     *
     * An image with a URI is a real asset with a sidecar, and the texture rule
     * cooks it. An image in a buffer view, or one whose URI carries the bytes
     * inline, has no file. It cannot hold a sidecar, so its identity is derived
     * from the parent the way a mesh and a material already are.
     *
     * Nothing decides its import settings either. So the color space comes from
     * the material slot that uses the image: a base color or an emissive map
     * reads as sRGB, and a normal, metallic-roughness, or occlusion map reads as
     * linear. That is a better answer than the file name heuristic gives,
     * because it reads what the glTF says rather than what somebody named a
     * file.
     *
     * An image no material uses is not cooked. Nothing could reference it.
     *
     * @param data The parsed glTF. cgltf_load_buffers() must have run, because
     * a buffer view image reads through it.
     * @param source The glTF file itself, for the log.
     * @param out_root The cooked root to write under.
     * @param relative The source path relative to the content root. It names the
     * cooked files, so `robot.glb` gives `robot.glb.0.tex` and up.
     * @param parent The GUID of the glTF file, from its sidecar.
     * @param out The identities and the outputs.
     * @return True when every image was written. False reports why in the log.
     */
    [[nodiscard]] bool cook_inline_images(const cgltf_data& data,
                                          const std::filesystem::path& source,
                                          const std::filesystem::path& out_root,
                                          const std::filesystem::path& relative,
                                          engine::Guid parent, InlineImages& out);

    /// @brief What cook_materials() made from one glTF file.
    struct CookedMaterials {
        /// @brief The identity of each material, in the order the glTF lists them.
        std::vector<engine::Guid> guids;
        /// @brief One manifest output for each cooked file.
        std::vector<engine::assets::ManifestOutput> outputs;
    };

    /**
     * @brief Writes one cooked material for every material in a glTF file.
     *
     * A glTF file with no materials writes nothing and reports success. Every
     * submesh then names the null GUID, and the renderer draws it with the
     * fallback texture.
     *
     * @param data The parsed glTF. The buffers need not be loaded.
     * @param source The glTF file itself. An image URI resolves beside it.
     * @param out_root The cooked root to write under.
     * @param relative The source path relative to the content root. It names the
     * cooked files, so `robot.gltf` gives `robot.gltf.0.material` and up.
     * @param parent The GUID of the glTF file, from its sidecar.
     * @param images What cook_inline_images() made, for the textures that have
     * no file of their own.
     * @param out The identities and the outputs, filled in glTF order.
     * @return True when every material was written. False reports why in the log.
     */
    [[nodiscard]] bool cook_materials(const cgltf_data& data,
                                      const std::filesystem::path& source,
                                      const std::filesystem::path& out_root,
                                      const std::filesystem::path& relative,
                                      engine::Guid parent, const InlineImages& images,
                                      CookedMaterials& out);

} // namespace cooker
