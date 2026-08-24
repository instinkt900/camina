#pragma once

// The whole test harness. Two test programs share it, which is the only reason
// it is a header rather than a few lines in one file. A real framework arrives
// when something needs one. See rule 4.6 in DESIGN.md.

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

namespace test {

    inline int g_failures = 0;

    /// Every scratch root this run asked for. report() removes them.
    inline std::vector<std::filesystem::path> g_scratch_roots;

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

    /**
     * The process id, as text.
     *
     * A scratch path carries it so that two runs of one test binary cannot
     * delete each other's fixtures. It also names the run that left a stale
     * directory behind, which a random number would not.
     *
     * @return The id of this process.
     */
    inline std::string process_id() {
#if defined(_WIN32)
        return std::to_string(_getpid());
#else
        return std::to_string(getpid());
#endif
    }

    /**
     * The scratch directory for one test binary, unique to this process.
     *
     * Every test that writes fixtures starts by emptying its scratch tree. With
     * a fixed name, two runs of the same binary delete each other's files, and
     * the loser fails somewhere unrelated to what it was testing. ctest running
     * the suite in parallel does not hit that, because each binary owns a
     * different name. Two people, or two terminals, do. See issue #293.
     *
     * @param suite A short name for the test binary, such as "cooker".
     * @return The path. It is not created.
     */
    inline std::filesystem::path scratch_root(std::string_view suite) {
        const std::filesystem::path path =
            std::filesystem::temp_directory_path() /
            ("camina_test_" + std::string{ suite } + "_" + process_id());
        // report() removes every root a run asked for. Without this each run
        // leaves a directory of its own behind, where the old fixed name was
        // reused, and a machine that runs the suite often fills with them.
        if (std::ranges::find(g_scratch_roots, path) == g_scratch_roots.end()) {
            g_scratch_roots.push_back(path);
        }
        return path;
    }

    /**
     * One named scratch directory, emptied and created.
     *
     * This is what a test calls. It removes whatever is there and makes the
     * directory again, so each case starts from nothing.
     *
     * @param suite A short name for the test binary, such as "cooker".
     * @param name A name for this case, unique within the binary.
     * @return The directory, which exists when this returns.
     */
    inline std::filesystem::path scratch(std::string_view suite, std::string_view name) {
        const std::filesystem::path path = scratch_root(suite) / name;
        remove_tree(path);
        std::filesystem::create_directories(path);
        return path;
    }

    inline int report() {
        for (const std::filesystem::path& root : g_scratch_roots) {
            remove_tree(root);
        }
        g_scratch_roots.clear();

        if (g_failures == 0) {
            std::printf("\nAll tests passed.\n");
            return 0;
        }
        std::printf("\n%d test(s) failed.\n", g_failures);
        return 1;
    }

} // namespace test
