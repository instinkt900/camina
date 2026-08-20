#pragma once

/**
 * @file
 * @brief Turns the image a moth_ui layout names into the identity it means.
 *
 * A layout is JSON, and it names an image by a path relative to its own
 * directory. That is moth_ui's convention rather than this engine's:
 * `moth_editor` writes a path that way and `moth_packer` reads one that way.
 * The engine follows the format rather than imposing a second convention on it.
 *
 * A path is the authored form and a GUID is the cooked one, which is the same
 * split `import/document.h` makes for a scene. A layout that stored a path
 * would break when somebody renamed the image, and that is what M4 gave every
 * other asset an identity to stop.
 *
 * `moth_ui::AssetId` is what carries the value on both sides. moth_ui never
 * reads it, so the engine is free to put a path in the source file and a GUID
 * in the cooked one. See `DESIGN.md` §8.4 and issue #211.
 */

#include "import/writer.h"

#include <filesystem>
#include <vector>

namespace engine::import {

    /**
     * @brief Reads every image a layout names, before anything cooks.
     *
     * The sidecar of each one is an input of the layout, because the identity
     * comes out of that file. Replacing a sidecar gives the image a new
     * identity, and a layout still holding the old one would name nothing.
     *
     * A file that will not parse names nothing here. The rule reports that,
     * where the message belongs.
     *
     * @param source The layout to read.
     * @param relative The layout path, relative to the content root.
     * @param out Receives each named image, relative to the content root.
     */
    void layout_references(const std::filesystem::path& source,
                           const std::filesystem::path& relative,
                           std::vector<std::filesystem::path>& out);

    /**
     * @brief Cooks one layout, resolving every image it names.
     *
     * @param source The layout to read.
     * @param writer Where the cooked file goes.
     * @param cooked The cooked path, relative to the cooked root.
     * @param relative The layout path, relative to the content root.
     * @param content_root The content root the image paths resolve against.
     * @return True when the layout parsed and every image resolved.
     *
     * @warning This writes a sidecar for an image it names, when that file has
     * none yet. It has to, because a layout may be cooked before the image, and
     * both have to agree on the identity. `cook_document` does the same.
     */
    [[nodiscard]] bool cook_layout(const std::filesystem::path& source, Writer& writer,
                                   const std::filesystem::path& cooked,
                                   const std::filesystem::path& relative,
                                   const std::filesystem::path& content_root);

} // namespace engine::import
