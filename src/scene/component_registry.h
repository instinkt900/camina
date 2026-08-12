#pragma once

/**
 * @file
 * @brief Turns a component type name in a file back into a live component.
 *
 * A scene file names its components. A reader holds only that name, and C++
 * cannot build a type from a string. This registry closes the gap: registering
 * a type stores function pointers that already know the type, and the caller
 * reaches them through the name.
 *
 * The functions are thin. Each one calls reflect/json.h or reflect/inspector.h,
 * so this adds no second descriptor system and rule 4.5 holds.
 */

#include "core/entt.h"
#include "reflect/inspector.h"
#include "reflect/json.h"
#include "reflect/reflect.h"

#include <entt/entity/registry.hpp>
#include <nlohmann/json.hpp>

#include <cstddef>
#include <string_view>
#include <vector>

namespace engine::scene {

    /**
     * @brief What a scene file and an editor need to do with one component type.
     *
     * The functions carry the type inside them, so the caller works with a name
     * and an entity and never names a component type.
     */
    struct ComponentOps {
        /// @brief The name the scene file stores. It comes from Describe<T>::name.
        const char* name = "";

        /// @brief Whether an entity carries this component.
        bool (*has)(const entt::registry& registry, entt::entity entity) = nullptr;

        /// @brief Reads the component off an entity and writes it to JSON.
        nlohmann::json (*save)(const entt::registry& registry, entt::entity entity) = nullptr;

        /// @brief Builds the component on an entity from JSON. Replaces any earlier one.
        bool (*load)(entt::registry& registry, entt::entity entity,
                     const nlohmann::json& document) = nullptr;

        /**
         * @brief Draws the component in the inspector and reports a change.
         *
         * Call this between ImGui::Begin() and ImGui::End(). It returns true
         * when the user changed a field, and the caller decides what that
         * means. A Transform changed this way needs World::mark_dirty(),
         * because the change went around set_local().
         */
        bool (*inspect)(entt::registry& registry, entt::entity entity) = nullptr;

        /// @brief The field names that carry AssetRef, or empty.
        std::vector<const char*> reference_field_names;
    };

    /**
     * @brief The component types a scene file can carry.
     *
     * A type that is not registered is not saved and not loaded. That is the
     * switch for a component the program derives at load time, in the same way
     * the Transient attribute works for one field.
     *
     * @code
     * engine::scene::components().add<engine::Transform>();
     * engine::scene::components().add<engine::scene::Name>();
     * @endcode
     */
    class ComponentRegistry {
    public:
        /**
         * @brief Registers a described component type.
         *
         * Registering the same type twice does nothing, so a caller does not
         * have to guard the call.
         *
         * @tparam T A described component type.
         */
        template <reflect::Described T>
        void add() {
            if (find(reflect::type_name<T>()) != nullptr) {
                return;
            }

            ComponentOps ops;
            ops.name = reflect::type_name<T>();
            ops.has = [](const entt::registry& registry, entt::entity entity) {
                return registry.all_of<T>(entity);
            };
            ops.save = [](const entt::registry& registry, entt::entity entity) {
                return reflect::to_json(registry.get<T>(entity));
            };
            ops.load = [](entt::registry& registry, entt::entity entity,
                          const nlohmann::json& document) {
                T value;
                if (!reflect::from_json(document, value)) {
                    return false;
                }
                registry.emplace_or_replace<T>(entity, std::move(value));
                return true;
            };
            ops.inspect = [](entt::registry& registry, entt::entity entity) {
                return reflect::inspect(registry.get<T>(entity));
            };
            if constexpr (reflect::field_count<T>() > 0) {
                // The descriptor walk, because only the name and the attribute
                // are wanted here. This used to build a throwaway instance,
                // which cost work for nothing and needed a default constructor.
                reflect::for_each_field_descriptor<T>([&](const auto& field) {
                    if constexpr (reflect::has_attribute_v<reflect::AssetRef,
                                                           decltype(field)>) {
                        ops.reference_field_names.push_back(field.name());
                    }
                });
            }
            entries_.push_back(ops);
        }

        /**
         * @brief Looks up a component type by the name a file stores.
         * @param name The name to find.
         * @return The entry, or nullptr when nothing registered that name.
         */
        [[nodiscard]] const ComponentOps* find(std::string_view name) const;

        /// @brief Every registered type, in registration order.
        /// @return The entries. The order decides the order a file stores them in.
        [[nodiscard]] const std::vector<ComponentOps>& all() const { return entries_; }

        /// @brief How many types are registered.
        /// @return The count.
        [[nodiscard]] std::size_t size() const { return entries_.size(); }

        /// @brief Forgets every registration. Written for tests.
        void clear() { entries_.clear(); }

    private:
        std::vector<ComponentOps> entries_;
    };

    /**
     * @brief The registry a scene file uses when the caller names no other.
     *
     * @return The process-wide registry. It lives until the program ends.
     */
    [[nodiscard]] ComponentRegistry& components();

    /**
     * @brief Writes every registered component an entity carries.
     *
     * A scene file and a prefab both store an entity this way, so they agree on
     * the shape without either one repeating the loop.
     *
     * @param registry The entity registry to read.
     * @param entity The entity to read.
     * @param types The component types to consider.
     * @return An object keyed by component name. It is empty when the entity
     * carries no registered component.
     */
    [[nodiscard]] nlohmann::json save_components(const entt::registry& registry,
                                                 entt::entity entity,
                                                 const ComponentRegistry& types = components());

    /**
     * @brief Builds every component an object names and the registry knows.
     *
     * A component the registry does not know is a warning, not a failure. The
     * rest of the entity still loads, so an older build can open a newer file.
     *
     * @param parts An object keyed by component name, as save_components writes.
     * @param registry The entity registry to fill.
     * @param entity The entity to build on.
     * @param types The component types to consider.
     * @param where What to name in a log line, for example "entity 3".
     * @return True when @p parts is an object and every known component loaded.
     */
    [[nodiscard]] bool load_components(const nlohmann::json& parts, entt::registry& registry,
                                       entt::entity entity, const ComponentRegistry& types,
                                       std::string_view where);

    /**
     * @brief Registers the component types the engine itself defines.
     *
     * Call this once at startup, before you read a scene. A game adds its own
     * types after.
     *
     * @param registry The registry to fill. Defaults to the process-wide one.
     */
    void register_builtin_components(ComponentRegistry& registry = components());

} // namespace engine::scene
