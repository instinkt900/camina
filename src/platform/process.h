#pragma once

/**
 * @file
 * @brief Runs another program and waits for it to finish.
 *
 * M4.5 needs this so the runtime can cook a changed asset. DESIGN.md section 6
 * keeps the cooker a separate executable, so the runtime asks for the work
 * rather than doing it, and no importer links into a shipping build.
 *
 * The arguments arrive as a list and no shell ever sees them. A shell would
 * read `$name` and a backtick in an asset path as a command to run, and the
 * two platforms disagree about how to escape them.
 */

#include <filesystem>
#include <string>
#include <vector>

namespace engine::platform {

    /// @brief What running one program did.
    struct ProcessResult {
        /// @brief True when the program started and ran to its own exit.
        bool ran = false;
        /// @brief What the program returned. Read this only when @ref ran is true.
        int exit_code = 0;
    };

    /**
     * @brief Runs a program, waits for it, and reports how it went.
     *
     * @param program The executable. This is used as given and no PATH search
     * happens, so pass a full path.
     * @param arguments The arguments after the program name.
     * @return Whether it ran, and what it returned. A program that a signal
     * stopped reports `ran` false, with the reason in the log.
     *
     * @warning This blocks until the program finishes. There is no timeout.
     * The caller must run something that ends on its own.
     *
     * @code
     * const auto result = engine::platform::run_process(cooker, { "--content", "x" });
     * if (result.ran && result.exit_code == 0) {
     *     // It worked.
     * }
     * @endcode
     */
    [[nodiscard]] ProcessResult run_process(const std::filesystem::path& program,
                                            const std::vector<std::string>& arguments);

} // namespace engine::platform
