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
    };

    /**
     * @brief Reads the layout an identity names.
     *
     * @warning A sub-layout is not resolved. `moth_ui::LayoutEntityRef` reads a
     * path against the directory the layout came from, and there is no
     * directory here. Issue #211 covers turning that reference into an identity
     * the way an image reference became one.
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
