// M6.3. A nine-sliced image must refuse rather than draw stretched.
//
// This program is expected to die. It passes only when the engine assertion
// reported the reason first, which is what expect_assert.cmake checks.
//
// moth_ui cuts a nine-slice into nine Stretch calls in NodeImage::DrawInternal,
// so no layout reaches this path. The interface still allows it, and drawing
// one stretched would show a distorted border rather than nothing. A picture
// that is wrong in that way reads as a layout mistake and costs a long search.

#include "ui/image.h"
#include "ui/renderer.h"

int main() {
    engine::ui::Renderer renderer;
    const engine::ui::Image image{ engine::gfx::TextureHandle{ 1 }, 64, 64 };

    renderer.begin(100, 100);
    renderer.RenderImage(image, moth_ui::IntRect{ { 0, 0 }, { 64, 64 } },
                         moth_ui::IntRect{ { 0, 0 }, { 32, 32 } },
                         moth_ui::ImageScaleType::NineSlice, 1.0F);
    renderer.end();
    return 0;
}
