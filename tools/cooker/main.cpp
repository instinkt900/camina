#include "cook.h"

#include "core/log.h"

#include <cstdio>
#include <string_view>

namespace {

    void usage() {
        std::printf("cooker --content <source directory> --out <cooked directory>\n"
                    "       [--force]\n");
    }

    /// Reads the command line. Reports what is wrong rather than guessing.
    [[nodiscard]] bool parse(int argc, char** argv, cooker::Options& options) {
        for (int i = 1; i < argc; ++i) {
            const std::string_view argument{ argv[i] };
            const bool has_value = i + 1 < argc;

            if (argument == "--content" && has_value) {
                options.content = argv[++i];
            } else if (argument == "--out" && has_value) {
                options.out = argv[++i];
            } else if (argument == "--force") {
                options.force = true;
            } else {
                std::printf("cooker: cannot read the argument %s\n", argv[i]);
                return false;
            }
        }

        if (options.content.empty() || options.out.empty()) {
            std::printf("cooker: --content and --out are both required\n");
            return false;
        }
        return true;
    }

} // namespace

int main(int argc, char** argv) {
    engine::log::init();

    cooker::Options options;
    if (!parse(argc, argv, options)) {
        usage();
        return 2;
    }

    cooker::Result result;
    const bool ok = cooker::cook_all(options, result);

    ENGINE_LOG_INFO("Cooked {}, skipped {}, failed {}.", result.cooked, result.skipped,
                    result.failed);

    // Non-zero on any failure, so a build step and CI both stop here.
    return ok ? 0 : 1;
}
