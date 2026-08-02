#pragma once

/**
 * @file
 * @brief An ImGui editor generated from the field descriptors.
 *
 * This is the second of the two consumers DESIGN.md section 7 asks for. It
 * reads the same descriptors reflect/json.h reads. Neither consumer knows the
 * other exists, which is the point: a descriptor format that serves only one
 * consumer has not been tested.
 *
 * The header carries no ImGui type. inspector.cpp holds every ImGui call, so a
 * caller that only wants the serializer never compiles ImGui.
 *
 * Attributes this consumer honors:
 *
 * - `Range` sets slider bounds. Without it a field gets a drag box.
 * - `Tooltip` shows help when the pointer rests on the field.
 * - `Category` groups fields under a collapsing header.
 * - `Hidden` removes the field from the editor. Serialization still writes it.
 * - `ReadOnly` shows the field but does not let the user change it.
 */

#include "reflect/attributes.h"
#include "reflect/reflect.h"
#include "reflect/traits.h"

#include <cstdint>
#include <string>
#include <type_traits>
#include <vector>

namespace engine::reflect {

    /**
     * @brief The ImGui calls the inspector makes.
     *
     * These sit between the templates in this header and ImGui itself, so the
     * header stays free of ImGui types. Call inspect() rather than these.
     */
    namespace widget {

        /// @brief Which number a field holds, so one entry point covers them all.
        enum class Scalar : std::uint8_t {
            Int8,   ///< signed char
            UInt8,  ///< unsigned char
            Int16,  ///< short
            UInt16, ///< unsigned short
            Int32,  ///< int
            UInt32, ///< unsigned int
            Int64,  ///< long long
            UInt64, ///< unsigned long long
            Float,  ///< float
            Double, ///< double
        };

        /**
         * @brief Opens a collapsing node for a nested object or a list.
         * @param label The node text. It also gives the node its ID.
         * @return True when the node is open and the caller must draw the contents.
         */
        [[nodiscard]] bool begin_node(const char* label);

        /// @brief Closes the node begin_node() opened. Call it only when that returned true.
        void end_node();

        /**
         * @brief Opens a collapsing header for one Category.
         * @param name The category name.
         * @return True when the header is open and the caller must draw the fields.
         */
        [[nodiscard]] bool begin_category(const char* name);

        /// @brief Closes the header begin_category() opened.
        void end_category();

        /// @brief Grays out and locks everything drawn until end_disabled().
        void begin_disabled();

        /// @brief Ends the block begin_disabled() started.
        void end_disabled();

        /// @brief Attaches help text to the item drawn last.
        /// @param text The text to show when the pointer rests on the item.
        void tooltip(const char* text);

        /**
         * @brief Draws one or more numbers of the same type.
         *
         * A glm vector arrives here as @p count numbers laid out next to each
         * other, which is what glm guarantees for its vector types.
         *
         * @param label The field name.
         * @param type Which number type @p data points at.
         * @param data The first number. The caller owns it.
         * @param count How many numbers to draw. 1 for a scalar, 2 to 4 for a vector.
         * @param range The slider bounds, or nullptr for a drag box.
         * @return True when the user changed the value.
         */
        [[nodiscard]] bool edit_scalar(const char* label, Scalar type, void* data, int count,
                                       const Range* range);

        /// @brief Draws a checkbox.
        /// @param label The field name.
        /// @param value The value to show and change.
        /// @return True when the user changed the value.
        [[nodiscard]] bool edit_bool(const char* label, bool& value);

        /// @brief Draws a text box that grows with what the user types.
        /// @param label The field name.
        /// @param value The value to show and change.
        /// @return True when the user changed the value.
        [[nodiscard]] bool edit_string(const char* label, std::string& value);

        /**
         * @brief Draws a field the inspector cannot edit.
         *
         * The field still appears, so a missing widget is visible instead of
         * silent.
         *
         * @param label The field name.
         */
        void show_unhandled(const char* label);

        /// @brief Draws the "add" and "remove" buttons for a list.
        /// @param size The current element count. The caller applies the change.
        /// @return 1 to add an element, -1 to remove the last one, 0 to do nothing.
        [[nodiscard]] int list_buttons(std::size_t size);

        /**
         * @brief Scopes the widget IDs to one object.
         *
         * Two objects of the same type in one window hold the same field names.
         * Without this their widgets share an ID and fight over the keyboard
         * focus.
         *
         * @param object The address to scope by.
         */
        void push_id(const void* object);

        /// @brief Ends the scope push_id() started.
        void pop_id();

    } // namespace widget

    template <typename T>
        requires Described<std::remove_cv_t<T>>
    [[nodiscard]] bool inspect(T& object);

    /// @cond
    // The dispatch below is an implementation detail. Documenting each branch
    // would describe the C++ type system, not the interface.
    namespace detail {

        template <typename T>
        constexpr widget::Scalar scalar_of() {
            using Value = std::remove_cv_t<T>;
            if constexpr (std::is_same_v<Value, float>) {
                return widget::Scalar::Float;
            } else if constexpr (std::is_same_v<Value, double>) {
                return widget::Scalar::Double;
            } else if constexpr (std::is_signed_v<Value>) {
                if constexpr (sizeof(Value) == 1) {
                    return widget::Scalar::Int8;
                } else if constexpr (sizeof(Value) == 2) {
                    return widget::Scalar::Int16;
                } else if constexpr (sizeof(Value) == 4) {
                    return widget::Scalar::Int32;
                } else {
                    return widget::Scalar::Int64;
                }
            } else {
                if constexpr (sizeof(Value) == 1) {
                    return widget::Scalar::UInt8;
                } else if constexpr (sizeof(Value) == 2) {
                    return widget::Scalar::UInt16;
                } else if constexpr (sizeof(Value) == 4) {
                    return widget::Scalar::UInt32;
                } else {
                    return widget::Scalar::UInt64;
                }
            }
        }

        template <typename V>
        [[nodiscard]] bool inspect_value(const char* label, V& value, const Range* range) {
            using Value = std::remove_cvref_t<V>;

            if constexpr (Described<Value>) {
                bool changed = false;
                if (widget::begin_node(label)) {
                    changed = inspect(value);
                    widget::end_node();
                }
                return changed;
            } else if constexpr (std::is_same_v<Value, bool>) {
                return widget::edit_bool(label, value);
            } else if constexpr (std::is_same_v<Value, std::string>) {
                return widget::edit_string(label, value);
            } else if constexpr (std::is_arithmetic_v<Value>) {
                return widget::edit_scalar(label, scalar_of<Value>(), &value, 1, range);
            } else if constexpr (GlmVector<Value>::value) {
                using Element = typename GlmVector<Value>::Element;
                return widget::edit_scalar(label, scalar_of<Element>(), &value[0],
                                           static_cast<int>(GlmVector<Value>::length), range);
            } else if constexpr (GlmQuat<Value>::value) {
                // GLM_FORCE_QUAT_DATA_WXYZ is set for every translation unit, so
                // the four numbers sit in wxyz order. See math/conventions.h.
                using Element = typename GlmQuat<Value>::Element;
                constexpr int kQuatLength = 4;
                return widget::edit_scalar(label, scalar_of<Element>(), &value.w, kQuatLength,
                                           range);
            } else if constexpr (StdVector<Value>::value) {
                if (!widget::begin_node(label)) {
                    return false;
                }
                bool changed = false;
                for (std::size_t i = 0; i < value.size(); ++i) {
                    const std::string element_label = "[" + std::to_string(i) + "]";
                    changed |= inspect_value(element_label.c_str(), value[i], range);
                }
                const int step = widget::list_buttons(value.size());
                if (step > 0) {
                    value.emplace_back();
                    changed = true;
                } else if (step < 0 && !value.empty()) {
                    value.pop_back();
                    changed = true;
                }
                widget::end_node();
                return changed;
            } else {
                widget::show_unhandled(label);
                return false;
            }
        }

        /// Draws one field, honoring Hidden, ReadOnly, Range, and Tooltip.
        template <typename FieldType, typename V>
        [[nodiscard]] bool inspect_field(const FieldType& field, V& value) {
            if constexpr (has_attribute_v<Hidden, FieldType>) {
                return false;
            } else {
                // The attribute lives in the descriptor, not in the type, so read
                // it into a local and point at that.
                Range bounds;
                const Range* range = nullptr;
                if constexpr (has_attribute_v<Range, FieldType>) {
                    bounds = field.template attribute<Range>();
                    range = &bounds;
                }

                constexpr bool read_only = has_attribute_v<ReadOnly, FieldType>;
                if constexpr (read_only) {
                    widget::begin_disabled();
                }

                const bool changed = inspect_value(field.name(), value, range);

                if constexpr (has_attribute_v<Tooltip, FieldType>) {
                    widget::tooltip(field.template attribute<Tooltip>().text);
                }
                if constexpr (read_only) {
                    widget::end_disabled();
                    return false;
                } else {
                    return changed;
                }
            }
        }

        /// The category a field belongs to, or nullptr when it has none.
        template <typename FieldType>
        constexpr const char* category_of(const FieldType& field) {
            if constexpr (has_attribute_v<Category, FieldType>) {
                return field.template attribute<Category>().name;
            } else {
                return nullptr;
            }
        }

        [[nodiscard]] bool same_category(const char* left, const char* right);

    } // namespace detail
    /// @endcond

    /**
     * @brief Draws an editor for every field of an object.
     *
     * Fields with no Category come first, in declaration order. Each Category
     * then gets a collapsing header, in the order the categories first appear.
     *
     * Call this between ImGui::Begin() and ImGui::End(), or inside any other
     * ImGui container.
     *
     * @tparam T A described type, deduced.
     * @param object The object to show and change.
     * @return True when the user changed any field this frame.
     *
     * @code
     * if (ImGui::Begin("Player")) {
     *     if (engine::reflect::inspect(player)) {
     *         mark_dirty();
     *     }
     * }
     * ImGui::End();
     * @endcode
     */
    template <typename T>
        requires Described<std::remove_cv_t<T>>
    [[nodiscard]] bool inspect(T& object) {
        bool changed = false;
        widget::push_id(&object);

        // Pass one. Everything the author did not put in a category.
        for_each_field(object, [&changed](const auto& field, auto& value) {
            if (detail::category_of(field) == nullptr) {
                changed |= detail::inspect_field(field, value);
            }
        });

        // Pass two. Collect the categories in the order they first appear, so
        // the editor layout follows the declaration and not the alphabet.
        std::vector<const char*> categories;
        for_each_field(object, [&categories](const auto& field, const auto& /*value*/) {
            const char* name = detail::category_of(field);
            if (name == nullptr) {
                return;
            }
            for (const char* seen : categories) {
                if (detail::same_category(seen, name)) {
                    return;
                }
            }
            categories.push_back(name);
        });

        // Pass three. One header for each category, holding its own fields.
        for (const char* category : categories) {
            if (!widget::begin_category(category)) {
                continue;
            }
            for_each_field(object, [&](const auto& field, auto& value) {
                const char* name = detail::category_of(field);
                if (name != nullptr && detail::same_category(name, category)) {
                    changed |= detail::inspect_field(field, value);
                }
            });
            widget::end_category();
        }

        widget::pop_id();
        return changed;
    }

} // namespace engine::reflect
