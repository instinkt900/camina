#pragma once

// The whole test harness. Two test programs share it, which is the only reason
// it is a header rather than a few lines in one file. A real framework arrives
// when something needs one. See rule 4.6 in DESIGN.md.

#include <cstdio>

namespace test {

    inline int g_failures = 0;

    inline void check(bool condition, const char* name) {
        if (condition) {
            std::printf("  pass  %s\n", name);
        } else {
            std::printf("  FAIL  %s\n", name);
            ++g_failures;
        }
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
