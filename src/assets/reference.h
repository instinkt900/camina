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
 */

#include "assets/manifest.h"
#include "core/guid.h"

#include <nlohmann/json.hpp>

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

    /**
     * @brief Every kind word a derived identity can carry.
     *
     * A kind word is part of an identity, so adding one is safe and changing
     * one moves every identity that used it. Treat that as a format version.
     */
    inline constexpr std::array<const char*, 4> kPartKinds{ kMeshPartKind, kMaterialPartKind,
                                                            kTexturePartKind, kPrefabPartKind };

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

    /**
     * @brief Puts every reference back into a document about to be written.
     *
     * A live document holds identities, because that is what the engine reads.
     * A document a person edits again holds references, because an identity is
     * derived and nobody chose it. This walks a document and replaces each
     * string that is an identity the manifest knows with the reference naming
     * it. It is the mirror of what the cooker does on the way in.
     *
     * @param document The document to change in place.
     * @param manifest The manifest that says what produced what.
     * @return How many strings were replaced.
     *
     * @warning This works on the text, so a string field that happens to hold
     * an identity the manifest knows is replaced as well. A field says what it
     * means through its type, and this reads only the text, which is the wrong
     * granularity. Issue #81 routes it through the descriptors instead.
     */
    std::size_t restore_references(nlohmann::json& document, const Manifest& manifest);

} // namespace engine::assets
