#include "reflect/inspector.h"

#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include <array>
#include <cstddef>
#include <cstring>
#include <string_view>

namespace engine::reflect {

    namespace {

        /// The widest scalar a field can hold, so one buffer serves every type.
        constexpr std::size_t kScalarBytes = 8;

        /// How fast a drag box moves when the field carries no Range step.
        constexpr float kIntegerDragSpeed = 1.0F;
        constexpr float kFloatDragSpeed = 0.01F;

        ImGuiDataType to_data_type(widget::Scalar type) {
            switch (type) {
            case widget::Scalar::Int8:
                return ImGuiDataType_S8;
            case widget::Scalar::UInt8:
                return ImGuiDataType_U8;
            case widget::Scalar::Int16:
                return ImGuiDataType_S16;
            case widget::Scalar::UInt16:
                return ImGuiDataType_U16;
            case widget::Scalar::Int32:
                return ImGuiDataType_S32;
            case widget::Scalar::UInt32:
                return ImGuiDataType_U32;
            case widget::Scalar::Int64:
                return ImGuiDataType_S64;
            case widget::Scalar::UInt64:
                return ImGuiDataType_U64;
            case widget::Scalar::Float:
                return ImGuiDataType_Float;
            case widget::Scalar::Double:
                return ImGuiDataType_Double;
            }
            return ImGuiDataType_Float;
        }

        bool is_real(widget::Scalar type) {
            return type == widget::Scalar::Float || type == widget::Scalar::Double;
        }

        /// Converts one bound from the Range attribute into the field type.
        template <typename T>
        void store_as(void* destination, double value) {
            const T converted = static_cast<T>(value);
            std::memcpy(destination, &converted, sizeof(T));
        }

        void store_bound(widget::Scalar type, void* destination, double value) {
            switch (type) {
            case widget::Scalar::Int8:
                store_as<std::int8_t>(destination, value);
                return;
            case widget::Scalar::UInt8:
                store_as<std::uint8_t>(destination, value);
                return;
            case widget::Scalar::Int16:
                store_as<std::int16_t>(destination, value);
                return;
            case widget::Scalar::UInt16:
                store_as<std::uint16_t>(destination, value);
                return;
            case widget::Scalar::Int32:
                store_as<std::int32_t>(destination, value);
                return;
            case widget::Scalar::UInt32:
                store_as<std::uint32_t>(destination, value);
                return;
            case widget::Scalar::Int64:
                store_as<std::int64_t>(destination, value);
                return;
            case widget::Scalar::UInt64:
                store_as<std::uint64_t>(destination, value);
                return;
            case widget::Scalar::Float:
                store_as<float>(destination, value);
                return;
            case widget::Scalar::Double:
                store_as<double>(destination, value);
                return;
            }
        }

    } // namespace

    namespace widget {

        bool begin_node(const char* label) { return ImGui::TreeNode(label); }

        void end_node() { ImGui::TreePop(); }

        bool begin_category(const char* name) {
            return ImGui::CollapsingHeader(name, ImGuiTreeNodeFlags_DefaultOpen);
        }

        void end_category() {
            // CollapsingHeader needs no matching call. The function exists so the
            // header reads as a pair and so a later layout change has a place to
            // go.
        }

        void begin_disabled() { ImGui::BeginDisabled(); }

        void end_disabled() { ImGui::EndDisabled(); }

        void tooltip(const char* text) { ImGui::SetItemTooltip("%s", text); }

        bool edit_scalar(const char* label, Scalar type, void* data, int count,
                         const Range* range) {
            const ImGuiDataType data_type = to_data_type(type);

            if (range != nullptr && range->max > range->min) {
                alignas(kScalarBytes) std::array<std::byte, kScalarBytes> low{};
                alignas(kScalarBytes) std::array<std::byte, kScalarBytes> high{};
                store_bound(type, low.data(), range->min);
                store_bound(type, high.data(), range->max);
                return ImGui::SliderScalarN(label, data_type, data, count, low.data(),
                                            high.data());
            }

            float speed = is_real(type) ? kFloatDragSpeed : kIntegerDragSpeed;
            if (range != nullptr && range->step > 0.0) {
                speed = static_cast<float>(range->step);
            }
            return ImGui::DragScalarN(label, data_type, data, count, speed);
        }

        bool edit_bool(const char* label, bool& value) { return ImGui::Checkbox(label, &value); }

        bool edit_string(const char* label, std::string& value) {
            return ImGui::InputText(label, &value);
        }

        bool edit_text_value(const char* label, std::string& text) {
            // The field the user is typing in needs a buffer that survives the
            // frame, because a half-typed value does not parse and the caller
            // therefore does not store it. Rebuilding the buffer from the value
            // on each frame would delete what the user typed, one keystroke at
            // a time.
            //
            // ImGui edits one item at a time, so one such buffer is enough.
            // Every other field of this kind draws from a copy that lives for
            // the call, which keeps an idle field from writing over the buffer
            // the active field owns.
            static std::string editing;
            static ImGuiID editing_id = 0;

            const ImGuiID id = ImGui::GetID(label);
            std::string idle = text;
            std::string& buffer = editing_id == id ? editing : idle;

            ImGui::InputText(label, &buffer);

            if (ImGui::IsItemActivated()) {
                // The field takes the buffer, holding what the value says now.
                // This frame already drew from the copy, which holds the same.
                editing = text;
                editing_id = id;
            }

            const bool committed = ImGui::IsItemDeactivatedAfterEdit();
            if (committed) {
                text = editing;
            }
            if (ImGui::IsItemDeactivated()) {
                editing_id = 0;
            }
            return committed;
        }

        void show_unhandled(const char* label) {
            ImGui::TextDisabled("%s: the inspector has no editor for this type", label);
        }

        int list_buttons(std::size_t size) {
            ImGui::Text("%zu elements", size);
            ImGui::SameLine();
            if (ImGui::SmallButton("Add")) {
                return 1;
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Remove")) {
                return -1;
            }
            return 0;
        }

        void push_id(const void* object) { ImGui::PushID(object); }

        void pop_id() { ImGui::PopID(); }

    } // namespace widget

    namespace detail {

        bool same_category(const char* left, const char* right) {
            // Two Category attributes with the same text can still be two
            // different pointers, so compare the text and not the address.
            return std::string_view{ left } == std::string_view{ right };
        }

    } // namespace detail

} // namespace engine::reflect
