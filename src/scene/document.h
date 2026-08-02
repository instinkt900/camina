#pragma once

/**
 * @file
 * @brief The shape a scene document and a prefab document share.
 *
 * A prefab is a scene fragment, so the two files hold the same shape. Both
 * readers name these keys and read the parent the same way, and one copy keeps
 * them from drifting apart.
 */

#include "core/log.h"
#include "reflect/json.h"

#include <nlohmann/json.hpp>

#include <string_view>

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

    /**
     * @brief Reads the parent index off one entity record.
     *
     * A record with no parent key is a root.
     *
     * @warning Do not reach for `nlohmann::json::value()` here. It converts the
     * stored value to the type of the default, and it throws when the stored
     * type cannot convert. A file holding `"parent": "root"` therefore ended the
     * process, and nothing in the engine catches a JSON exception. This function
     * checks the type first and reports through the return value, which is what
     * the rest of the readers do.
     *
     * @param record The entity record to read.
     * @param where What to name in a log line, for example "entity 3".
     * @param out The index, or kNoParent for a root. Untouched when the read
     * fails.
     * @return True when the record holds no parent key or a whole number.
     */
    [[nodiscard]] inline bool read_parent(const nlohmann::json& record, std::string_view where,
                                          int& out) {
        const auto found = record.find(kParentKey);
        if (found == record.end()) {
            out = kNoParent;
            return true;
        }
        if (!found->is_number_integer()) {
            ENGINE_LOG_ERROR("{}: {} is {}, and it must be a whole number.", where, kParentKey,
                             found->type_name());
            return false;
        }
        out = found->get<int>();
        return true;
    }

} // namespace engine::scene
