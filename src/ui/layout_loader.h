#pragma once

/**
 * @file
 * @brief Reads a cooked moth_ui layout out of the asset system.
 *
 * M10.3. A layout is an asset, so it arrives as bytes from `assets::Content`
 * rather than as a file a loader opens itself. That is what makes hot reload
 * possible, because the same call serves the first load and every one after it.
 *
 * `moth_ui::Layout::Load` is not used, because it takes a path. Going through
 * Content keeps a layout on the one accessor every other asset uses, and it is
 * what will let the editor read a layout it imported rather than cooked.
 *
 * This half opens no device. Turning the layout into live nodes needs a
 * `moth_ui::Context`, which needs a renderer and both factories, and that is the
 * caller's job. The split is what lets a test drive the reading with no GPU.
 *
 * **A sub-layout is read the same way, through a `moth_ui::ILayoutProvider`.**
 * moth_ui reads a reference off the filesystem without one, against the
 * directory the referencing layout came from, and there is no directory here.
 * The provider is built inside `read_layout` and lives only for that call, so a
 * caller has nothing to hold.
 */

#include "assets/content.h"
#include "core/guid.h"

#include <moth_ui/layout/layout.h>

#include <memory>

namespace engine::ui {

    /// @brief Why a layout did not read.
    enum class LayoutLoad : std::uint8_t {
        Ok,         ///< It read and it is a layout.
        NotInTree,  ///< The content tree holds no asset with that identity.
        NotJson,    ///< The bytes are not JSON.
        NotALayout, ///< It parsed, and it is not a moth_ui layout.
        Cycle,      ///< It refers to itself, through however many layouts.
    };

    /**
     * @brief Reads the layout an identity names, and every layout it refers to.
     *
     * @warning A reference that names an identity the tree does not hold drops
     * that child rather than failing the layout, which is what moth_ui does with
     * any child it cannot read. So a layout can come back Ok with a button
     * missing. The log says which one.
     *
     * @param content The cooked or imported assets to read from.
     * @param guid The identity of the layout.
     * @param out Receives the layout. Untouched unless the result is Ok.
     * @return What happened, so the caller can say which failure it was.
     */
    [[nodiscard]] LayoutLoad read_layout(const assets::Content& content, Guid guid,
                                         std::shared_ptr<moth_ui::Layout>& out);

    /**
     * @brief What a load result should tell a person.
     * @param result The result.
     * @return A sentence, with no trailing stop.
     */
    [[nodiscard]] const char* describe(LayoutLoad result);

} // namespace engine::ui
