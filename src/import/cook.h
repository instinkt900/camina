#pragma once

/**
 * @file
 * @brief Turns a source content tree into a cooked one.
 *
 * The cooker is a separate executable, and this is the part a test can call.
 * `main.cpp` holds only the command line.
 *
 * It links no Vulkan and opens no window. A cook runs on a build machine with
 * no graphics driver, and CI is exactly that machine.
 *
 * It names no game either. A document names an asset in a component field, and
 * which field that is comes from the descriptors. So the caller hands in the
 * component registry, and an application that cooks a game's content registers
 * that game's components first. See @ref engine::import::engine_components.
 */

#include "scene/component_registry.h"

#include <cstddef>
#include <filesystem>
#include <string>

/// @brief The asset cooker. It reads source assets and writes cooked ones.
namespace engine::import {

    /**
     * @brief The component types the engine itself defines.
     *
     * The builtin scene components, the physics components, and the script
     * component when the build carries Lua. A game adds its own on top.
     *
     * @return A registry holding them, for a caller to extend and hand back
     * through @ref Options::components.
     */
    [[nodiscard]] engine::scene::ComponentRegistry engine_components();

    /// @brief What one run of the cooker was asked to do.
    struct Options {
        /// @brief The source content tree to read.
        std::filesystem::path content;

        /// @brief The cooked tree to write. The cooker makes it when it is missing.
        std::filesystem::path out;

        /// @brief Cook every asset, even one the manifest calls unchanged.
        bool force = false;

        /**
         * @brief The component types a document may carry.
         *
         * A scene and a prefab name an asset in a field that carries
         * `reflect::AssetRef`, and only the descriptors say which field that
         * is. A component this does not hold has no fields the cooker will
         * resolve, so a reference inside one fails the cook rather than
         * reaching the cooked tree unresolved.
         *
         * It defaults to @ref engine_components when it is null, which covers
         * a caller with no game of its own.
         *
         * @warning Nothing owns this. The registry has to outlive the call to
         * @ref cook_all, which reads it for the whole run.
         */
        const engine::scene::ComponentRegistry* components = nullptr;
    };

    /// @brief What one run of the cooker did.
    struct Result {
        std::size_t cooked = 0;  ///< Assets this run built.
        std::size_t skipped = 0; ///< Assets the manifest already had, unchanged.
        std::size_t failed = 0;  ///< Assets that did not cook. Each one is logged.
    };

    /**
     * @brief Cooks every asset under the source tree.
     *
     * A source file with no `.meta` sidecar gets one, so the first cook of a
     * tree writes the identities. Commit those sidecars.
     *
     * An asset the cooker has no rule for is copied through unchanged. That
     * keeps a scene and a prefab working while M4.3 and M4.4 add the real
     * rules for a texture and a mesh.
     *
     * @param options What to cook and where to put it.
     * @param result Counts for the caller to report.
     * @return True when every asset cooked. False when any one failed, and the
     * log then names each failure.
     */
    [[nodiscard]] bool cook_all(const Options& options, Result& result);

} // namespace engine::import
