#pragma once

/**
 * @file
 * @brief A runtime lookup from a type name to what the type described.
 *
 * A consumer that knows the type at compile time does not need this. It calls
 * for_each_field() directly. The registry serves the cases where the name
 * arrives as data: a scene file naming a component, or a script naming a type.
 *
 * The entries hold counts and sizes only. Type-erased field access arrives when
 * a consumer needs it, per rule 4.6 in DESIGN.md. M3 prefab overrides is the
 * likely first caller.
 */

#include "reflect/reflect.h"

#include <cstddef>
#include <string_view>
#include <vector>

namespace engine::reflect {

    /// @brief What the registry records about one described type.
    struct TypeInfo {
        const char* name = "";       ///< The name from the Describe specialization.
        std::size_t size = 0;        ///< `sizeof` the type, in bytes.
        std::size_t field_count = 0; ///< How many fields the type describes.
    };

    /**
     * @brief Holds the described types that a program registered.
     *
     * A registry is an ordinary object. Use registry() for the process-wide one,
     * or build a local one in a test.
     */
    class Registry {
    public:
        /**
         * @brief Records a described type.
         *
         * Registering the same name twice does nothing, so a header included in
         * two translation units stays safe.
         *
         * @tparam T A described type.
         */
        template <Described T>
        void add() {
            if (find(type_name<T>()) != nullptr) {
                return;
            }
            types_.push_back(TypeInfo{ type_name<T>(), sizeof(T), field_count<T>() });
        }

        /**
         * @brief Looks a type up by the name it was described with.
         * @param name The name to find.
         * @return The entry, or nullptr when no type registered under that name.
         * @warning The pointer becomes stale after the next add(). Copy what you need.
         */
        [[nodiscard]] const TypeInfo* find(std::string_view name) const;

        /// @brief Every registered type, in the order they were added.
        /// @return The entries.
        [[nodiscard]] const std::vector<TypeInfo>& types() const { return types_; }

        /// @brief How many types are registered.
        /// @return The count.
        [[nodiscard]] std::size_t size() const { return types_.size(); }

        /// @brief Forgets every registered type.
        void clear() { types_.clear(); }

    private:
        std::vector<TypeInfo> types_;
    };

    /**
     * @brief The process-wide registry.
     * @return The single instance, built on the first call.
     */
    [[nodiscard]] Registry& registry();

} // namespace engine::reflect
