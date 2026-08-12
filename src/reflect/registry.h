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
#include "reflect/value.h"

#include <cstddef>
#include <string_view>
#include <vector>

namespace engine::reflect {

    /// @brief What the registry records about one described type.
    struct TypeInfo {
        const char* name = "";       ///< The name from the Describe specialization.
        std::size_t size = 0;        ///< `sizeof` the type, in bytes.
        std::size_t field_count = 0; ///< How many fields the type describes.

        /**
         * @brief A callback that receives one field from a described type.
         *
         * @param name       The field name.
         * @param type_name  A readable name for the field type, or "".
         * @param offset     Byte offset of the field inside the struct.
         * @param size       Byte size of the field.
         * @param user       Whatever the caller passed.
         */
        using FieldVisitor = void (*)(const char* name, const char* type_name,
                                      std::size_t offset, std::size_t size, void* user);

        /**
         * @brief Walks every described field of @p instance, calling @p visit.
         *
         * nullptr when the type was not registered through add() or when the
         * type has no fields.
         *
         * @param instance A pointer to a live instance of this type. The caller
         * knows the type from the name and casts it.
         * @param visit    Called once for each described field.
         * @param user     Passed through to every call of @p visit.
         */
        void (*walk_fields)(void* instance, FieldVisitor visit, void* user) = nullptr;

        /**
         * @brief Reads one field by name into a Value.
         *
         * This is what a consumer that holds only strings needs. A script
         * arrives with a component name and a field name, and C++ can turn
         * neither one into a type.
         *
         * @param instance A pointer to a live instance of this type.
         * @param field The field name, as Describe gave it.
         * @param out Receives the value. Its kind is None when no field has
         * that name, and Unsupported when the field exists and its type does
         * not fit a Value. See issue #271.
         * @return True when a field of that name was read.
         */
        bool (*get_field)(const void* instance, std::string_view field, Value& out) = nullptr;

        /**
         * @brief Writes one field by name from a Value.
         *
         * A value whose kind does not match the field leaves the field alone
         * and reports false. Half-writing a component is worse than refusing.
         *
         * @param instance A pointer to a live instance of this type.
         * @param field The field name, as Describe gave it.
         * @param in The value to write.
         * @return True when the field was found and the value fitted it.
         *
         * @warning A Transform written this way goes around
         * `scene::World::set_local()`, so the caller has to call
         * `scene::World::mark_dirty()` or the world matrix stays stale.
         */
        bool (*set_field)(void* instance, std::string_view field, const Value& in) = nullptr;

        /// @brief Every described field name, in the order Describe gave them.
        std::vector<const char*> field_names;
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
            // Set member by member rather than braced, because the struct has
            // grown past the point where a reader can match a brace list to it.
            TypeInfo info;
            info.name = type_name<T>();
            info.size = sizeof(T);
            info.field_count = field_count<T>();

            if constexpr (field_count<T>() > 0) {
                // Read once here rather than on each lookup, because a caller
                // that lists the fields of a type does not want to build an
                // instance to do it.
                //
                // The descriptor walk takes no object. A throwaway instance
                // would cost work for nothing, and it would stop a described
                // type with no default constructor registering at all.
                for_each_field_descriptor<T>(
                    [&](const auto& field) { info.field_names.push_back(field.name()); });

                info.get_field = [](const void* instance, std::string_view field,
                                    Value& out) {
                    // Cleared first, so a caller that reuses one Value across
                    // reads sees None on a miss rather than the last read.
                    out = Value{};

                    const T& object = *static_cast<const T*>(instance);
                    bool found = false;
                    for_each_field(object, [&](const auto& described, const auto& value) {
                        if (!found && field == described.name()) {
                            found = true;
                            to_value(value, out);
                        }
                    });
                    return found;
                };

                info.set_field = [](void* instance, std::string_view field,
                                    const Value& in) {
                    T& object = *static_cast<T*>(instance);
                    bool written = false;
                    for_each_field(object, [&](const auto& described, auto& value) {
                        if (!written && field == described.name()) {
                            written = from_value(in, value);
                        }
                    });
                    return written;
                };

                info.walk_fields = [](void* instance, TypeInfo::FieldVisitor visit,
                                      void* user) {
                    T& object = *static_cast<T*>(instance);
                    for_each_field(object, [&](const auto& field, const auto& value) {
                        // A pointer difference is signed. This one cannot be
                        // negative, because value is a member of object, so the
                        // cast says so rather than leaving the conversion
                        // implicit. That warning was counted once for every
                        // translation unit and was most of issue #179.
                        const auto offset = static_cast<std::size_t>(
                            reinterpret_cast<const char*>(&value) -
                            reinterpret_cast<const char*>(&object));
                        visit(field.name(), "", offset, sizeof(value), user);
                    });
                };
            }
            types_.push_back(info);
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
