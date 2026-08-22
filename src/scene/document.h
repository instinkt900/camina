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

    /**
     * @brief The key that holds the identity of an entity.
     *
     * Beside the parent rather than among the components, because an identity
     * is what an entity is rather than something it carries. A document from
     * before version 4 has none, and the loader makes one as it reads.
     *
     * A prefab instance writes the identity of its root, and one for each
     * entity it added. A member derives its own, so an instance stays one
     * record.
     */
    inline constexpr const char* kIdKey = "id";

    /// @brief The kind word a prefab member's identity is derived under.
    inline constexpr const char* kMemberKind = "member";

    /// @brief The key on one entity that names the prefab it is an instance of.
    inline constexpr const char* kPrefabKey = "prefab";

    /// @brief The key on a prefab instance that holds the fields it changed.
    inline constexpr const char* kOverridesKey = "overrides";

    /**
     * @brief The key on a prefab instance listing the members it destroyed.
     *
     * An array of prefab entity indices. Everything under a destroyed member
     * goes with it, the way destroying an entity does in a world.
     */
    inline constexpr const char* kRemovedKey = "removed";

    /**
     * @brief The key on a prefab instance holding the entities added under it.
     *
     * An array of records, each with a parent index and a component set, the
     * same shape a prefab entity has. They continue the index space the prefab
     * started: a prefab of N entities owns 0 to N-1, and the first added entity
     * is N. One index space covers both, so a parent reads the same whichever
     * kind it names.
     */
    inline constexpr const char* kAddedKey = "added";

    /**
     * @brief The key on a prefab instance holding the members that moved.
     *
     * An object from entity index to its new parent index, both written the way
     * @ref kAddedKey describes. Only a member needs this, because an added
     * entity already carries its own parent.
     */
    inline constexpr const char* kReparentedKey = "reparented";

    /// @brief The parent value a root stores. No entity holds index -1.
    inline constexpr int kNoParent = -1;

    /**
     * @brief The key on one entity record that links it to a prefab instance.
     *
     * A fragment writes this and a scene file does not. A scene collapses an
     * instance to one record, so no member ever gets a record of its own there.
     * A fragment is one subtree, and that subtree can be a single member
     * somebody deleted, so the link has to survive on the record itself.
     *
     * The value is an object holding @ref kRootKey and @ref kIndexKey.
     */
    inline constexpr const char* kMemberKey = "member";

    /// @brief The key inside @ref kMemberKey naming the instance root, as text.
    inline constexpr const char* kRootKey = "root";

    /// @brief The key inside @ref kMemberKey holding the prefab entity index.
    inline constexpr const char* kIndexKey = "index";

    /**
     * @brief The key on a fragment naming the entity its root hung under.
     *
     * An identity, as text. A fragment whose root was a root of the world
     * carries no such key.
     */
    inline constexpr const char* kUnderKey = "under";

    /**
     * @brief The key on a fragment naming the sibling its root sat in front of.
     *
     * An identity, as text. Child order is what a scene file writes and what
     * the hierarchy panel shows, so a subtree put back at the end of the list
     * gives back a different document from the one it was taken from. A
     * fragment whose root was the last child carries no such key.
     */
    inline constexpr const char* kBeforeKey = "before";

    /**
     * @brief The key on a fragment holding where its root sat among the roots.
     *
     * A whole number, and only on a fragment whose root was a root of the
     * world. The roots of a world are not a sibling list, so ::kBeforeKey has
     * nothing to name for one. Scene::Id::order is what orders them, and a root
     * put back without its number would land after every other root and give a
     * different scene file for the same world. See issue #353.
     */
    inline constexpr const char* kOrderKey = "order";

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
