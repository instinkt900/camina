// M6.2 tests for the moth_ui batching recorder.
//
// Every one of these runs with no device, for the reason test_frustum.cpp
// gives: the recorder names no Vulkan type, and the part worth testing is the
// state stack and the batching decision rather than the draw call.
//
// The stack rules are not obvious and they are not this engine's. moth_ui makes
// PushColor compose, PushTransform replace, and PushClip intersect. Getting one
// of those backwards produces a picture that is wrong in a way no compiler
// reports.

#include "check.h"
#include "ui/image.h"
#include "ui/renderer.h"

#include <cmath>
#include <cstdlib>

namespace {

    using engine::ui::Renderer;

    moth_ui::IntRect rect(int x0, int y0, int x1, int y1) {
        return moth_ui::IntRect{ { x0, y0 }, { x1, y1 } };
    }

    /// An image with no device behind it. The recorder never reads the texture,
    /// it only compares handles and divides by the size, so a made up handle is
    /// enough and the whole file still runs with no GPU.
    engine::ui::Image fake_image(std::uint64_t handle, int width, int height) {
        return engine::ui::Image{ engine::gfx::TextureHandle{ handle }, width, height };
    }

    void a_fresh_recording_holds_nothing() {
        Renderer renderer;
        renderer.begin(100, 50);
        renderer.end();
        test::check(renderer.vertices().empty(), "no draw means no vertex");
        test::check(renderer.indices().empty(), "no draw means no index");
        test::check(renderer.batches().empty(), "an empty batch is dropped rather than drawn");
        test::check(renderer.logical_width() == 100, "begin records the width");
        test::check(renderer.logical_height() == 50, "begin records the height");
    }

    void one_rect_is_two_triangles() {
        Renderer renderer;
        renderer.begin(100, 100);
        renderer.RenderFilledRect(rect(0, 0, 10, 10));
        renderer.end();

        test::check(renderer.vertices().size() == 4, "a quad is four vertices");
        test::check(renderer.indices().size() == 6, "a quad is two triangles");
        test::check(renderer.batches().size() == 1, "one draw needs one batch");
        test::check(renderer.batches()[0].index_count == 6, "the batch holds the whole quad");
    }

    void push_color_composes_rather_than_replaces() {
        Renderer renderer;
        renderer.begin(100, 100);
        // Half, then half again, is a quarter. A replace would leave a half.
        renderer.PushColor(moth_ui::Color{ 0.5F, 0.5F, 0.5F, 1.0F });
        renderer.PushColor(moth_ui::Color{ 0.5F, 0.5F, 0.5F, 1.0F });
        renderer.RenderFilledRect(rect(0, 0, 10, 10));
        renderer.end();

        // 0.25 in sRGB is about 0.0508 in linear. A composed half would be 0.5,
        // which is about 0.2140, so the two cannot be confused.
        const float red = renderer.vertices()[0].r;
        test::check(red > 0.04F && red < 0.06F,
                    "two half colours compose to a quarter, not a half");
    }

    void popping_never_empties_a_stack() {
        Renderer renderer;
        renderer.begin(100, 100);
        // More pops than pushes. moth_ui does this, and the reference backend
        // guards against it the same way.
        renderer.PopColor();
        renderer.PopColor();
        renderer.PopTransform();
        renderer.PopBlendMode();
        renderer.PopTextureFilter();
        renderer.RenderFilledRect(rect(0, 0, 10, 10));
        renderer.end();

        test::check(renderer.vertices().size() == 4, "the default state survives extra pops");
        const float red = renderer.vertices()[0].r;
        test::check(red > 0.99F, "the default colour is still white");
    }

    void push_transform_replaces_and_bakes_into_the_vertex() {
        Renderer renderer;
        renderer.begin(100, 100);

        renderer.PushTransform(moth_ui::FloatMat4x4::Translation({ 20.0F, 30.0F }));
        renderer.RenderFilledRect(rect(0, 0, 10, 10));
        renderer.end();

        // The corner left local space at (0, 0) and must arrive at (20, 30).
        test::check(std::abs(renderer.vertices()[0].x - 20.0F) < 0.001F,
                    "the transform reached the vertex on the CPU");
        test::check(std::abs(renderer.vertices()[0].y - 30.0F) < 0.001F,
                    "the transform reached the vertex on the CPU");
    }

    void a_second_transform_replaces_the_first() {
        Renderer renderer;
        renderer.begin(100, 100);

        renderer.PushTransform(moth_ui::FloatMat4x4::Translation({ 20.0F, 0.0F }));
        renderer.PushTransform(moth_ui::FloatMat4x4::Translation({ 5.0F, 0.0F }));
        renderer.RenderFilledRect(rect(0, 0, 10, 10));
        renderer.end();

        // Composing would give 25. The node tree already walked the parent
        // chain, so the second one stands alone.
        test::check(std::abs(renderer.vertices()[0].x - 5.0F) < 0.001F,
                    "a transform replaces rather than composes");
    }

    void a_transform_does_not_break_the_batch() {
        Renderer renderer;
        renderer.begin(100, 100);

        for (int i = 0; i < 8; ++i) {
            renderer.PushTransform(
                moth_ui::FloatMat4x4::Translation({ static_cast<float>(i * 10), 0.0F }));
            renderer.RenderFilledRect(rect(0, 0, 5, 5));
            renderer.PopTransform();
        }
        renderer.end();

        // This is the whole point of baking on the CPU. Eight nodes collapse
        // into one draw, where a per-draw push constant would cost eight.
        test::check(renderer.batches().size() == 1, "eight transformed quads are one draw");
        test::check(renderer.batches()[0].index_count == 48, "and the batch holds all of them");
    }

    void a_clip_breaks_the_batch() {
        Renderer renderer;
        renderer.begin(100, 100);
        renderer.RenderFilledRect(rect(0, 0, 10, 10));
        renderer.PushClip(rect(0, 0, 5, 5));
        renderer.RenderFilledRect(rect(0, 0, 10, 10));
        renderer.PopClip();
        renderer.RenderFilledRect(rect(0, 0, 10, 10));
        renderer.end();

        test::check(renderer.batches().size() == 3, "a clip and its end each break the batch");
        test::check(!renderer.batches()[0].clipped, "the first run has no scissor");
        test::check(renderer.batches()[1].clipped, "the middle run has one");
        test::check(!renderer.batches()[2].clipped, "and the last run is clear again");
    }

    void a_nested_clip_intersects() {
        Renderer renderer;
        renderer.begin(100, 100);
        renderer.PushClip(rect(0, 0, 50, 50));
        renderer.PushClip(rect(25, 25, 100, 100));
        renderer.RenderFilledRect(rect(0, 0, 10, 10));
        renderer.end();

        const auto& batch = renderer.batches().back();
        test::check(batch.clipped, "the inner clip is a scissor");
        test::check(batch.clip_x == 25 && batch.clip_y == 25,
                    "the intersection takes the later top left");
        test::check(batch.clip_width == 25 && batch.clip_height == 25,
                    "and the earlier bottom right");
    }

    void clips_that_miss_each_other_are_empty_rather_than_inverted() {
        Renderer renderer;
        renderer.begin(100, 100);
        renderer.PushClip(rect(0, 0, 10, 10));
        renderer.PushClip(rect(50, 50, 60, 60));
        renderer.RenderFilledRect(rect(0, 0, 10, 10));
        renderer.end();

        const auto& batch = renderer.batches().back();
        // A naive intersection gives a negative width, which a scissor reads as
        // an enormous unsigned extent and draws the whole target.
        test::check(batch.clip_width == 0 && batch.clip_height == 0,
                    "two clips that miss leave nothing rather than everything");
    }

    void a_blend_change_breaks_the_batch() {
        Renderer renderer;
        renderer.begin(100, 100);
        renderer.RenderFilledRect(rect(0, 0, 10, 10));
        renderer.PushBlendMode(moth_ui::BlendMode::Add);
        renderer.RenderFilledRect(rect(0, 0, 10, 10));
        renderer.end();

        test::check(renderer.batches().size() == 2, "a blend mode change needs its own draw");
        test::check(renderer.batches()[0].blend == moth_ui::BlendMode::Replace,
                    "the first run carries the default");
        test::check(renderer.batches()[1].blend == moth_ui::BlendMode::Add,
                    "and the second carries the new one");
    }

    void repeated_pushes_leave_no_empty_batch() {
        Renderer renderer;
        renderer.begin(100, 100);
        // A node tree pushes several times before it draws anything.
        renderer.PushClip(rect(0, 0, 50, 50));
        renderer.PushClip(rect(0, 0, 40, 40));
        renderer.PushClip(rect(0, 0, 30, 30));
        renderer.RenderFilledRect(rect(0, 0, 10, 10));
        renderer.end();

        test::check(renderer.batches().size() == 1,
                    "pushes that drew nothing reuse the open batch");
    }

    void a_colour_leaves_srgb_before_it_reaches_the_vertex() {
        Renderer renderer;
        renderer.begin(100, 100);
        renderer.PushColor(moth_ui::Color{ 0.5F, 0.5F, 0.5F, 0.5F });
        renderer.RenderFilledRect(rect(0, 0, 10, 10));
        renderer.end();

        // sRGB 0.5 is about 0.2140 linear. Writing 0.5 straight through would
        // leave the swapchain encoding it a second time.
        const auto& vertex = renderer.vertices()[0];
        test::check(vertex.r > 0.21F && vertex.r < 0.22F, "the colour is linear now");
        test::check(std::abs(vertex.a - 0.5F) < 0.001F, "alpha carries no curve");
    }

    void a_gradient_runs_between_its_two_colours() {
        Renderer renderer;
        renderer.begin(100, 100);

        moth_ui::LinearGradient gradient;
        gradient.startColor = moth_ui::Color{ 0.0F, 0.0F, 0.0F, 1.0F };
        gradient.endColor = moth_ui::Color{ 1.0F, 1.0F, 1.0F, 1.0F };
        gradient.angle = 0.0F; // along +x
        renderer.RenderGradientRect(rect(0, 0, 100, 10), gradient);
        renderer.end();

        // Corner 0 is the top left and corner 1 is the top right.
        const float left = renderer.vertices()[0].r;
        const float right = renderer.vertices()[1].r;
        test::check(left < 0.01F, "the left edge is the start colour");
        test::check(right > 0.99F, "the right edge is the end colour");
    }

    void a_gradient_angle_turns_it() {
        Renderer renderer;
        renderer.begin(100, 100);

        moth_ui::LinearGradient gradient;
        gradient.startColor = moth_ui::Color{ 0.0F, 0.0F, 0.0F, 1.0F };
        gradient.endColor = moth_ui::Color{ 1.0F, 1.0F, 1.0F, 1.0F };
        gradient.angle = 1.5707963F; // a quarter turn, so along +y
        renderer.RenderGradientRect(rect(0, 0, 10, 100), gradient);
        renderer.end();

        const float top_left = renderer.vertices()[0].r;
        const float top_right = renderer.vertices()[1].r;
        const float bottom_left = renderer.vertices()[3].r;
        test::check(std::abs(top_left - top_right) < 0.01F,
                    "a vertical gradient does not vary across x");
        test::check(bottom_left > top_left + 0.9F, "and it does vary down y");
    }

    void drawing_outside_a_recording_is_ignored() {
        Renderer renderer;
        // moth_ui decides the call order, so a draw can arrive before begin()
        // and after end(). Both leave no open batch, and reading one would be
        // an out of bounds read on an empty vector.
        renderer.RenderFilledRect(rect(0, 0, 10, 10));
        test::check(renderer.vertices().empty(), "a draw before begin records nothing");

        renderer.begin(100, 100);
        renderer.end();
        renderer.RenderFilledRect(rect(0, 0, 10, 10));
        test::check(renderer.vertices().empty(), "a draw after end records nothing");
    }

    void pushing_state_outside_a_recording_is_ignored() {
        Renderer renderer;
        // A push and a pop reach break_batch, which reads the open batch. There
        // is none before begin(), and end() drops a trailing batch that
        // recorded nothing, so a recording that drew nothing leaves none
        // either. ENGINE_ASSERT compiles out of a Release build, so the guard
        // has to be a real return.
        renderer.PushClip(rect(0, 0, 10, 10));
        renderer.PopClip();
        renderer.PushBlendMode(moth_ui::BlendMode::Add);
        renderer.PopBlendMode();
        renderer.PushTextureFilter(moth_ui::TextureFilter::Nearest);
        renderer.PopTextureFilter();
        test::check(renderer.batches().empty(), "state before begin records no batch");

        renderer.begin(100, 100);
        renderer.end();
        test::check(renderer.batches().empty(), "a recording that drew nothing keeps no batch");

        renderer.PushClip(rect(0, 0, 10, 10));
        renderer.PushBlendMode(moth_ui::BlendMode::Add);
        renderer.PushTextureFilter(moth_ui::TextureFilter::Nearest);
        test::check(renderer.batches().empty(), "and state after end records none either");
    }

    void an_image_records_its_texture_and_its_source_rect() {
        Renderer renderer;
        const engine::ui::Image image = fake_image(7, 64, 32);

        renderer.begin(100, 100);
        // The bottom right quarter of the image, drawn into a bigger rectangle.
        renderer.RenderImage(image, rect(32, 16, 64, 32), rect(0, 0, 80, 80),
                             moth_ui::ImageScaleType::Stretch, 1.0F);
        renderer.end();

        test::check(renderer.batches().size() == 1, "one image is one draw");
        test::check(renderer.batches()[0].texture.value == 7, "the batch names the texture");

        // A source rectangle is in texels of the whole image, so half the width
        // and half the height are 0.5 either way whatever the two sizes are.
        const auto& top_left = renderer.vertices()[0];
        const auto& bottom_right = renderer.vertices()[2];
        test::check(std::abs(top_left.u - 0.5F) < 0.001F, "the left edge is half the width");
        test::check(std::abs(top_left.v - 0.5F) < 0.001F, "the top edge is half the height");
        test::check(std::abs(bottom_right.u - 1.0F) < 0.001F, "the right edge is the far side");
        test::check(std::abs(bottom_right.v - 1.0F) < 0.001F, "and so is the bottom edge");
    }

    void a_shape_after_an_image_goes_back_to_no_texture() {
        Renderer renderer;
        const engine::ui::Image image = fake_image(7, 64, 64);

        renderer.begin(100, 100);
        renderer.RenderImage(image, rect(0, 0, 64, 64), rect(0, 0, 10, 10),
                             moth_ui::ImageScaleType::Stretch, 1.0F);
        renderer.RenderFilledRect(rect(0, 0, 10, 10));
        renderer.end();

        // Carrying the image into the rectangle would tint it with whatever
        // texel the coordinates land on, which is a wrong picture rather than a
        // missing one.
        test::check(renderer.batches().size() == 2, "a shape after an image needs its own draw");
        test::check(renderer.batches()[0].texture.value == 7, "the image run keeps the texture");
        test::check(!renderer.batches()[1].texture.valid(), "and the shape run has none");
    }

    void two_images_of_one_texture_are_one_draw() {
        Renderer renderer;
        const engine::ui::Image image = fake_image(7, 64, 64);

        renderer.begin(100, 100);
        for (int i = 0; i < 4; ++i) {
            // NodeImage pushes a filter around every image it draws. Breaking
            // on the push rather than on a change would cost four draws here.
            renderer.PushTextureFilter(moth_ui::TextureFilter::Linear);
            renderer.RenderImage(image, rect(0, 0, 64, 64), rect(0, 0, 10, 10),
                                 moth_ui::ImageScaleType::Stretch, 1.0F);
            renderer.PopTextureFilter();
        }
        renderer.end();

        test::check(renderer.batches().size() == 1, "four draws of one image are one batch");
        test::check(renderer.batches()[0].index_count == 24, "and the batch holds all of them");
    }

    void a_second_texture_breaks_the_batch() {
        Renderer renderer;
        const engine::ui::Image first = fake_image(7, 64, 64);
        const engine::ui::Image second = fake_image(9, 64, 64);

        renderer.begin(100, 100);
        renderer.RenderImage(first, rect(0, 0, 64, 64), rect(0, 0, 10, 10),
                             moth_ui::ImageScaleType::Stretch, 1.0F);
        renderer.RenderImage(second, rect(0, 0, 64, 64), rect(0, 0, 10, 10),
                             moth_ui::ImageScaleType::Stretch, 1.0F);
        renderer.end();

        test::check(renderer.batches().size() == 2, "one draw reads one texture");
        test::check(renderer.batches()[0].texture.value == 7, "the first run keeps the first");
        test::check(renderer.batches()[1].texture.value == 9, "and the second takes the second");
    }

    void a_filter_change_breaks_the_batch() {
        Renderer renderer;
        const engine::ui::Image image = fake_image(7, 64, 64);

        renderer.begin(100, 100);
        renderer.RenderImage(image, rect(0, 0, 64, 64), rect(0, 0, 10, 10),
                             moth_ui::ImageScaleType::Stretch, 1.0F);
        renderer.PushTextureFilter(moth_ui::TextureFilter::Nearest);
        renderer.RenderImage(image, rect(0, 0, 64, 64), rect(0, 0, 10, 10),
                             moth_ui::ImageScaleType::Stretch, 1.0F);
        renderer.end();

        // Nothing applies the filter yet, because a gfx sampler belongs to its
        // texture. See issue #209. The recorder still carries it, so the day
        // that changes the batches already say what they wanted.
        test::check(renderer.batches().size() == 2, "a filter change needs its own draw");
        test::check(renderer.batches()[0].filter == moth_ui::TextureFilter::Linear,
                    "the first run carries the default");
        test::check(renderer.batches()[1].filter == moth_ui::TextureFilter::Nearest,
                    "and the second carries the new one");
    }

    void an_invalid_filter_keeps_the_one_in_force() {
        Renderer renderer;
        renderer.begin(100, 100);
        renderer.PushTextureFilter(moth_ui::TextureFilter::Nearest);
        // Invalid is the moth_ui sentinel for "nothing was set", which a layout
        // that saved no filter loads. Taking it as a filter would break the
        // batch and record a state that is not one.
        renderer.PushTextureFilter(moth_ui::TextureFilter::Invalid);
        renderer.RenderFilledRect(rect(0, 0, 10, 10));
        renderer.end();

        test::check(renderer.batches().size() == 1, "a sentinel is not a state change");
        test::check(renderer.batches()[0].filter == moth_ui::TextureFilter::Nearest,
                    "and the filter under it still stands");
    }

    void a_tiled_image_runs_its_coordinates_past_one() {
        Renderer renderer;
        const engine::ui::Image image = fake_image(7, 64, 64);

        renderer.begin(200, 200);
        // A destination four tiles wide at half scale, so 128 pixels of
        // destination over 32 pixels of tile.
        renderer.RenderImage(image, rect(0, 0, 64, 64), rect(0, 0, 128, 64),
                             moth_ui::ImageScaleType::Tile, 0.5F);
        renderer.end();

        const auto& bottom_right = renderer.vertices()[2];
        test::check(std::abs(bottom_right.u - 4.0F) < 0.001F, "four tiles across");
        test::check(std::abs(bottom_right.v - 2.0F) < 0.001F, "and two down");
    }

    void an_image_of_another_backend_draws_nothing() {
        // moth_ui hands the renderer an IImage, and another backend's image is
        // the wrong type behind it. Reading its texture would read a handle
        // that was never a handle.
        class Foreign final : public moth_ui::IImage {
        public:
            [[nodiscard]] int GetWidth() const override { return 8; }
            [[nodiscard]] int GetHeight() const override { return 8; }
            [[nodiscard]] moth_ui::IntVec2 GetDimensions() const override { return { 8, 8 }; }
        };

        Renderer renderer;
        const Foreign foreign;
        renderer.begin(100, 100);
        renderer.RenderImage(foreign, rect(0, 0, 8, 8), rect(0, 0, 10, 10),
                             moth_ui::ImageScaleType::Stretch, 1.0F);
        renderer.end();

        test::check(renderer.vertices().empty(), "another backend's image records nothing");
    }

    void an_empty_destination_records_nothing() {
        Renderer renderer;
        const engine::ui::Image image = fake_image(7, 64, 64);

        renderer.begin(100, 100);
        renderer.RenderImage(image, rect(0, 0, 64, 64), rect(10, 10, 10, 10),
                             moth_ui::ImageScaleType::Stretch, 1.0F);
        renderer.end();

        test::check(renderer.vertices().empty(), "a destination with no area draws nothing");
    }

} // namespace

int main() {
    drawing_outside_a_recording_is_ignored();
    pushing_state_outside_a_recording_is_ignored();
    a_fresh_recording_holds_nothing();
    one_rect_is_two_triangles();
    push_color_composes_rather_than_replaces();
    popping_never_empties_a_stack();
    push_transform_replaces_and_bakes_into_the_vertex();
    a_second_transform_replaces_the_first();
    a_transform_does_not_break_the_batch();
    a_clip_breaks_the_batch();
    a_nested_clip_intersects();
    clips_that_miss_each_other_are_empty_rather_than_inverted();
    a_blend_change_breaks_the_batch();
    repeated_pushes_leave_no_empty_batch();
    a_colour_leaves_srgb_before_it_reaches_the_vertex();
    a_gradient_runs_between_its_two_colours();
    a_gradient_angle_turns_it();
    an_image_records_its_texture_and_its_source_rect();
    a_shape_after_an_image_goes_back_to_no_texture();
    two_images_of_one_texture_are_one_draw();
    a_second_texture_breaks_the_batch();
    a_filter_change_breaks_the_batch();
    an_invalid_filter_keeps_the_one_in_force();
    a_tiled_image_runs_its_coordinates_past_one();
    an_image_of_another_backend_draws_nothing();
    an_empty_destination_records_nothing();
    return test::report();
}
