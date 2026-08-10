#include "ui/ui.h"

#include <moth_ui/layout/layout.h>
#include <moth_ui/version.h>

namespace engine::ui {

    std::string_view moth_ui_version() {
        return moth_ui::Version;
    }

    bool self_test() {
        // A path that cannot exist. Load returns a pair of the layout and a
        // result code, and it must answer DoesNotExist rather than throw.
        //
        // Layout::Load is a real symbol in libmoth_ui.a, so this call is what
        // proves the link. A version constant alone would not, because it is
        // constexpr and the compiler folds it away.
        auto const [layout, result] =
            moth_ui::Layout::Load("engine_ui_self_test_does_not_exist.mothui");
        return layout == nullptr && result == moth_ui::Layout::LoadResult::DoesNotExist;
    }

}
