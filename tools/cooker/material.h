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

#include <filesystem>
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
     * @param out The identities and the outputs, filled in glTF order.
     * @return True when every material was written. False reports why in the log.
     */
    [[nodiscard]] bool cook_materials(const cgltf_data& data,
                                      const std::filesystem::path& source,
                                      const std::filesystem::path& out_root,
                                      const std::filesystem::path& relative,
                                      engine::Guid parent, CookedMaterials& out);

} // namespace cooker
