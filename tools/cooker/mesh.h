#pragma once

/**
 * @file
 * @brief The cooker rule that turns a glTF file into cooked meshes.
 *
 * This is the only place that reads glTF. `src/assets/mesh.h` holds the file
 * format the two sides agree on, and the runtime reads that and nothing else.
 * DESIGN.md section 5 rejects assimp, so glTF is the only import format.
 *
 * One glTF file holds several meshes, and each one becomes a cooked file of
 * its own. A prefab names a single mesh, so a file that held all of them
 * together could not be referenced.
 */

#include "assets/manifest.h"
#include "assets/reference.h"
#include "core/guid.h"

#include <filesystem>
#include <string>
#include <vector>

namespace cooker {

    /**
     * @brief Whether this rule handles a file with this extension.
     * @param extension The extension, with the dot, in any letter case.
     * @return True for `.gltf` and `.glb`.
     */
    [[nodiscard]] bool is_mesh_extension(const std::string& extension);

    /**
     * @brief Turns a URI a glTF file holds into the path it names.
     *
     * A URI escapes a space as `%20` and so on, so the text is not a path until
     * it is decoded. Two callers need this: the scan that lists what a glTF
     * names, and the material rule that resolves an image to its sidecar. They
     * have to agree about which file a URI names, so they share one function.
     *
     * @param uri The URI, as cgltf holds it. A null pointer is allowed.
     * @param directory The directory the glTF sits in. A URI resolves against
     * it.
     * @param out The path. It is untouched when this returns false.
     * @return True when the URI names a file. False for a null URI, which is
     * what a `.glb` gives, and for a data URI, which carries the bytes inline.
     * Neither one is a file, so neither one can carry a sidecar. See issue #51.
     */
    [[nodiscard]] bool gltf_uri_path(const char* uri, const std::filesystem::path& directory,
                                     std::filesystem::path& out);

    /// @brief The files a glTF file names besides itself.
    struct GltfReferences {
        /**
         * @brief The buffers, relative to the content root.
         *
         * A `.bin` is payload rather than an asset. It is an input, so editing
         * the geometry cooks the mesh again, and it is not cooked itself.
         */
        std::vector<std::filesystem::path> buffers;

        /**
         * @brief The images, relative to the content root.
         *
         * An image is a real asset with a sidecar of its own, and the texture
         * rule cooks it. It appears here because a cooked material stores the
         * identity out of that sidecar, so replacing the sidecar has to cook
         * the material again.
         */
        std::vector<std::filesystem::path> images;
    };

    /**
     * @brief Lists the files a glTF names besides itself.
     *
     * Without this the manifest would hash only the JSON, and editing the
     * geometry would look like it did nothing. A `.glb` carries its buffer and
     * its images inside and names no file.
     *
     * This parses the JSON and stops. It does not load the buffers, because the
     * cooker calls it before it decides whether to cook at all.
     *
     * @param source The glTF or GLB file to read.
     * @param relative The source path relative to the content root. A URI
     * resolves against the directory this sits in.
     * @param out The paths, appended in the order the file names them.
     * @return True when the file parsed. A file that will not parse reports
     * nothing here and fails later, where the message belongs.
     */
    [[nodiscard]] bool gltf_references(const std::filesystem::path& source,
                                       const std::filesystem::path& relative,
                                       GltfReferences& out);

    /**
     * @brief Cooks every mesh and every material in one glTF file.
     *
     * The cooked meshes stay in the space the glTF put them in. The node tree
     * carries the transforms, and a later part of M4.4 turns that tree into a
     * prefab. Baking a transform here would make one mesh unusable at a second
     * node.
     *
     * @warning The meshes come first in @p outputs and the materials follow.
     * The cooker checks the name of the first output to decide whether it may
     * skip a source, so a glTF file has to keep naming its first mesh there.
     *
     * @param source The glTF or GLB file to read.
     * @param out_root The cooked root to write under.
     * @param relative The source path relative to the content root. It names
     * the cooked files, so `robot.gltf` gives `robot.gltf.0.mesh` and up.
     * @param parent The GUID of the glTF file, from its sidecar. Every part
     * derives its identity from this.
     * @param outputs The cooked files and their identities, appended.
     * @return True when every part was written. False reports why in the log.
     */
    [[nodiscard]] bool cook_gltf(const std::filesystem::path& source,
                                 const std::filesystem::path& out_root,
                                 const std::filesystem::path& relative, engine::Guid parent,
                                 std::vector<engine::assets::ManifestOutput>& outputs);

} // namespace cooker
