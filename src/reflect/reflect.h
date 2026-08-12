#pragma once

/**
 * @file
 * @brief Field descriptors, `describe<T>()`, and the field visitor.
 *
 * DESIGN.md section 7 sets the approach. A type describes itself once, and the
 * inspector, the serializer, the script binding, and prefab overrides all read
 * that one description.
 *
 * The descriptors are hand-written today. A later libclang step over annotated
 * headers can generate the same specializations. That move must not change a
 * consumer, so a consumer only ever calls for_each_field(), field_count(), and
 * type_name(). Nothing outside this header should touch the tuple.
 */

#include "reflect/attributes.h"

#include <concepts>
#include <cstddef>
#include <string_view>
#include <tuple>
#include <type_traits>

namespace engine::reflect {

    /**
     * @brief One field of a type, with its name, its address, and its attributes.
     *
     * Build one with field() rather than by hand. The type parameters are
     * deduced there.
     *
     * @tparam Class The type that owns the field.
     * @tparam Member The type of the field.
     * @tparam Attributes The attribute types this field carries. See attributes.h.
     */
    template <typename Class, typename Member, typename... Attributes>
    class Field {
    public:
        /// @brief The type that owns the field.
        using ClassType = Class;
        /// @brief The type of the field.
        using MemberType = Member;

        /**
         * @brief Builds a descriptor. Prefer field(), which deduces the types.
         * @param name The field name, as a static string.
         * @param pointer A pointer to the member.
         * @param attributes Zero or more attributes.
         */
        constexpr Field(const char* name, Member Class::* pointer, Attributes... attributes)
            : name_(name)
            , pointer_(pointer)
            , attributes_(attributes...) {}

        /// @brief The field name.
        /// @return A static string. The descriptor does not own it.
        [[nodiscard]] constexpr const char* name() const { return name_; }

        /**
         * @brief Reads the field out of an object.
         * @param object The object to read from.
         * @return A reference to the field inside @p object.
         */
        [[nodiscard]] constexpr const Member& get(const Class& object) const {
            return object.*pointer_;
        }

        /**
         * @brief Reaches the field inside an object so a caller can change it.
         * @param object The object to reach into.
         * @return A reference to the field inside @p object.
         */
        [[nodiscard]] constexpr Member& get(Class& object) const { return object.*pointer_; }

        /**
         * @brief Whether the field carries an attribute.
         *
         * This is static on purpose. A consumer needs the answer inside
         * `if constexpr`, and a constexpr member call on a runtime reference is
         * not a constant expression. Prefer has_attribute_v, which reads better
         * at a call site.
         *
         * @tparam Attribute The attribute type to look for.
         * @return True when the field was declared with that attribute.
         */
        template <typename Attribute>
        [[nodiscard]] static constexpr bool has() {
            return (std::is_same_v<Attribute, Attributes> || ...);
        }

        /**
         * @brief Reads an attribute the field carries.
         *
         * Guard the call with has(), because asking for an absent attribute
         * fails the build.
         *
         * @tparam Attribute The attribute type to read.
         * @return A copy of the attribute. They are small plain structs.
         */
        template <typename Attribute>
        [[nodiscard]] constexpr Attribute attribute() const {
            static_assert(has<Attribute>(), "This field does not carry that attribute.");
            return std::get<Attribute>(attributes_);
        }

    private:
        const char* name_;
        Member Class::* pointer_;
        std::tuple<Attributes...> attributes_;
    };

    /**
     * @brief Builds a field descriptor with the types deduced.
     *
     * @tparam Class The type that owns the field.
     * @tparam Member The type of the field.
     * @tparam Attributes The attribute types, deduced from the arguments.
     * @param name The field name, as a static string.
     * @param pointer A pointer to the member.
     * @param attributes Zero or more attributes from attributes.h.
     * @return The descriptor.
     *
     * @code
     * field("health", &Player::health, Range{ 0.0, 100.0, 1.0 })
     * @endcode
     */
    template <typename Class, typename Member, typename... Attributes>
    [[nodiscard]] constexpr auto field(const char* name, Member Class::* pointer,
                                       Attributes... attributes) {
        return Field<Class, Member, Attributes...>(name, pointer, attributes...);
    }

    /**
     * @brief Whether a field descriptor carries an attribute.
     *
     * Pass `decltype(field)` from inside a visitor. This works in `if constexpr`,
     * because it asks the type rather than the object.
     *
     * @tparam Attribute The attribute type to look for.
     * @tparam FieldType The descriptor type, usually `decltype(field)`.
     *
     * @code
     * if constexpr (!has_attribute_v<Transient, decltype(field)>) {
     *     write(field.name(), value);
     * }
     * @endcode
     */
    template <typename Attribute, typename FieldType>
    inline constexpr bool has_attribute_v =
        std::remove_cvref_t<FieldType>::template has<Attribute>();

    /**
     * @brief The description of a type. Specialize this for each reflected type.
     *
     * The primary template is deliberately empty, so asking about an undescribed
     * type fails the build with a clear message instead of silently reflecting
     * nothing.
     *
     * @tparam T The type being described.
     *
     * @code
     * template <>
     * struct engine::reflect::Describe<Player> {
     *     static constexpr const char* name = "Player";
     *     static constexpr auto fields() {
     *         return std::make_tuple(
     *             field("health", &Player::health, Range{ 0.0, 100.0, 1.0 }),
     *             field("cache", &Player::cache, Transient{}));
     *     }
     * };
     * @endcode
     */
    template <typename T>
    struct Describe;

    /**
     * @brief Whether a type has a Describe specialization.
     *
     * This asks for `fields()`, so it covers a struct and never an enum. An enum
     * describes itself with `enumerators()` instead, and DescribedEnum asks for
     * that. A consumer that walks fields therefore cannot pick up an enum by
     * accident.
     *
     * @tparam T The type to test.
     */
    template <typename T>
    concept Described = requires {
        { Describe<T>::name } -> std::convertible_to<const char*>;
        Describe<T>::fields();
    };

    /**
     * @brief One enumerator, with its name and its value.
     *
     * Build one with ENGINE_ENUMERATOR rather than by hand.
     *
     * @tparam E The enum type.
     */
    template <typename E>
    class Enumerator {
    public:
        /// @brief The enum this enumerator belongs to.
        using EnumType = E;

        /**
         * @brief Builds one. Prefer ENGINE_ENUMERATOR, which spells the name once.
         * @param name The enumerator name, as a static string.
         * @param value The value it stands for.
         */
        constexpr Enumerator(const char* name, E value)
            : name_(name)
            , value_(value) {}

        /// @brief The enumerator name.
        /// @return A static string. The descriptor does not own it.
        [[nodiscard]] constexpr const char* name() const { return name_; }

        /// @brief The value.
        /// @return The enumerator itself.
        [[nodiscard]] constexpr E value() const { return value_; }

    private:
        const char* name_;
        E value_;
    };

    /**
     * @brief Builds an enumerator descriptor with the type deduced.
     * @tparam E The enum type, deduced.
     * @param name The enumerator name, as a static string.
     * @param value The value it stands for.
     * @return The descriptor.
     */
    template <typename E>
    [[nodiscard]] constexpr auto enumerator(const char* name, E value) {
        return Enumerator<E>(name, value);
    }

    /**
     * @brief Whether an enum has a Describe specialization that lists its values.
     * @tparam T The type to test.
     */
    template <typename T>
    concept DescribedEnum = std::is_enum_v<T> && requires {
        { Describe<T>::name } -> std::convertible_to<const char*>;
        Describe<T>::enumerators();
    };

    /**
     * @brief How many enumerators an enum describes.
     * @tparam E A described enum.
     * @return The enumerator count.
     */
    template <DescribedEnum E>
    [[nodiscard]] constexpr std::size_t enumerator_count() {
        return std::tuple_size_v<decltype(Describe<E>::enumerators())>;
    }

    /**
     * @brief Calls a function once for each enumerator of an enum.
     *
     * This is the whole consumer interface, the way for_each_field() is for a
     * struct. A consumer that only uses this keeps working when the descriptors
     * move from hand-written to generated.
     *
     * @tparam E A described enum.
     * @tparam Fn The visitor type, deduced. It is taken by value, because it runs
     * once for each enumerator and so cannot be forwarded.
     * @param visit Called as `visit(enumerator)` for each one.
     *
     * @code
     * for_each_enumerator<BodyType>([](const auto& e) {
     *     printf("%s = %d\n", e.name(), static_cast<int>(e.value()));
     * });
     * @endcode
     */
    template <DescribedEnum E, typename Fn>
    constexpr void for_each_enumerator(Fn visit) {
        std::apply([&visit](const auto&... entries) { (visit(entries), ...); },
                   Describe<E>::enumerators());
    }

    /**
     * @brief The name of one enum value.
     *
     * @tparam E A described enum.
     * @param value The value to name.
     * @return The enumerator name, or nullptr when no enumerator has that value.
     * A caller that reached a value outside the description has a bug, or read a
     * file that a newer build wrote.
     */
    template <DescribedEnum E>
    [[nodiscard]] constexpr const char* enumerator_name(E value) {
        const char* found = nullptr;
        for_each_enumerator<E>([&](const auto& entry) {
            if (entry.value() == value) {
                found = entry.name();
            }
        });
        return found;
    }

    /**
     * @brief The value one enumerator name stands for.
     *
     * The comparison is exact, because these names come from a file or from a
     * script and a near miss is a mistake worth reporting rather than guessing at.
     *
     * @tparam E A described enum.
     * @param name The name to look up.
     * @param value Set to the value when the name matches one. Left alone
     *              otherwise, so a caller keeps whatever default it started with.
     * @return True when the name named an enumerator.
     */
    template <DescribedEnum E>
    [[nodiscard]] constexpr bool enumerator_value(std::string_view name, E& value) {
        bool found = false;
        for_each_enumerator<E>([&](const auto& entry) {
            if (!found && name == entry.name()) {
                value = entry.value();
                found = true;
            }
        });
        return found;
    }

    /**
     * @brief The name a type was described with.
     * @tparam T A described type.
     * @return A static string.
     */
    template <Described T>
    [[nodiscard]] constexpr const char* type_name() {
        return Describe<T>::name;
    }

    /**
     * @brief The name an enum was described with.
     *
     * This is the same call as the one above. The two concepts do not overlap, so
     * a caller writes type_name<T>() without caring which kind of type T is.
     *
     * @tparam T A described enum.
     * @return A static string.
     */
    template <DescribedEnum T>
    [[nodiscard]] constexpr const char* type_name() {
        return Describe<T>::name;
    }

    /**
     * @brief How many fields a type describes.
     * @tparam T A described type.
     * @return The field count.
     */
    template <Described T>
    [[nodiscard]] constexpr std::size_t field_count() {
        return std::tuple_size_v<decltype(Describe<T>::fields())>;
    }

    /**
     * @brief Calls a function once for each field of an object.
     *
     * This is the whole consumer interface. A consumer that only uses this keeps
     * working when the descriptors move from hand-written to generated.
     *
     * The function receives the descriptor and a reference to the value. The
     * reference is const when @p object is const, so a serializer and an editor
     * share one entry point.
     *
     * @tparam T The object type, deduced. Deducing `const` gives const values.
     * @tparam Fn The visitor type, deduced. It is taken by value, because it
     * runs once for each field and so cannot be forwarded.
     * @param object The object to walk.
     * @param visit Called as `visit(field, value)` for each field.
     *
     * @code
     * for_each_field(player, [](const auto& f, const auto& value) {
     *     if constexpr (!has_attribute_v<Transient, decltype(f)>) {
     *         write(f.name(), value);
     *     }
     * });
     * @endcode
     */
    template <typename T, typename Fn>
        requires Described<std::remove_cv_t<T>>
    constexpr void for_each_field(T& object, Fn visit) {
        std::apply([&](const auto&... fields) { (visit(fields, fields.get(object)), ...); },
                   Describe<std::remove_cv_t<T>>::fields());
    }

    /**
     * @brief Calls a function once for each field descriptor, with no instance.
     *
     * The same walk as for_each_field(), for a caller that wants the name or an
     * attribute rather than a value. A description is static, so asking what a
     * type has needs no object of that type.
     *
     * Use this rather than building a throwaway instance. A default-constructed
     * object costs work for nothing, and it stops a described type that has no
     * default constructor from registering at all.
     *
     * @tparam T A described type.
     * @tparam Fn The visitor type, deduced. Taken by value, because it runs once
     * for each field and so cannot be forwarded.
     * @param visit Called as `visit(field)` for each field.
     *
     * @code
     * for_each_field_descriptor<Player>([&](const auto& f) {
     *     names.push_back(f.name());
     * });
     * @endcode
     */
    template <Described T, typename Fn>
    constexpr void for_each_field_descriptor(Fn visit) {
        std::apply([&](const auto&... fields) { (visit(fields), ...); }, Describe<T>::fields());
    }

} // namespace engine::reflect

/**
 * @brief Names a field once instead of twice inside a Describe specialization.
 *
 * The field name and the member must agree, and writing both by hand invites a
 * typo that compiles. This macro derives the name from the member.
 *
 * @param Type The type that owns the field.
 * @param member The member name.
 *
 * @code
 * static constexpr auto fields() {
 *     return std::make_tuple(
 *         ENGINE_FIELD(Player, health, Range{ 0.0, 100.0, 1.0 }),
 *         ENGINE_FIELD(Player, cache, Transient{}));
 * }
 * @endcode
 */
#define ENGINE_FIELD(Type, member, ...) \
    ::engine::reflect::field(#member, &Type::member __VA_OPT__(, ) __VA_ARGS__)

/**
 * @brief Names an enumerator once instead of twice, the way ENGINE_FIELD does.
 *
 * The name reaches a scene file and the inspector, so a typo here is a typo a
 * person reads. Deriving it from the enumerator removes that.
 *
 * @param Type The enum type.
 * @param value The enumerator name.
 *
 * @code
 * static constexpr auto enumerators() {
 *     return std::make_tuple(
 *         ENGINE_ENUMERATOR(BodyType, Static),
 *         ENGINE_ENUMERATOR(BodyType, Dynamic));
 * }
 * @endcode
 */
#define ENGINE_ENUMERATOR(Type, value) ::engine::reflect::enumerator(#value, Type::value)
