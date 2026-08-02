#pragma once

/**
 * @file
 * @brief The key names a scene document and a prefab document share.
 *
 * A prefab is a scene fragment, so the two files hold the same shape. Both
 * readers name these keys, and one copy keeps them from drifting apart.
 */

#include "reflect/json.h"

namespace engine::scene {

    /// @brief The key that carries the schema version. The same one reflect writes.
    inline constexpr const char* kVersionKey = reflect::kVersionKey;

    /// @brief The key that holds the entity array.
    inline constexpr const char* kEntitiesKey = "entities";

    /// @brief The key on one entity that holds its components, by component name.
    inline constexpr const char* kComponentsKey = "components";

    /// @brief The key on one entity that holds its parent index.
    inline constexpr const char* kParentKey = "parent";

    /// @brief The key on one entity that names the prefab it is an instance of.
    inline constexpr const char* kPrefabKey = "prefab";

    /// @brief The key on a prefab instance that holds the fields it changed.
    inline constexpr const char* kOverridesKey = "overrides";

    /// @brief The parent value a root stores. No entity holds index -1.
    inline constexpr int kNoParent = -1;

} // namespace engine::scene
