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
 *
 * Which field of a document holds a reference comes from the component
 * descriptors, through `scene::for_each_reference_field`. The save side reads
 * that same list, so the two directions cannot disagree about what a reference
 * is. A string that starts with `asset:` anywhere else fails the cook, because
 * it is a reference a person put where nothing will ever resolve it.
 */

#include "assets/manifest.h"
#include "assets/reference.h"
#include "scene/component_registry.h"

#include <filesystem>
#include <vector>

namespace cooker {

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
     * @param types The component types that say which fields name an asset.
     * @param out Receives each named source path, relative to the content root.
     */
    void document_references(const std::filesystem::path& source,
                             const engine::scene::ComponentRegistry& types,
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
     * It also catches a reference standing in a field that names no asset,
     * which the resolve step leaves alone and nothing later would read.
     *
     * @param source The document to check.
     * @param content_root The content root the reference paths are relative to.
     * @param manifest The manifest this cook produced.
     * @param types The component types that say which fields name an asset.
     * @return True when every reference names something the manifest holds.
     */
    [[nodiscard]] bool validate_references(const std::filesystem::path& source,
                                           const std::filesystem::path& content_root,
                                           const engine::assets::Manifest& manifest,
                                           const engine::scene::ComponentRegistry& types);

    /**
     * @brief Cooks one scene or prefab, resolving every reference it holds.
     *
     * @param source The document to read.
     * @param destination Where the cooked document goes.
     * @param content_root The content root the reference paths are relative to.
     * @param types The component types that say which fields name an asset.
     * @return True when the document parsed and every reference resolved.
     *
     * @warning This writes a sidecar for a file a reference names, when that
     * file has none yet. It has to, because a document may be cooked before
     * the asset it names, and both have to agree on the identity.
     */
    [[nodiscard]] bool cook_document(const std::filesystem::path& source,
                                     const std::filesystem::path& destination,
                                     const std::filesystem::path& content_root,
                                     const engine::scene::ComponentRegistry& types);

} // namespace cooker
