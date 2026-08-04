#pragma once

/**
 * @file
 * @brief A reusable scene fragment, and the per-field overrides an instance keeps.
 *
 * A prefab holds one root and its descendants, in the same document shape a
 * scene file uses. A scene stores an instance as the prefab name plus the
 * fields that instance changed, and nothing else. Editing the prefab therefore
 * reaches every instance that left the field alone.
 *
 * The field granularity comes from the reflection descriptors. A component
 * document carries one key for each described field, so a patch that names one
 * key overrides one field. This file adds no descriptor system of its own, and
 * rule 4.5 holds.
 */

#include "scene/component_registry.h"
#include "scene/world.h"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace engine::scene {

    /**
     * @brief The schema version this build writes for a prefab document.
     *
     * A reader accepts this version and every earlier one.
     */
    inline constexpr std::uint32_t kPrefabVersion = 1;

    /**
     * @brief One entity inside a prefab.
     *
     * The components stay as JSON. A prefab is a template, not a live entity,
     * so nothing builds a component type until an instance asks for one.
     */
    struct PrefabEntity {
        /// @brief The parent, as an index into the prefab. The root holds -1.
        int parent = -1;
        /// @brief Component name to component document, as reflect::to_json writes it.
        nlohmann::json components = nlohmann::json::object();
    };

    /**
     * @brief A named scene fragment that many instances share.
     *
     * Entity 0 is the root, and it is the only root. Every other entity names a
     * parent that comes before it in the list. That order lets an instance build
     * in one forward pass, and it rules out a cycle without a search.
     */
    class Prefab {
    public:
        /// @brief The name a scene file stores.
        /// @return The name.
        [[nodiscard]] const std::string& name() const { return name_; }

        /// @brief The entities, root first.
        /// @return The entity list.
        [[nodiscard]] const std::vector<PrefabEntity>& entities() const { return entities_; }

        /// @brief How many entities one instance creates.
        /// @return The count.
        [[nodiscard]] std::size_t size() const { return entities_.size(); }

        /**
         * @brief Reads a prefab out of a JSON document.
         *
         * The document holds a version and an array of entities, in the same
         * shape a scene file uses.
         *
         * @param name The name to store the prefab under.
         * @param document The document to read.
         * @param out The prefab to fill. Untouched when the read fails.
         * @return True when the document is a prefab this build can use.
         */
        [[nodiscard]] static bool parse(std::string name, const nlohmann::json& document,
                                        Prefab& out);

    private:
        std::string name_;
        std::vector<PrefabEntity> entities_;
    };

    /**
     * @brief Marks the root entity of a prefab instance.
     *
     * A scene file writes this entity as a link and a patch, and it writes none
     * of the descendants. This component is structure, not content, so it stays
     * out of the component registry in the same way Hierarchy does.
     */
    struct PrefabInstance {
        /// @brief The name to look up in the library.
        std::string prefab;
    };

    /**
     * @brief Links one live entity back to the prefab entity it came from.
     *
     * Every entity of an instance carries this, including the root. The save
     * step reads it to work out which fields the instance changed.
     */
    struct PrefabMember {
        /// @brief The root of the instance this entity belongs to.
        entt::entity root = entt::null;
        /// @brief Which prefab entity this came from.
        std::size_t index = 0;
    };

    /**
     * @brief The prefabs a scene file can name.
     *
     * @code
     * engine::scene::PrefabLibrary library;
     * library.add_file("crate", "content/crate.prefab");
     * @endcode
     */
    class PrefabLibrary {
    public:
        /**
         * @brief Reads a prefab from a document and stores it under a name.
         *
         * A second prefab with the same name replaces the first, so a hot
         * reload does not need a remove call.
         *
         * @param name The name a scene file uses.
         * @param document The prefab document.
         * @return True when the document parsed.
         */
        [[nodiscard]] bool add(std::string name, const nlohmann::json& document);

        /**
         * @brief Reads a prefab from a `.prefab` file.
         * @param name The name a scene file uses.
         * @param path The file to read.
         * @return True when the file parsed.
         */
        [[nodiscard]] bool add_file(std::string name, const std::filesystem::path& path);

        /**
         * @brief Looks a prefab up by name.
         * @param name The name to find.
         * @return The prefab, or nullptr when the library does not hold it.
         */
        [[nodiscard]] const Prefab* find(std::string_view name) const;

        /// @brief How many prefabs the library holds.
        /// @return The count.
        [[nodiscard]] std::size_t size() const { return entries_.size(); }

        /// @brief Forgets every prefab.
        void clear() { entries_.clear(); }

    private:
        std::vector<Prefab> entries_;
    };

    /**
     * @brief The library a scene file uses when the caller names no other.
     * @return The process-wide library. It lives until the program ends.
     */
    [[nodiscard]] PrefabLibrary& prefabs();

    /**
     * @brief Builds one instance of a prefab in a world.
     *
     * The instance root comes back with no parent. Attach it where you want it.
     *
     * @param world The world to build in.
     * @param prefab The template to copy.
     * @param record What this instance changes about the prefab. Pass an empty
     * object for an instance that follows it exactly. Four keys are read, and
     * every one is optional:
     * - @ref kOverridesKey, the fields it changed, keyed by entity index
     *   written as a decimal string, each value a merge patch over that
     *   entity's component set.
     * - @ref kRemovedKey, the members it destroyed.
     * - @ref kAddedKey, the entities it added under itself.
     * - @ref kReparentedKey, the members it moved.
     *
     * @param registry The component types to build. A component the registry
     * does not know is a warning, in the same way a scene file treats one.
     * @return The instance root, or `entt::null` when the build failed. On
     * failure nothing is left behind in the world.
     *
     * @code
     * nlohmann::json record;
     * record["overrides"]["0"]["Transform"]["position"] = { 3.0F, 0.0F, 0.0F };
     * const entt::entity crate = instantiate(world, *library.find("crate"), record);
     * @endcode
     */
    [[nodiscard]] entt::entity instantiate(World& world, const Prefab& prefab,
                                           const nlohmann::json& record,
                                           const ComponentRegistry& registry = components());

    /**
     * @brief Works out the merge patch that turns one document into another.
     *
     * The result follows RFC 7386. A key the two documents agree on is left
     * out, so a patch names only what changed. A key the live document dropped
     * comes back as null, which is how a merge patch says "remove this".
     *
     * The walk goes into a nested object, so two components that differ in one
     * field produce a patch that names that one field. It does not go into an
     * array, because half of a vector is not a useful override.
     *
     * @param base What the prefab holds.
     * @param live What the instance holds now.
     * @return The patch. An empty object means the two agree.
     */
    [[nodiscard]] nlohmann::json override_patch(const nlohmann::json& base,
                                                const nlohmann::json& live);

    /**
     * @brief Works out everything an instance changed, fields and shape both.
     *
     * This is what a scene file stores under a prefab instance, and what
     * instantiate() reads back. It holds the field overrides, the members the
     * instance destroyed, the entities it added, and the members it moved.
     * Every one of those keys is left out when there is nothing to say, so an
     * instance that follows its prefab writes an empty object.
     *
     * @param world The world holding the instance.
     * @param root The instance root. It must carry a PrefabInstance.
     * @param prefab The template the instance came from.
     * @param registry The component types to compare and to write.
     * @return The record, as instantiate() takes it.
     *
     * @warning A member dragged out of the instance reads as destroyed, because
     * from the root there is no longer any way to tell the two apart.
     */
    [[nodiscard]] nlohmann::json instance_record(const World& world, entt::entity root,
                                                 const Prefab& prefab,
                                                 const ComponentRegistry& registry = components());

    /**
     * @brief Works out every field an instance changed since it was built.
     *
     * This is the field half of instance_record(), and it says nothing about
     * the shape. A caller writing a scene wants the whole record. This one is
     * for a caller that only wants to know which fields moved.
     *
     * @param world The world holding the instance.
     * @param root The instance root. It must carry a PrefabInstance.
     * @param prefab The template the instance came from.
     * @param registry The component types to compare.
     * @return An object keyed by entity index, which is what the
     * @ref kOverridesKey of an instance record holds. An empty object means
     * the instance still matches the prefab field for field.
     */
    [[nodiscard]] nlohmann::json instance_overrides(
        const World& world, entt::entity root, const Prefab& prefab,
        const ComponentRegistry& registry = components());

} // namespace engine::scene
