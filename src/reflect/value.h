#pragma once

/**
 * @file
 * @brief One field's value, carried without naming its type.
 *
 * Every consumer so far knew each field type at compile time. The inspector
 * draws a widget for it, and the serializer writes JSON for it. Both reach the
 * type through `for_each_field()`, inside a template.
 *
 * A script does not. It arrives with two strings, a component name and a field
 * name, and neither one is a type. So something has to carry a value across
 * that boundary, and this is that something.
 *
 * **This is not a second descriptor system, and rule 4.5 still holds.** A Value
 * describes no type and stores no field list. It is one value in transit, filled
 * by walking the same `Describe<T>` every other consumer walks.
 *
 * The type is deliberately plain rather than a variant. A caller reads `kind`
 * and then the one member that goes with it, which is easy to write a binding
 * against and easy to read in a debugger. It names no script type and no JSON
 * type, so `reflect/` gains no dependency on either.
 */

#include "math/conventions.h"
#include "reflect/reflect.h"
#include "reflect/traits.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>

namespace engine::reflect {

    /**
     * @brief Which member of a Value carries the value.
     *
     * The set is what the sandbox needs, per rule 4.6, and no more. A field
     * whose type is not in this list reports Unsupported rather than a wrong
     * value, so a caller can say so instead of silently reading a zero.
     */
    enum class ValueKind : std::uint8_t {
        None,        ///< Nothing was read. The field does not exist.
        Unsupported, ///< The field exists and its type does not fit a Value yet.
        Bool,        ///< `boolean` carries it.
        Number,      ///< `number` carries it. Every arithmetic field arrives here.
        Text,        ///< `text` carries it. A std::string, or a TextValue such as a Guid.
        Enum,        ///< `text` carries the enumerator name, not the number.
        Vec2,        ///< `vector` carries it, in x and y.
        Vec3,        ///< `vector` carries it, in x, y and z.
        Vec4,        ///< `vector` carries all four.
        Quat,        ///< `quat` carries it, in wxyz order per DESIGN.md section 3.
    };

    /**
     * @brief One field value, and which member holds it.
     *
     * @warning Read @ref kind before any member. The others hold whatever the
     * last write left, and only the one @ref kind names is meaningful.
     *
     * Every arithmetic field arrives as a Number, whatever its width. A double
     * holds every value a float, an int, or a smaller integer can, so nothing is
     * lost on the way out. Writing back narrows once, at the field, where the
     * type is known again.
     *
     * @code
     * engine::reflect::Value value;
     * if (info->get_field(&transform, "position", value) &&
     *     value.kind == engine::reflect::ValueKind::Vec3) {
     *     use(value.vector.x, value.vector.y, value.vector.z);
     * }
     * @endcode
     */
    struct Value {
        /// @brief Which member below carries the value.
        ValueKind kind = ValueKind::None;

        /// @brief The value when kind is Bool.
        bool boolean = false;

        /// @brief The value when kind is Number.
        double number = 0.0;

        /// @brief The value when kind is Text, or the enumerator name when it is Enum.
        std::string text;

        /// @brief The value when kind is Vec2, Vec3 or Vec4. Unused parts are zero.
        Vec4 vector{ 0.0F, 0.0F, 0.0F, 0.0F };

        /// @brief The value when kind is Quat, in wxyz order.
        Quat quat{ 1.0F, 0.0F, 0.0F, 0.0F };
    };

    /**
     * @brief Which vector kind a glm length maps to.
     *
     * A function rather than a chain of conditional operators, which
     * clang-tidy refuses to nest and which reads worse than this does.
     *
     * @param length How many components the vector has.
     * @return The matching kind, or Unsupported for a length no Value carries.
     */
    [[nodiscard]] constexpr ValueKind vector_kind(glm::length_t length) {
        switch (length) {
        case 2:
            return ValueKind::Vec2;
        case 3:
            return ValueKind::Vec3;
        case 4:
            return ValueKind::Vec4;
        default:
            return ValueKind::Unsupported;
        }
    }

    /**
     * @brief Copies one typed field into a Value.
     *
     * The branches are the same ones, in the same order, that `reflect/json.h`
     * uses to write a field. Keeping the order together is what stops the two
     * consumers disagreeing about what a type is: a `Guid` declares `to_text`
     * and must reach the text branch before anything else claims it.
     *
     * A type this cannot carry sets ValueKind::Unsupported rather than a zero,
     * so the caller reports it instead of reading a value that was never there.
     *
     * @tparam V The field type, deduced.
     * @param in The field to read.
     * @param out Receives the value and its kind.
     */
    template <typename V>
    void to_value(const V& in, Value& out) {
        using Field = std::remove_cvref_t<V>;

        if constexpr (TextValue<Field>) {
            out.kind = ValueKind::Text;
            out.text = to_text(in);
        } else if constexpr (DescribedEnum<Field>) {
            // The name and not the number, for the reason json.h gives: a
            // number means something else the moment somebody inserts an
            // enumerator above it, and nothing warns.
            const char* name = enumerator_name(in);
            out.kind = name == nullptr ? ValueKind::Unsupported : ValueKind::Enum;
            out.text = name == nullptr ? "" : name;
        } else if constexpr (GlmVector<Field>::value) {
            constexpr glm::length_t kLength = GlmVector<Field>::length;
            out.kind = vector_kind(kLength);
            out.vector = Vec4{ 0.0F, 0.0F, 0.0F, 0.0F };
            if constexpr (vector_kind(kLength) != ValueKind::Unsupported) {
                for (glm::length_t i = 0; i < kLength; ++i) {
                    out.vector[i] = static_cast<float>(in[i]);
                }
            }
        } else if constexpr (GlmQuat<Field>::value) {
            out.kind = ValueKind::Quat;
            out.quat = in;
        } else if constexpr (std::same_as<Field, std::string>) {
            out.kind = ValueKind::Text;
            out.text = in;
        } else if constexpr (std::same_as<Field, bool>) {
            out.kind = ValueKind::Bool;
            out.boolean = in;
        } else if constexpr (std::is_arithmetic_v<Field>) {
            // Every width collapses to double, which holds each of them without
            // loss. The narrowing happens in from_value(), at the field, where
            // the type is known again.
            out.kind = ValueKind::Number;
            out.number = static_cast<double>(in);
        } else if constexpr (std::is_enum_v<Field>) {
            // An enum that describes itself no way at all. The number is all
            // there is.
            out.kind = ValueKind::Number;
            out.number = static_cast<double>(static_cast<std::underlying_type_t<Field>>(in));
        } else {
            // A nested described struct or a list. Neither reaches a script
            // today. See issue #271.
            out.kind = ValueKind::Unsupported;
        }
    }

    /**
     * @brief Writes a Value into a described enum field.
     *
     * Split out of from_value() so that neither function grows past the
     * cognitive complexity the lint gate allows. See `cmake/ClangTidy.cmake`.
     *
     * @tparam E The enum type, deduced.
     * @param in The value to write.
     * @param out The field to fill.
     * @return True when the value named an enumerator, or carried a number.
     */
    template <DescribedEnum E>
    [[nodiscard]] bool enum_from_value(const Value& in, E& out) {
        // The name is the spelling a person chose, so a wrong one is a mistake
        // rather than a gap in the description.
        if (in.kind == ValueKind::Enum || in.kind == ValueKind::Text) {
            return enumerator_value(in.text, out);
        }
        // A number is taken as well, because a caller that read one back has to
        // be able to write it again.
        if (in.kind == ValueKind::Number) {
            out = static_cast<E>(static_cast<std::underlying_type_t<E>>(in.number));
            return true;
        }
        return false;
    }

    /**
     * @brief Writes a Value into a glm vector field.
     *
     * Split out of from_value() for the same reason enum_from_value() is.
     *
     * @tparam V The vector type, deduced.
     * @param in The value to write.
     * @param out The field to fill.
     * @return True when the kind matched the length of the vector.
     */
    template <typename V>
        requires GlmVector<V>::value
    [[nodiscard]] bool vector_from_value(const Value& in, V& out) {
        constexpr glm::length_t kLength = GlmVector<V>::length;
        constexpr ValueKind kWanted = vector_kind(kLength);

        if constexpr (kWanted == ValueKind::Unsupported) {
            return false;
        } else {
            if (in.kind != kWanted) {
                return false;
            }
            for (glm::length_t i = 0; i < kLength; ++i) {
                out[i] = static_cast<typename GlmVector<V>::Element>(in.vector[i]);
            }
            return true;
        }
    }

    /**
     * @brief Writes a Value back into one typed field.
     *
     * A kind that does not match the field leaves @p out alone and reports
     * false, the same way `from_text` leaves a value alone when it rejects the
     * text. A caller that half-wrote a component would be worse than one that
     * refused.
     *
     * @tparam V The field type, deduced.
     * @param in The value to write.
     * @param out The field to fill.
     * @return True when the value fitted the field.
     */
    template <typename V>
    [[nodiscard]] bool from_value(const Value& in, V& out) {
        using Field = std::remove_cvref_t<V>;

        if constexpr (TextValue<Field>) {
            return in.kind == ValueKind::Text && from_text(in.text, out);
        } else if constexpr (DescribedEnum<Field>) {
            return enum_from_value(in, out);
        } else if constexpr (GlmVector<Field>::value) {
            return vector_from_value(in, out);
        } else if constexpr (GlmQuat<Field>::value) {
            if (in.kind != ValueKind::Quat) {
                return false;
            }
            out = in.quat;
            return true;
        } else if constexpr (std::same_as<Field, std::string>) {
            if (in.kind != ValueKind::Text && in.kind != ValueKind::Enum) {
                return false;
            }
            out = in.text;
            return true;
        } else if constexpr (std::same_as<Field, bool>) {
            if (in.kind != ValueKind::Bool) {
                return false;
            }
            out = in.boolean;
            return true;
        } else if constexpr (std::is_arithmetic_v<Field>) {
            if (in.kind != ValueKind::Number) {
                return false;
            }
            out = static_cast<Field>(in.number);
            return true;
        } else if constexpr (std::is_enum_v<Field>) {
            if (in.kind != ValueKind::Number) {
                return false;
            }
            out = static_cast<Field>(static_cast<std::underlying_type_t<Field>>(in.number));
            return true;
        } else {
            return false;
        }
    }

} // namespace engine::reflect
