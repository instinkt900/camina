#include "ui/blend.h"

#include "core/assert.h"

#include <array>

namespace engine::ui {

    namespace {

        /// The slot each mode draws with, in the order blend_pipeline_for reads.
        constexpr std::size_t kReplace = 0;
        constexpr std::size_t kAlpha = 1;
        constexpr std::size_t kAdd = 2;
        constexpr std::size_t kMultiply = 3;
        constexpr std::size_t kModulate = 4;

        /**
         * The five equations, indexed by slot.
         *
         * Every one of them adds its two terms. The differences are all in the
         * factors, and the alpha pair is not the colour pair in four of the
         * five, so writing them out beats deriving them.
         */
        constexpr std::array<BlendPipeline, kBlendModeCount> kPipelines{ {
            // Replace: the source lands unchanged, so nothing blends at all.
            // The factors below are what Vulkan wants recorded for a disabled
            // attachment, and they are never read.
            { .blend = false,
              .state = { .src_color = gfx::BlendFactor::One,
                         .dst_color = gfx::BlendFactor::Zero,
                         .color_op = gfx::BlendOp::Add,
                         .src_alpha = gfx::BlendFactor::One,
                         .dst_alpha = gfx::BlendFactor::Zero,
                         .alpha_op = gfx::BlendOp::Add } },
            // Alpha: the "over" operator. This is what all three of the modes
            // below drew as until issue #206.
            { .blend = true,
              .state = { .src_color = gfx::BlendFactor::SrcAlpha,
                         .dst_color = gfx::BlendFactor::OneMinusSrcAlpha,
                         .color_op = gfx::BlendOp::Add,
                         .src_alpha = gfx::BlendFactor::One,
                         .dst_alpha = gfx::BlendFactor::OneMinusSrcAlpha,
                         .alpha_op = gfx::BlendOp::Add } },
            // Add: the source is added on top and the attachment keeps its own
            // alpha. This is what a glow or a flash wants.
            { .blend = true,
              .state = { .src_color = gfx::BlendFactor::SrcAlpha,
                         .dst_color = gfx::BlendFactor::One,
                         .color_op = gfx::BlendOp::Add,
                         .src_alpha = gfx::BlendFactor::Zero,
                         .dst_alpha = gfx::BlendFactor::One,
                         .alpha_op = gfx::BlendOp::Add } },
            // Multiply: the source scales what is under it, and its own alpha
            // decides how much of the original survives. A shadow or a tint.
            { .blend = true,
              .state = { .src_color = gfx::BlendFactor::DstColor,
                         .dst_color = gfx::BlendFactor::OneMinusSrcAlpha,
                         .color_op = gfx::BlendOp::Add,
                         .src_alpha = gfx::BlendFactor::DstAlpha,
                         .dst_alpha = gfx::BlendFactor::OneMinusSrcAlpha,
                         .alpha_op = gfx::BlendOp::Add } },
            // Modulate: the attachment is scaled by the source colour and the
            // source itself contributes nothing. Alpha is left alone.
            { .blend = true,
              .state = { .src_color = gfx::BlendFactor::Zero,
                         .dst_color = gfx::BlendFactor::SrcColor,
                         .color_op = gfx::BlendOp::Add,
                         .src_alpha = gfx::BlendFactor::Zero,
                         .dst_alpha = gfx::BlendFactor::One,
                         .alpha_op = gfx::BlendOp::Add } },
        } };

    } // namespace

    std::size_t blend_mode_index(moth_ui::BlendMode mode) {
        switch (mode) {
        case moth_ui::BlendMode::Replace:
            return kReplace;
        case moth_ui::BlendMode::Alpha:
            return kAlpha;
        case moth_ui::BlendMode::Add:
            return kAdd;
        case moth_ui::BlendMode::Multiply:
            return kMultiply;
        case moth_ui::BlendMode::Modulate:
            return kModulate;
        case moth_ui::BlendMode::Invalid:
            break;
        }
        // Invalid means nobody set a mode, and a mode moth_ui adds later lands
        // here too. Either way this pass has no pipeline for it, and drawing it
        // as something else is exactly the silent error #206 was.
        ENGINE_ASSERT(false, "UiPass has no pipeline for that blend mode.");
        return kAlpha;
    }

    BlendPipeline blend_pipeline_for(std::size_t index) {
        ENGINE_ASSERT(index < kBlendModeCount, "That is not a UI blend pipeline slot.");
        if (index >= kBlendModeCount) {
            return kPipelines[kAlpha];
        }
        return kPipelines[index];
    }

}
