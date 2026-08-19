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
 * **An import is on demand and it is kept.** Nothing is imported when the tree
 * opens, because a cold cook of the sandbox is about 2.8 seconds and most of
 * that is BC7 and the environment prefilter. The first `read` of an asset runs
 * its rule and keeps every asset that rule produced, so a glTF is imported once
 * however many of its meshes are drawn.
 */

#include "assets/asset_source.h"
#include "assets/content.h"
#include "assets/manifest.h"
#include "core/guid.h"
#include "scene/component_registry.h"

#include <cstddef>
#include <filesystem>
#include <map>
#include <set>
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
         * @brief Imports an asset if it has to, and answers with its bytes.
         *
         * The bytes are the bytes the cooker would write, because the same rule
         * produced them. See AssetSource.
         *
         * @param guid The identity.
         * @param out The bytes.
         * @return True when the asset imported and its bytes were found. An
         * import that fails is reported and leaves the rest of the project
         * usable.
         */
        [[nodiscard]] bool read(Guid guid, std::vector<std::byte>& out) const override;

        /**
         * @brief The component types a document may carry.
         *
         * A scene and a prefab name an asset in a field that carries
         * `reflect::AssetRef`, and only the descriptors say which field that
         * is. An application with a game of its own registers that game's
         * components and hands the registry in here.
         *
         * @param types The registry. Nothing owns it, so it must outlive this.
         */
        void set_components(const engine::scene::ComponentRegistry* types) {
            components_ = types;
        }

        /**
         * @brief How many times a rule has been run.
         *
         * This counts imports, not sources, so it goes up when an import
         * happens a second time. That is what makes it a measurement of the
         * cache rather than of what has been asked for.
         *
         * @return The count.
         */
        [[nodiscard]] std::size_t imports() const { return imports_; }

        /**
         * @brief Takes source files that changed and says what to load again.
         *
         * The editor imports in memory, so there is no cook here. A file that
         * changed is a cache entry to drop, and the next read of it imports
         * again. The whole tree is indexed again as well, because a change can
         * add an asset or take one away: a glTF gains a mesh, a shader gains a
         * variant, a file is deleted.
         *
         * **The changed list comes from a watcher rather than from a diff of
         * the index.** An index says what a tree holds and not what is in the
         * files, so editing the pixels of a texture changes nothing it can see.
         * `assets::Content` can diff, because the cooker hashes every input.
         *
         * @param sources The source paths that changed, relative, with forward
         * slashes. A path the tree never held is ignored.
         * @param out Every identity to load again, and every one that went
         * away. It is cleared first.
         * @return True when the tree was indexed again.
         */
        [[nodiscard]] bool reload(const std::vector<std::filesystem::path>& sources,
                                  std::vector<assets::AssetChange>& out);

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

        /**
         * @brief What the tree holds, in the form a manifest states it.
         *
         * The index already knows every identity, the source that names it and
         * what it is called, which is what a manifest is. So everything that
         * reads a manifest works over a source tree with no change of its own:
         * the asset browser, `assets::reference_for`, and
         * `scene::restore_references`.
         *
         * **It carries no inputs and no hashes.** Those answer "does this need
         * cooking again", which is a question about a cooked tree. Nothing that
         * reads this manifest asks it.
         *
         * @return The manifest, built when the tree was opened.
         */
        [[nodiscard]] const assets::Manifest& manifest() const { return manifest_; }

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

        /// States the finished index as a manifest. See @ref manifest.
        void build_manifest(const std::vector<std::filesystem::path>& sources);

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

        /// The index stated as a manifest, for everything that reads one.
        assets::Manifest manifest_;

        /// The component types a document is read against. Null means the
        /// engine's own, which is right for a tree with no game in it.
        const engine::scene::ComponentRegistry* components_ = nullptr;

        // Mutable, because reading an asset is a const question with an
        // expensive answer. The cache is what makes the second read free, and
        // a caller should not have to hold a mutable source to ask.
        mutable std::map<std::string, std::vector<std::byte>> cache_;
        mutable std::set<std::string> imported_;
        mutable std::size_t imports_ = 0;
    };

} // namespace engine::import
