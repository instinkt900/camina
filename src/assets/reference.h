#pragma once

/**
 * @file
 * @brief How authored content names an asset, and how a GUID reads back.
 *
 * A cooked sub-asset has a derived identity and nobody chooses it, so a person
 * cannot know it until the cooker has run. Authored content therefore names an
 * asset by source path, and the cooker turns that into the identity before it
 * writes the file. `asset:models/crate/crate.gltf#mesh:0` is one such name.
 *
 * The syntax lives here rather than in the cooker because both ends need it.
 * The cooker reads a reference and writes a GUID. A tool that saves a document
 * a person will edit again does the opposite, or the save would replace every
 * name with the identity it resolved to and undo the whole point.
 *
 * This file knows one reference at a time. Which field of a document holds one
 * is a question only the component descriptors answer, so the walk over a whole
 * document lives in `scene/references.h`.
 */

#include "assets/manifest.h"
#include "core/guid.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace engine::assets {

    /// @brief What marks a string as an asset reference rather than a GUID.
    inline constexpr std::string_view kAssetPrefix = "asset:";

    /// @brief What separates the path from the part of it being named.
    inline constexpr char kPartSeparator = '#';

    /**
     * @brief What a cooked prefab is called.
     *
     * The same as the source, because a prefab is still a prefab after its
     * references resolve, and the runtime opens it by that name.
     */
    inline constexpr const char* kPrefabExtension = ".prefab";

    /// @brief The kind word `Guid::derive` uses for a mesh inside a glTF file.
    inline constexpr const char* kMeshPartKind = "mesh";
    /// @brief The kind word `Guid::derive` uses for a material inside a glTF file.
    inline constexpr const char* kMaterialPartKind = "material";
    /// @brief The kind word `Guid::derive` uses for an image inside a glTF file.
    inline constexpr const char* kTexturePartKind = "texture";
    /// @brief The kind word `Guid::derive` uses for a prefab inside a glTF file.
    inline constexpr const char* kPrefabPartKind = "prefab";
    /// @brief The kind word `Guid::derive` uses for one variant of a shader.
    inline constexpr const char* kShaderPartKind = "shader";
    /// @brief The kind word `Guid::derive` uses for the irradiance of an environment.
    inline constexpr const char* kIrradiancePartKind = "irradiance";

    /**
     * @brief Every kind word a derived identity can carry.
     *
     * A kind word is part of an identity, so adding one is safe and changing
     * one moves every identity that used it. Treat that as a format version.
     */
    inline constexpr std::array<const char*, 6> kPartKinds{
        kMeshPartKind, kMaterialPartKind, kTexturePartKind,
        kPrefabPartKind, kShaderPartKind, kIrradiancePartKind
    };

    /**
     * @brief One asset reference, taken apart.
     *
     * `asset:models/crate/crate.gltf#mesh:0` names part 0 of kind `mesh` inside
     * that file. `asset:models/crate/crate.png` names the file itself, and
     * leaves @ref kind empty.
     */
    struct AssetReference {
        /// @brief The source path, relative to the content root.
        std::filesystem::path source;
        /// @brief The kind word `Guid::derive` takes, or empty for the file itself.
        std::string kind;
        /// @brief The index `Guid::derive` takes. Zero when there is no kind.
        std::uint32_t index = 0;
    };

    /**
     * @brief Reads a reference string, without touching the file system.
     *
     * @param text The whole string, the `asset:` prefix included.
     * @param out The parts. It is not written when the text is rejected.
     * @return True when the text is a reference and it parsed. False for text
     * that does not start with the prefix, and for a reference that will not
     * read, with the reason in the log for the second case.
     *
     * @warning A path that leaves the content tree is refused. Resolving one
     * would read, and write a sidecar beside, a file the content tree does not
     * own. So an absolute path is refused, and so is any path holding a `..`
     * step.
     */
    [[nodiscard]] bool parse_reference(std::string_view text, AssetReference& out);

    /**
     * @brief Writes a reference back out as the string a person would type.
     * @param reference The parts.
     * @return The text, the `asset:` prefix included.
     */
    [[nodiscard]] std::string format_reference(const AssetReference& reference);

    /**
     * @brief The name a scene file uses for one cooked prefab.
     *
     * The name is the source path, so a scene names the file a person edits. A
     * cooked prefab has a derived identity that nobody chose, and a stem alone
     * collides as soon as two directories hold a model of the same name.
     * Neither is a name somebody can write down.
     *
     * A glTF that lists several scenes cooks one prefab for each, and the cooked
     * path carries the scene index. Scene 0 keeps the source path, because that
     * is the common case and it reads well. Every later scene is
     * `<source>#<index>`.
     *
     * This lived in `sandbox/game.h` until M9.6b. It is a cooker convention
     * rather than a game rule, and the editor needs it to turn a dropped
     * identity back into the prefab the library holds.
     *
     * @warning The index comes out of the cooked path and is never counted from
     * the position in the manifest. A prefab's identity is derived from the
     * scene index, so counting would be a second source of truth that disagrees
     * as soon as the outputs of one source are held in another order.
     *
     * @code
     * prefab_name("models/room/room.gltf", "models/room/room.gltf.0.prefab");
     * // "models/room/room.gltf"
     * prefab_name("models/room/room.gltf", "models/room/room.gltf.2.prefab");
     * // "models/room/room.gltf#2"
     * prefab_name("crate.prefab", "crate.prefab"); // "crate.prefab"
     * @endcode
     *
     * @param source The source path the cooker read, as the manifest holds it.
     * @param cooked The cooked output path, which carries the scene index.
     * @return The name a scene file uses to instance that prefab.
     */
    [[nodiscard]] std::string prefab_name(std::string_view source, std::string_view cooked);

    /**
     * @brief Finds the reference that names an identity, if one does.
     *
     * This is the way back from a GUID. The manifest says which source produced
     * an output, and a derived identity is worked out again from the source
     * identity to find which part of it this is.
     *
     * @param manifest The manifest to search.
     * @param guid The identity to name.
     * @return The reference text, or an empty string when nothing in the
     * manifest produced that identity.
     */
    [[nodiscard]] std::string reference_for(const Manifest& manifest, Guid guid);

} // namespace engine::assets
