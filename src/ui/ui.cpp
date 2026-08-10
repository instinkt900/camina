#include "ui/ui.h"

#include <moth_ui/layout/layout.h>
#include <moth_ui/version.h>

#include <filesystem>
#include <string>

namespace engine::ui {

    namespace {
        // How many names to try before giving up. A machine where all of these
        // exist is one where the test cannot say anything useful.
        constexpr int kSelfTestAttempts = 64;
    }

    std::string_view moth_ui_version() {
        return moth_ui::Version;
    }

    bool self_test() {
        // The working directory belongs to whoever started the program, so one
        // fixed relative name is not safe. A file with that name would make
        // this report a failure that is not there. So look under the temporary
        // directory for a name nothing holds.
        //
        // This creates no file and removes none. It only needs a path that
        // Load will refuse.
        std::error_code error;
        const std::filesystem::path root = std::filesystem::temp_directory_path(error);
        if (error) {
            return false;
        }

        std::filesystem::path missing;
        for (int attempt = 0; attempt < kSelfTestAttempts; ++attempt) {
            std::filesystem::path candidate =
                root / ("engine_ui_self_test_" + std::to_string(attempt) + ".mothui");
            if (!std::filesystem::exists(candidate, error)) {
                missing = std::move(candidate);
                break;
            }
        }
        if (missing.empty()) {
            return false;
        }

        // Layout::Load is a real symbol in libmoth_ui.a, so this call is what
        // proves the link. A version constant alone would not, because it is
        // constexpr and the compiler folds it away.
        auto const [layout, result] = moth_ui::Layout::Load(missing);
        return layout == nullptr && result == moth_ui::Layout::LoadResult::DoesNotExist;
    }

}
