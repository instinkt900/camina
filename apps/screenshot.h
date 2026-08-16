#pragma once

/**
 * @file
 * @brief Writes the last frame to a PNG.
 *
 * A run that ends with no error says the commands were valid. It says nothing
 * about a mesh that came out mirrored, inside out, or upside down, and those are
 * the mistakes a new importer makes. So `--screenshot <file>` writes what the
 * program drew, and a person or a build can look at it.
 *
 * Both applications take that option, so this sits beside them rather than
 * inside one of them. The editor needs it for the same reason the runtime does:
 * the picture is the only thing that catches a pass that drew nothing, and
 * issue #190 is the case where the validation layer was happy throughout.
 */

#include "gfx/device.h"

#include <filesystem>

namespace apps {

    /**
     * @brief Captures the frame that was presented last and writes it as a PNG.
     *
     * @warning Call this after end_frame() and before the next begin_frame().
     * There is no finished frame to read at any other time.
     *
     * @param device The device that drew the frame.
     * @param path Where to write. The directory must exist.
     * @return True when the file was written. False reports why in the log.
     */
    [[nodiscard]] bool write_screenshot(engine::gfx::Device* device,
                                        const std::filesystem::path& path);

} // namespace apps
