#pragma once

// The whole test harness. Two test programs share it, which is the only reason
// it is a header rather than a few lines in one file. A real framework arrives
// when something needs one. See rule 4.6 in DESIGN.md.

#include <cstdio>
#include <filesystem>
#include <string>

namespace test {

    inline int g_failures = 0;

    /**
     * Reports one check, and flushes.
     *
     * The flush is what makes a crash readable. ctest reads the program through
     * a pipe, and stdout to a pipe is fully buffered, so a program that dies
     * takes every line it printed with it. The log then shows the process
     * stopped and nothing about where. Flushing costs a test run nothing and it
     * turns "assets crashed" into a line number.
     */
    inline void check(bool condition, const char* name) {
        if (condition) {
            std::printf("  pass  %s\n", name);
        } else {
            std::printf("  FAIL  %s\n", name);
            ++g_failures;
        }
        std::fflush(stdout);
    }

    /// Names the group of checks that follow, and flushes for the same reason.
    inline void section(const char* name) {
        std::printf("%s\n", name);
        std::fflush(stdout);
    }

    /**
     * Removes a directory tree without throwing.
     *
     * Windows refuses to delete a file another handle holds open, and the
     * throwing remove_all then ends the process with no message. This function
     * catches that error and reports it through check(), so the test continues
     * to the end and the log names the path that failed.
     *
     * This is for cleanup at the end of a test. A remove that is part of the
     * test itself should use the throwing overload, because it is an assertion.
     *
     * @param path The directory to remove.
     */
    inline void remove_tree(const std::filesystem::path& path) {
        std::error_code error;
        std::filesystem::remove_all(path, error);
        // The name says what is true when the check passes, the way every
        // other check in the suite reads. Naming the failure instead printed
        // "pass  cleanup: could not remove ..." on every cleanup, and a log
        // full of lines that read as contradictions is a log nobody scans.
        check(!error, ("cleanup removed " + path.string()).c_str());
    }

    inline int report() {
        if (g_failures == 0) {
            std::printf("\nAll tests passed.\n");
            return 0;
        }
        std::printf("\n%d test(s) failed.\n", g_failures);
        return 1;
    }

} // namespace test
