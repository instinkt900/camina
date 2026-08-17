#pragma once

/**
 * @file
 * @brief Reads and writes a whole world as a `.scene` document.
 *
 * This is the third consumer of the reflection descriptors, after the inspector
 * and the field serializer. It adds no descriptor system of its own. It walks
 * the hierarchy, and hands each component to reflect/json.h through the
 * component registry.
 *
 * The document is deterministic. Saving a world, loading it, and saving it
 * again produces the same bytes, which is the check that catches a field the
 * writer drops or a link the reader rebuilds in the wrong order.
 */

#include "scene/component_registry.h"
#include "scene/prefab.h"
#include "scene/world.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>

namespace engine::scene {

    /**
     * @brief The schema version this writer produces.
     *
     * A reader accepts this version and every earlier one. Raise it when the
     * shape of the document changes, and not when a component gains a field.
     * A component carries its own version, per reflect/json.h.
     *
     * Version 2 added the prefab link and the override patch on an entity.
     * A version 1 document names no prefab, so it still reads.
     *
     * Version 3 added the structural overrides on an instance: the members it
     * destroyed, the entities added under it, and the members that moved. A
     * version 2 document carries none of those, so it still reads. The raise
     * is for the other direction: a reader from before this refuses a version 3
     * document rather than loading it with every structural change dropped and
     * nothing said.
     *
     * Version 4 added the identity of an entity, beside its parent. A version 3
     * document carries none, and the loader makes one for each entity as it
     * reads, so an older scene still opens. What it cannot do is carry an undo
     * entry from a session before it was saved, which nothing does anyway.
     */
    inline constexpr std::uint32_t kSceneVersion = 4;

    /**
     * @brief Writes a world to a JSON document.
     *
     * Entities appear in hierarchy order, parents before children. Each one
     * stores an index rather than an `entt::entity`, because an entity value is
     * an internal detail that a reader must not depend on.
     *
     * A component type that is not registered is not written.
     *
     * A prefab instance collapses to its name and the fields it changed. The
     * entities the prefab supplied are not written, because the prefab already
     * holds them.
     *
     * @param world The world to write.
     * @param registry The component types to consider. Defaults to the
     * process-wide registry.
     * @param library The prefabs to collapse against. An instance of a prefab
     * this library does not hold is written entity by entity, and the link is
     * lost. The writer reports that.
     * @return The document.
     */
    [[nodiscard]] nlohmann::json save_scene(const World& world,
                                            const ComponentRegistry& registry = components(),
                                            const PrefabLibrary& library = prefabs());

    /**
     * @brief Reads a world back from a JSON document.
     *
     * The world must be empty. Loading into a world that already holds entities
     * would mix two scenes, and no caller has asked for that.
     *
     * A component the file names but the registry does not know is a warning,
     * not a failure. The rest of the entity still loads. That keeps an older
     * build able to open a newer file.
     *
     * A prefab the file names but the library does not hold is an error. The
     * reader cannot invent the entities, so it leaves an empty one in its place
     * and keeps reading the rest of the scene.
     *
     * @param document The document to read.
     * @param world The world to fill.
     * @param registry The component types to consider.
     * @param library The prefabs to build instances from.
     * @return True when the document parsed and every known component loaded.
     */
    [[nodiscard]] bool load_scene(const nlohmann::json& document, World& world,
                                  const ComponentRegistry& registry = components(),
                                  const PrefabLibrary& library = prefabs());

    /**
     * @brief Writes a world to a `.scene` file.
     * @param path Where to write. The parent directory must exist.
     * @param world The world to write.
     * @param registry The component types to consider.
     * @param library The prefabs to collapse against.
     * @return True when the file was written.
     */
    [[nodiscard]] bool save_scene_file(const std::filesystem::path& path, const World& world,
                                       const ComponentRegistry& registry = components(),
                                       const PrefabLibrary& library = prefabs());

    /**
     * @brief Reads a `.scene` file into an empty world.
     * @param path The file to read.
     * @param world The world to fill.
     * @param registry The component types to consider.
     * @param library The prefabs to build instances from.
     * @return True when the file parsed and every known component loaded.
     */
    [[nodiscard]] bool load_scene_file(const std::filesystem::path& path, World& world,
                                       const ComponentRegistry& registry = components(),
                                       const PrefabLibrary& library = prefabs());

} // namespace engine::scene
