#pragma once

/**
 * @file
 * @brief Turns an asset reference a person wrote into the identity it names.
 *
 * A cooked sub-asset has a derived identity, and nobody chooses it.
 * `Guid::derive` works it out from the parent GUID, a kind word, and an index,
 * so a person cannot know it until the cooker has run once. Content the cooker
 * writes is fine, because it derives both ends itself. Content a person writes
 * is not: it used to hold the answer, copied out of the manifest by hand.
 *
 * So a scene and a prefab may name an asset by source path, and this resolves
 * that to the identity before the file is cooked. The cooked file still holds
 * only GUIDs, because that is what survives a rename. The path is the authored
 * form and the GUID is the cooked one.
 *
 * A reference that will not resolve fails the cook and names the file. Before
 * this, a wrong GUID drew nothing and reported one line, which looks exactly
 * like a mesh that failed to upload.
 */

#include "assets/manifest.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace cooker {

    /// @brief What marks a string as an asset reference rather than a GUID.
    inline constexpr std::string_view kAssetPrefix = "asset:";

    /// @brief What separates the path from the part of it being named.
    inline constexpr char kPartSeparator = '#';

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
     * that does not start with the prefix, and for a fragment that will not
     * read, with the reason in the log for the second case.
     *
     * @warning A path that leaves the content tree is refused here. Resolving
     * one would read, and write a sidecar beside, a file the content tree does
     * not own. So an absolute path is refused, and so is any path holding a
     * `..` step.
     */
    [[nodiscard]] bool parse_reference(std::string_view text, AssetReference& out);

    /**
     * @brief Reads every source path a document names, before anything cooks.
     *
     * The sidecar of each one is an input of the document, because the identity
     * comes out of that file. Replacing a sidecar gives the asset a new
     * identity, and a document that still held the old one would name nothing.
     *
     * A file that will not parse names nothing here. The rule reports that,
     * where the message belongs.
     *
     * @param source The document to read.
     * @param out Receives each named source path, relative to the content root.
     */
    void document_references(const std::filesystem::path& source,
                             std::vector<std::filesystem::path>& out);

    /**
     * @brief Checks that every identity a document names was really cooked.
     *
     * `Guid::derive` answers for any index, so `#mesh:5` on a file holding one
     * mesh gives a GUID that looks like every other one and names nothing. The
     * cook would pass and the runtime would draw nothing, which is the failure
     * naming an asset by path exists to remove.
     *
     * This runs after the whole tree is cooked, because only then does the
     * manifest hold every identity there is to find. It also catches a
     * reference that was right and stopped being right, which is what happens
     * when a model loses a mesh.
     *
     * @param source The document to check.
     * @param content_root The content root the reference paths are relative to.
     * @param manifest The manifest this cook produced.
     * @return True when every reference names something the manifest holds.
     */
    [[nodiscard]] bool validate_references(const std::filesystem::path& source,
                                           const std::filesystem::path& content_root,
                                           const engine::assets::Manifest& manifest);

    /**
     * @brief Cooks one scene or prefab, resolving every reference it holds.
     *
     * @param source The document to read.
     * @param destination Where the cooked document goes.
     * @param content_root The content root the reference paths are relative to.
     * @return True when the document parsed and every reference resolved.
     *
     * @warning This writes a sidecar for a file a reference names, when that
     * file has none yet. It has to, because a document may be cooked before
     * the asset it names, and both have to agree on the identity.
     */
    [[nodiscard]] bool cook_document(const std::filesystem::path& source,
                                     const std::filesystem::path& destination,
                                     const std::filesystem::path& content_root);

} // namespace cooker
