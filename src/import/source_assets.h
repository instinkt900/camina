#pragma once

/**
 * @file
 * @brief An asset source over a source tree that was never cooked.
 *
 * The editor opens a project by pointing this at the source content directory.
 * It works out what the project holds and what everything is called, from the
 * source files and their `.meta` sidecars alone. See `DESIGN.md` §10 M13.
 *
 * **It agrees with the cooker by construction, not by coincidence.** It takes a
 * rule for a file through `rule_for`, names a part through `part_record`, and
 * asks `gltf_parts` what a glTF holds. The cooker calls the same three.
 * A second copy of any of those answers would drift the first time a rule
 * changed, and the drift would be quiet: the editor would name an asset
 * differently from the runtime and nothing would report it.
 *
 * @warning M13.3a builds the index and imports nothing, so `read` always
 * fails. Issue #363 is the import.
 */

#include "assets/asset_source.h"
#include "core/guid.h"

#include <cstddef>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace engine::import {

    /**
     * @brief One source tree, open for reading.
     *
     * @code
     * engine::import::SourceAssets assets;
     * if (assets.open(directory)) {
     *     std::vector<engine::assets::AssetRecord> prefabs;
     *     (void)assets.assets_of_kind(".prefab", prefabs);
     * }
     * @endcode
     */
    class SourceAssets : public assets::AssetSource {
    public:
        /**
         * @brief Walks a source tree and works out what it holds.
         *
         * A source file with no sidecar gets one, the same as a cook, because
         * that is where an identity comes from. So opening a tree for the first
         * time writes sidecars into it, and those belong in version control.
         *
         * A file that will not be read is reported and skipped. The rest of the
         * tree still opens, because one broken asset should not stop an editor.
         *
         * @param content_root The source content directory.
         * @return True when the directory could be walked. A tree holding a
         * file that would not read is still true.
         */
        [[nodiscard]] bool open(const std::filesystem::path& content_root);

        /**
         * @brief Every asset one source path names. See AssetSource.
         * @param source The source path, relative, with forward slashes.
         * @param out The records. It is cleared first.
         * @return True when the tree holds that path with an asset.
         */
        [[nodiscard]] bool assets_for(std::string_view source,
                                      std::vector<assets::AssetRecord>& out) const override;

        /**
         * @brief Every asset whose name ends with a suffix. See AssetSource.
         * @param suffix The end of the name, with the dot.
         * @param out The records. It is cleared first.
         * @return True. A tree with none of that kind is not a fault.
         */
        [[nodiscard]] bool assets_of_kind(std::string_view suffix,
                                          std::vector<assets::AssetRecord>& out) const override;

        /**
         * @brief Imports an asset and answers with its bytes. See AssetSource.
         * @param guid The identity.
         * @param out The bytes.
         * @return False, always. The import is issue #363.
         */
        [[nodiscard]] bool read(Guid guid, std::vector<std::byte>& out) const override;

        /**
         * @brief The identity a reference names.
         *
         * A scene in a source tree names an asset by path, as
         * `asset:models/crate/crate.gltf#mesh:0`, where a cooked scene names it
         * by identity. This is the resolve the cooker does when it copies a
         * scene, and the editor needs it as it loads one.
         *
         * @param reference The reference text, `asset:` prefix included.
         * @param out The identity. It is untouched when the resolve fails.
         * @return True when the reference named an asset this tree holds.
         */
        [[nodiscard]] bool resolve(std::string_view reference, Guid& out) const;

        /// @brief The directory this was opened on.
        /// @return The source content root.
        [[nodiscard]] const std::filesystem::path& root() const { return root_; }

        /// @brief How many assets the tree holds, parts counted separately.
        /// @return The count.
        [[nodiscard]] std::size_t size() const { return records_.size(); }

        /// @brief How many source files would not be read.
        /// @return The count. Each one was logged as it was found.
        [[nodiscard]] std::size_t failed() const { return failed_; }

    private:
        /// Appends every asset one source file names. False when it would not read.
        [[nodiscard]] bool index_one(const std::filesystem::path& relative);

        std::filesystem::path root_;

        /// Every asset in the tree, in the order a cook would write them.
        std::vector<assets::AssetRecord> records_;

        /// Source path to the records it names, as indices into @ref records_.
        std::map<std::string, std::vector<std::size_t>, std::less<>> by_source_;

        /// Source path to the identity in its sidecar.
        ///
        /// This is not the same as the first record of that path. A glTF names
        /// only derived parts, so no record of one carries the identity of the
        /// file itself, and a reference resolves against the file.
        std::map<std::string, Guid, std::less<>> identity_of_;

        /// Identity to its record, for @ref read and @ref resolve.
        std::map<Guid, std::size_t> by_guid_;

        std::size_t failed_ = 0;
    };

} // namespace engine::import
