// Tests for the blend mode a UI batch draws with. Issue #206.
//
// These run with no device, for the reason test_ui_renderer.cpp gives. The
// mapping from a moth_ui blend mode to pipeline state names no Vulkan type, and
// the part worth testing is which equation each mode gets rather than the draw
// call that uses it.
//
// The equations are not this engine's. They come from the moth graphics
// backend, so a layout draws here the way it draws in moth_editor. Getting one
// backwards gives a picture that is wrong rather than missing, and nothing
// reports it. That is exactly what #206 was: three of the five modes drew as
// straight alpha.

#include "check.h"
#include "ui/blend.h"

#include <array>
#include <cstddef>
#include <set>

namespace {

    using engine::ui::blend_mode_index;
    using engine::ui::blend_pipeline_for;
    using engine::ui::BlendPipeline;
    using engine::ui::kBlendModeCount;
    using test::check;

    namespace gfx = engine::gfx;

    /// The state one mode draws with, by the two calls a draw makes.
    BlendPipeline pipeline_for(moth_ui::BlendMode mode) {
        return blend_pipeline_for(blend_mode_index(mode));
    }

    /**
     * Every mode gets a slot of its own.
     *
     * This is the whole of #206 in one check. Before the fix, `needs_blending`
     * answered a bool, so Alpha, Add, Multiply and Modulate all shared one
     * pipeline and three of them drew as the fourth.
     */
    void test_every_mode_gets_its_own_pipeline() {
        constexpr std::array<moth_ui::BlendMode, kBlendModeCount> modes{ {
            moth_ui::BlendMode::Replace,
            moth_ui::BlendMode::Alpha,
            moth_ui::BlendMode::Add,
            moth_ui::BlendMode::Multiply,
            moth_ui::BlendMode::Modulate,
        } };

        std::set<std::size_t> slots;
        for (const moth_ui::BlendMode mode : modes) {
            const std::size_t slot = blend_mode_index(mode);
            check(slot < kBlendModeCount, "the slot is one the pass built a pipeline for");
            slots.insert(slot);
        }
        check(slots.size() == kBlendModeCount, "and no two modes share one");
    }

    /**
     * Replace is the only mode that does not blend.
     *
     * The other four all read what is already in the attachment, so a pipeline
     * that disabled blending for one of them would drop everything under it.
     */
    void test_only_replace_leaves_blending_off() {
        check(!pipeline_for(moth_ui::BlendMode::Replace).blend, "Replace does not blend");
        check(pipeline_for(moth_ui::BlendMode::Alpha).blend, "Alpha blends");
        check(pipeline_for(moth_ui::BlendMode::Add).blend, "Add blends");
        check(pipeline_for(moth_ui::BlendMode::Multiply).blend, "Multiply blends");
        check(pipeline_for(moth_ui::BlendMode::Modulate).blend, "Modulate blends");
    }

    /**
     * Each equation is the one the moth backend uses.
     *
     * Written out rather than derived, because a derivation that is wrong the
     * same way in the test and in the code proves nothing. Two independent moth
     * sources agree on these: the Vulkan backend and the software `Blend` in
     * `moth_ui/utils/color.h`.
     */
    void test_the_equations_are_the_ones_moth_draws_with() {
        const gfx::BlendState alpha = pipeline_for(moth_ui::BlendMode::Alpha).state;
        check(alpha.src_color == gfx::BlendFactor::SrcAlpha &&
                  alpha.dst_color == gfx::BlendFactor::OneMinusSrcAlpha,
              "Alpha is the over operator");
        check(alpha.src_alpha == gfx::BlendFactor::One &&
                  alpha.dst_alpha == gfx::BlendFactor::OneMinusSrcAlpha,
              "and its alpha builds coverage up rather than scaling it down");

        const gfx::BlendState add = pipeline_for(moth_ui::BlendMode::Add).state;
        check(add.src_color == gfx::BlendFactor::SrcAlpha &&
                  add.dst_color == gfx::BlendFactor::One,
              "Add leaves what is under it and adds to it");
        check(add.src_alpha == gfx::BlendFactor::Zero && add.dst_alpha == gfx::BlendFactor::One,
              "and the attachment keeps its own alpha");

        const gfx::BlendState multiply = pipeline_for(moth_ui::BlendMode::Multiply).state;
        check(multiply.src_color == gfx::BlendFactor::DstColor &&
                  multiply.dst_color == gfx::BlendFactor::OneMinusSrcAlpha,
              "Multiply scales the source by what is under it");
        check(multiply.src_alpha == gfx::BlendFactor::DstAlpha &&
                  multiply.dst_alpha == gfx::BlendFactor::OneMinusSrcAlpha,
              "and its alpha follows the same shape");

        const gfx::BlendState modulate = pipeline_for(moth_ui::BlendMode::Modulate).state;
        check(modulate.src_color == gfx::BlendFactor::Zero &&
                  modulate.dst_color == gfx::BlendFactor::SrcColor,
              "Modulate scales what is under it and contributes nothing itself");
        check(modulate.src_alpha == gfx::BlendFactor::Zero &&
                  modulate.dst_alpha == gfx::BlendFactor::One,
              "and it leaves the alpha alone");
    }

    /**
     * Add and Multiply are not each other, and neither is Alpha.
     *
     * The three checks above each name their own equation, so a mapping that
     * gave two modes the same one would pass two of them and fail the third.
     * This says the distinctness out loud, because that is the property the
     * author cares about and it survives a later change to any one equation.
     */
    void test_no_two_blending_modes_share_an_equation() {
        const auto same = [](const gfx::BlendState& left, const gfx::BlendState& right) {
            return left.src_color == right.src_color && left.dst_color == right.dst_color &&
                   left.color_op == right.color_op && left.src_alpha == right.src_alpha &&
                   left.dst_alpha == right.dst_alpha && left.alpha_op == right.alpha_op;
        };

        const gfx::BlendState alpha = pipeline_for(moth_ui::BlendMode::Alpha).state;
        const gfx::BlendState add = pipeline_for(moth_ui::BlendMode::Add).state;
        const gfx::BlendState multiply = pipeline_for(moth_ui::BlendMode::Multiply).state;
        const gfx::BlendState modulate = pipeline_for(moth_ui::BlendMode::Modulate).state;

        check(!same(alpha, add), "Add is not Alpha");
        check(!same(alpha, multiply), "Multiply is not Alpha");
        check(!same(alpha, modulate), "Modulate is not Alpha");
        check(!same(add, multiply), "Add is not Multiply");
        check(!same(add, modulate), "Add is not Modulate");
        check(!same(multiply, modulate), "Multiply is not Modulate");
    }

    /**
     * The default of gfx::BlendState is the over operator.
     *
     * Every other pass in the engine sets `blend` and nothing else, so a change
     * to these defaults would move the scene without touching any pass. The
     * blended geometry of the mesh pass is what that would break.
     */
    void test_the_gfx_default_is_still_the_over_operator() {
        const gfx::BlendState state;
        check(state.src_color == gfx::BlendFactor::SrcAlpha &&
                  state.dst_color == gfx::BlendFactor::OneMinusSrcAlpha,
              "a default blend state is over");
        check(state.src_alpha == gfx::BlendFactor::One &&
                  state.dst_alpha == gfx::BlendFactor::OneMinusSrcAlpha,
              "and its alpha pair is not its colour pair");
        check(state.color_op == gfx::BlendOp::Add && state.alpha_op == gfx::BlendOp::Add,
              "and both equations add");

        // The pass that draws the UI must agree, or a layout that says nothing
        // about blending would not look like a blended surface in the scene.
        const gfx::BlendState from_alpha = pipeline_for(moth_ui::BlendMode::Alpha).state;
        check(from_alpha.src_color == state.src_color &&
                  from_alpha.dst_color == state.dst_color &&
                  from_alpha.src_alpha == state.src_alpha &&
                  from_alpha.dst_alpha == state.dst_alpha,
              "and the UI Alpha mode is that same equation");
    }

}

int main() {
    test::section("the slot each mode draws with");
    test_every_mode_gets_its_own_pipeline();
    test_only_replace_leaves_blending_off();
    test::section("the equations");
    test_the_equations_are_the_ones_moth_draws_with();
    test_no_two_blending_modes_share_an_equation();
    test_the_gfx_default_is_still_the_over_operator();
    return test::report();
}
