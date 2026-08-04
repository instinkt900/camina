#pragma once

/**
 * @file
 * @brief The cooker rule that turns a glTF node tree into a prefab.
 *
 * A glTF node tree is a scene fragment with one root and parents that come
 * first. That is the shape M3.3 already reads, so the importer writes a prefab
 * and a scene instances it. There is no second hierarchy format, and rule 4.5
 * holds because the components go through the reflection descriptors.
 *
 * The prefab is what makes a model usable without hand-writing a GUID. Before
 * this, a scene had to name each cooked mesh itself, and those identities are
 * derived rather than chosen, so a person had to cook once and copy them out.
 */

#include "assets/manifest.h"
#include "assets/reference.h"
#include "core/guid.h"

#include <filesystem>
#include <vector>

struct cgltf_data;

namespace cooker {

    /**
     * @brief Writes one prefab for each scene in a glTF file.
     *
     * Every node becomes an entity carrying a Name and a Transform, and a node
     * with a mesh also carries a MeshRenderer naming that cooked mesh.
     *
     * A prefab holds exactly one root. A glTF scene may list several, and the
     * Flight Helmet lists six, so this adds a root of its own when it has to.
     * The added root sits at the identity transform, so it changes nothing about
     * where the parts sit, and it gives an instance one entity to place.
     *
     * A glTF that names no scene writes no prefab and reports success. The
     * meshes still cook, and a scene can still name one.
     *
     * @param data The parsed glTF. The buffers need not be loaded.
     * @param source The glTF file itself, for the log and for the root name.
     * @param out_root The cooked root to write under.
     * @param relative The source path relative to the content root. It names the
     * cooked files, so `robot.gltf` gives `robot.gltf.0.prefab`.
     * @param parent The GUID of the glTF file, from its sidecar.
     * @param meshes The identity of each cooked mesh, in glTF mesh order, so a
     * node that draws one can name it.
     * @param outputs The cooked files and their identities, appended.
     * @return True when every prefab was written. False reports why in the log.
     */
    [[nodiscard]] bool cook_prefabs(const cgltf_data& data,
                                    const std::filesystem::path& source,
                                    const std::filesystem::path& out_root,
                                    const std::filesystem::path& relative, engine::Guid parent,
                                    const std::vector<engine::Guid>& meshes,
                                    std::vector<engine::assets::ManifestOutput>& outputs);

} // namespace cooker
