#pragma once

/**
 * @file
 * @brief The sandbox game: what it registers, what it loads, and what it runs.
 *
 * This is the game module. It links into the runtime today, and into the editor
 * at M9. Rule 4.3 says the editor is an application over the same core rather
 * than a build mode, so the game has to compile into both. Building it as a
 * library from the start is what makes that true rather than hopeful.
 *
 * The game names no Vulkan type and no ImGui type. It builds a world, and the
 * application decides how to show it.
 */

#include "assets/content.h"
#include "scene/component_registry.h"
#include "scene/prefab.h"
#include "scene/world.h"

#include <filesystem>
#include <string>
#include <string_view>

namespace sandbox {

    /// @brief The scene the game opens with, inside the content directory.
    inline constexpr const char* kSceneFile = "main.scene";

    /**
     * @brief The crate the scene stacks, and the one the runtime throws.
     *
     * Named here because two places instance it now. The scene names it in a
     * file, and the runtime looks it up to throw one at the stack.
     */
    inline constexpr const char* kCratePrefab = "crate.prefab";

    /**
     * @brief The game's directory inside the cooked content root.
     *
     * The cooker writes each source tree under its own name, so the engine's
     * assets and the game's assets do not collide.
     */
    inline constexpr const char* kContentName = "game";

    /**
     * @brief The name a scene uses for one cooked prefab.
     *
     * The name is the source path, so a scene names the file a person edits. A
     * cooked prefab has a derived identity that nobody chose, and a stem alone
     * collides as soon as two directories hold a model of the same name.
     * Neither is a name somebody can write down.
     *
     * A glTF that lists several scenes cooks one prefab for each, and the
     * cooked path carries the scene index. Scene 0 keeps the source path,
     * because that is the common case and it reads well. Every later scene is
     * `<source>#<index>`.
     *
     * @warning The index comes out of the cooked path and is never counted from
     * the position in the manifest. A prefab's identity is derived from the
     * scene index, so counting would be a second source of truth that disagrees
     * as soon as the outputs of one source are held in another order.
     *
     * @code
     * prefab_name("models/room/room.gltf", "models/room/room.gltf.0.prefab");
     * // "models/room/room.gltf"
     * prefab_name("models/room/room.gltf", "models/room/room.gltf.2.prefab");
     * // "models/room/room.gltf#2"
     * prefab_name("crate.prefab", "crate.prefab"); // "crate.prefab"
     * @endcode
     *
     * @param source The source path the cooker read, as the manifest holds it.
     * @param cooked The cooked output path, which carries the scene index.
     * @return The name a scene file uses to instance that prefab.
     */
    [[nodiscard]] std::string prefab_name(std::string_view source, std::string_view cooked);

    /**
     * @brief Where the game reads its content from when nobody says otherwise.
     *
     * This is the cooked directory next to the executable. An application that
     * wants another one passes it to load() instead.
     *
     * @return The cooked content directory for this game.
     */
    [[nodiscard]] std::filesystem::path default_content_directory();

    /**
     * @brief Registers the component types the game defines.
     *
     * Call this before you read a scene, and after
     * engine::scene::register_builtin_components(). A component the registry
     * does not know is skipped with a warning, so the order matters.
     *
     * @param registry The registry to fill. Defaults to the process-wide one.
     */
    void register_components(
        engine::scene::ComponentRegistry& registry = engine::scene::components());

    /**
     * @brief Reads the prefabs and the opening scene.
     *
     * The world must be empty. The prefabs go in first, because the scene names
     * them.
     *
     * Every prefab in the cooked tree is registered, under the name
     * prefab_name() gives it. So this reads any cooked content tree and not
     * only the sandbox's own, which is what the large test scene of issue #130
     * needs.
     *
     * @param content The directory holding the prefabs and the scene.
     * @param cooked The open cooked content, which holds every prefab and the
     * source path each one came from. Pass null to register no prefab at all,
     * which is what a test with no cooked tree does.
     * @param world The world to fill.
     * @param registry The component types to build. Register the game types
     * first, or the scene loses them.
     * @param library The library to read the prefabs into.
     * @return True when every file parsed and the scene loaded.
     */
    [[nodiscard]] bool load(const std::filesystem::path& content,
                            const engine::assets::Content* cooked, engine::scene::World& world,
                            const engine::scene::ComponentRegistry& registry =
                                engine::scene::components(),
                            engine::scene::PrefabLibrary& library = engine::scene::prefabs());

    /**
     * @brief Runs the game for one frame.
     *
     * Every entity carrying a Spin turns to face the elapsed time. The call
     * changes the local transform and leaves the world matrices to
     * engine::scene::World::update(), which the application calls after this.
     *
     * @param world The world to run.
     * @param seconds Seconds since the game started, not since the last frame.
     * @return How many entities moved.
     */
    std::size_t update(engine::scene::World& world, float seconds);

} // namespace sandbox
