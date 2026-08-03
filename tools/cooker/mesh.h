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
     * @brief Lists the files a glTF names besides itself.
     *
     * A `.gltf` keeps its vertex data in a `.bin` next to it, and that file is
     * an input to the cook as much as the `.gltf` is. Without this the manifest
     * would hash only the JSON, and editing the geometry would look like it did
     * nothing. A `.glb` carries its buffer inside and names no file.
     *
     * This parses the JSON and stops. It does not load the buffers, because the
     * cooker calls it before it decides whether to cook at all.
     *
     * @param source The glTF or GLB file to read.
     * @param relative The source path relative to the content root. A URI
     * resolves against the directory this sits in.
     * @param inputs The paths, relative to the content root, appended in the
     * order the file names them.
     * @return True when the file parsed. A file that will not parse reports
     * nothing here and fails later, where the message belongs.
     */
    [[nodiscard]] bool gltf_extra_inputs(const std::filesystem::path& source,
                                         const std::filesystem::path& relative,
                                         std::vector<std::filesystem::path>& inputs);

    /**
     * @brief Cooks every mesh in one glTF file.
     *
     * The cooked meshes stay in the space the glTF put them in. The node tree
     * carries the transforms, and M4.4b turns that tree into a prefab. Baking a
     * transform here would make one mesh unusable at a second node.
     *
     * @param source The glTF or GLB file to read.
     * @param out_root The cooked root to write under.
     * @param relative The source path relative to the content root. It names
     * the cooked files, so `robot.gltf` gives `robot.gltf.0.mesh` and up.
     * @param cooked The cooked paths, relative to @p out_root, in mesh order.
     * @return True when every mesh was written. False reports why in the log.
     */
    [[nodiscard]] bool cook_gltf(const std::filesystem::path& source,
                                 const std::filesystem::path& out_root,
                                 const std::filesystem::path& relative,
                                 std::vector<std::filesystem::path>& cooked);

} // namespace cooker
