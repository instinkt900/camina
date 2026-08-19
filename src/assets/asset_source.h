#pragma once

/**
 * @file
 * @brief The one seam between a caller that wants an asset and wherever the
 * assets are.
 *
 * Every cache and every pass reads its assets through this interface. One
 * implementation reads a cooked tree, which is `assets::Content`. M13.3 adds
 * one that imports from the source tree, and nothing above this file learns
 * which of the two it holds. See `DESIGN.md` §10 M13.
 *
 * **This is not the whole of `Content`.** A caller that means a cooked tree
 * still names `Content`: the hot reload path cooks and compares manifests, and
 * the asset browser lists what a cook produced. Neither question has an answer
 * in a source tree, so neither belongs here.
 */

#include "core/guid.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace engine::assets {

    /**
     * @brief One asset an AssetSource can read.
     *
     * The name is what a log message calls the asset, and it is also what
     * `prefab_name` reads to work out the key a prefab goes into the library
     * under. An importing source has to give an asset the name the cooker
     * would, for the same reason it has to give it the bytes the cooker would.
     */
    struct AssetRecord {
        /// @brief The identity, which is what one asset stores to name another.
        Guid guid;

        /// @brief The source path that names it, relative, with forward slashes.
        std::string source;

        /// @brief What to call it. For a cooked tree this is the cooked path.
        std::string name;
    };

    /**
     * @brief Reads an asset by the identity it goes by.
     *
     * A caller asks three things, and nothing else:
     *
     * - What does this source path name? A path names several assets when one
     *   file holds several, so this answers with a list. A glTF holds a mesh
     *   for each primitive, and a shader with a variant list holds one module
     *   for each form.
     * - What is in the project of this kind? The script host wants every
     *   script and the prefab library wants every prefab, and neither knows a
     *   path to ask about.
     * - What are the bytes for this identity?
     *
     * The first two are asked once at startup and the third is asked once for
     * each asset. So a virtual call costs nothing measurable: a mesh is read
     * once and drawn every frame.
     *
     * @code
     * std::vector<engine::assets::AssetRecord> forms;
     * if (source.assets_for("mesh.frag", forms)) {
     *     std::vector<std::byte> bytes;
     *     (void)source.read(forms[0].guid, bytes);
     * }
     * @endcode
     */
    class AssetSource {
    public:
        AssetSource() = default;
        virtual ~AssetSource() = default;

        AssetSource(const AssetSource&) = delete;
        AssetSource& operator=(const AssetSource&) = delete;
        AssetSource(AssetSource&&) = delete;
        AssetSource& operator=(AssetSource&&) = delete;

        /**
         * @brief Every asset one source path names.
         *
         * The order is the order the importer made them, so a caller that
         * numbers its forms reads the same form on both sides. A shader
         * variant list is numbered that way.
         *
         * @param source The source path, relative to the content root, with
         * forward slashes. Name the GLSL file and not the `.spv`.
         * @param out The records. It is cleared first.
         * @return True when the source is in the project. A source that named
         * no asset is false, because a caller asking about a path wants the
         * asset and never the empty answer.
         */
        [[nodiscard]] virtual bool assets_for(std::string_view source,
                                              std::vector<AssetRecord>& out) const = 0;

        /**
         * @brief Every asset whose name ends with a suffix.
         *
         * This is how a caller that holds no path finds its assets. The script
         * host asks for ::kScriptExtension and the prefab library asks for
         * ::kPrefabExtension.
         *
         * @param suffix The end of the name, with the dot, as ".lua".
         * @param out The records. It is cleared first.
         * @return True when the source could be read. An empty answer is true,
         * because a project with no scripts is a project and not a fault.
         */
        [[nodiscard]] virtual bool assets_of_kind(std::string_view suffix,
                                                  std::vector<AssetRecord>& out) const = 0;

        /**
         * @brief Reads the bytes of one asset.
         * @param guid The identity, from a record or from an asset that names
         * another asset.
         * @param out The bytes.
         * @return True when the source holds that identity and it read.
         */
        [[nodiscard]] virtual bool read(Guid guid, std::vector<std::byte>& out) const = 0;
    };

} // namespace engine::assets
