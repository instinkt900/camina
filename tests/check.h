#pragma once

// The whole test harness. Two test programs share it, which is the only reason
// it is a header rather than a few lines in one file. A real framework arrives
// when something needs one. See rule 4.6 in DESIGN.md.

#include <cstdio>

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

    inline int report() {
        if (g_failures == 0) {
            std::printf("\nAll tests passed.\n");
            return 0;
        }
        std::printf("\n%d test(s) failed.\n", g_failures);
        return 1;
    }

} // namespace test
