#pragma once

/**
 * @file
 * @brief Where the program is, so it can find what ships beside it.
 */

#include <filesystem>

namespace engine::platform {

    /**
     * @brief The directory holding the running executable.
     *
     * Cooked content ships next to the executable, so this is what turns a
     * relative content name into a path that works from any working directory.
     * Starting the runtime from a shell, from a debugger, or by double-click
     * all give the same answer, which the working directory does not.
     *
     * @return The directory, with a trailing separator removed. It is empty
     * when the platform will not say, and the caller then falls back to the
     * working directory.
     */
    [[nodiscard]] std::filesystem::path executable_directory();

    /**
     * @brief The cooked content root that ships with this executable.
     * @return executable_directory() with the content directory name added.
     */
    [[nodiscard]] std::filesystem::path cooked_content_root();

} // namespace engine::platform
