#pragma once

/**
 * @file blend.h
 * @brief Turns a moth_ui blend mode into the pipeline state that draws it.
 */

#include "gfx/types.h"

#include <moth_ui/graphics/blend_mode.h>

#include <cstddef>

namespace engine::ui {

    /**
     * @brief How many blend modes UiPass builds a pipeline for.
     *
     * moth_ui::BlendMode carries five real values and an `Invalid` sentinel.
     * The sentinel gets no pipeline, because it means "nobody set one" rather
     * than a way to draw.
     */
    inline constexpr std::size_t kBlendModeCount = 5;

    /**
     * @brief What GraphicsPipelineDesc needs to draw one blend mode.
     *
     * Two fields rather than one, because `blend` and `blend_state` are two
     * fields on the descriptor and `Replace` is the only mode that leaves the
     * first false.
     */
    struct BlendPipeline {
        /// @brief What `gfx::GraphicsPipelineDesc::blend` takes.
        bool blend = false;
        /// @brief What `gfx::GraphicsPipelineDesc::blend_state` takes.
        gfx::BlendState state{};
    };

    /**
     * @brief Which pipeline slot a mode draws with.
     *
     * The slot is an index into the array UiPass builds, not the numeric value
     * of the enum. `Invalid` is -1, so the enum value cannot be the index.
     *
     * @param mode The mode a batch recorded.
     * @return A value below kBlendModeCount.
     *
     * @warning An unserved mode asserts and falls back to `Alpha`. Drawing the
     * wrong thing quietly is what issue #206 was, and an assert is what stops
     * the next mode moth_ui adds from repeating it.
     */
    [[nodiscard]] std::size_t blend_mode_index(moth_ui::BlendMode mode);

    /**
     * @brief The blend equation for one slot.
     *
     * The five equations are taken from the moth graphics backend rather than
     * worked out here, so a layout draws in the engine the way it draws in
     * `moth_editor`. Two independent moth sources agree on them: the Vulkan
     * backend and the software `Blend` in `moth_ui/utils/color.h`.
     *
     * @param index A slot from blend_mode_index().
     * @return The pipeline state for that slot.
     *
     * @warning An index at or above kBlendModeCount asserts and falls back to
     * the `Alpha` equation.
     */
    [[nodiscard]] BlendPipeline blend_pipeline_for(std::size_t index);

}
