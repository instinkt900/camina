/**
 * @file ui.h
 * @brief The moth_ui integration layer.
 *
 * This target exists only when the `with_ui` Conan option is on. `engine_core`
 * does not depend on it, so a build without game UI carries none of it. See
 * `DESIGN.md` section 8.5.
 */

#pragma once

#include <string_view>

/**
 * @brief The moth_ui integration.
 *
 * moth_ui is a retained-mode UI library with its own node graph, JSON layouts,
 * and keyframe animation. This namespace holds the engine side of it: the
 * implementations of `moth_ui::IRenderer`, `moth_ui::IImage`, and
 * `moth_ui::IFont` that draw through `gfx::`.
 *
 * @warning No file in this namespace may include a Vulkan header. Hard rule 4.1
 *          keeps Vulkan inside `src/gfx/vulkan/`, and CI greps for a breach.
 */
namespace engine::ui {

    /**
     * @brief Returns the moth_ui version this build compiled and linked against.
     *
     * The engine asks Conan for a version range, and a Conan editable can
     * shadow the cache. So the version a build actually got is worth reporting
     * rather than assuming. See `DESIGN.md` section 8.5.
     *
     * @return The full version string, for example "1.1.1".
     */
    [[nodiscard]] std::string_view moth_ui_version();

    /**
     * @brief Reports whether moth_ui links and its layout reader answers.
     *
     * This asks moth_ui to read a path that does not exist and checks that it
     * refuses rather than crashes. That pulls a symbol out of the static
     * library, so a build that compiles the headers but fails to link cannot
     * pass.
     *
     * @return True when the call returned a failure result, as it should.
     */
    [[nodiscard]] bool self_test();

}
