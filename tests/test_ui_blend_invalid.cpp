// A blend mode the UI pass has no pipeline for must refuse rather than draw.
//
// This program is expected to die. It passes only when the engine assertion
// reported the reason first, which is what expect_assert.cmake checks.
//
// moth_ui::BlendMode carries an `Invalid` sentinel meaning nobody set a mode,
// and moth_ui can add a sixth value at any release. Either one lands in the
// same place. Issue #206 was three modes drawing as a fourth and nothing
// reporting it, so the answer here is a stop rather than a fallback picture.

#include "ui/blend.h"

int main() {
    (void)engine::ui::blend_mode_index(moth_ui::BlendMode::Invalid);
    return 0;
}
