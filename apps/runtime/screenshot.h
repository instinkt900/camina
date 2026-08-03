#pragma once

/**
 * @file
 * @brief Writes the last frame to a PNG.
 *
 * A run that ends with no error says the commands were valid. It says nothing
 * about a mesh that came out mirrored, inside out, or upside down, and those are
 * the mistakes a new importer makes. So `runtime --screenshot <file>` writes
 * what it drew, and a person or a build can look at it.
 */

#include "gfx/device.h"

#include <filesystem>

namespace runtime {

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

} // namespace runtime
