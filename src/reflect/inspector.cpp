#include "reflect/inspector.h"

#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include <array>
#include <cstddef>
#include <cstring>
#include <string_view>
#include <utility>
#include <unordered_map>

namespace engine::reflect {

    namespace {
        /// What every AssetRef field reads to turn an identity into a name.
        AssetNamer g_asset_namer;
    } // namespace

    void set_asset_namer(AssetNamer namer) { g_asset_namer = std::move(namer); }


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

        bool edit_enum(const char* label, const char* const* names, std::size_t count,
                       std::size_t& index) {
            // ImGui::Combo takes a signed index and shows an empty box for one
            // that is out of range, which is what a value no enumerator names
            // should look like. So an index of count passes straight through.
            int chosen = static_cast<int>(index);
            if (!ImGui::Combo(label, &chosen, names, static_cast<int>(count))) {
                return false;
            }

            // ImGui reports a change only when the user picked an entry, so this
            // holds today. It is checked because the caller turns the index into
            // an array subscript, and this is the boundary with code we do not
            // own. A report of a change to nothing is not worth passing on.
            if (chosen < 0 || static_cast<std::size_t>(chosen) >= count) {
                return false;
            }

            index = static_cast<std::size_t>(chosen);
            return true;
        }

        bool edit_string(const char* label, std::string& value) {
            return ImGui::InputText(label, &value);
        }

        bool edit_asset(const char* label, std::string& text) {
            bool changed = false;

            // What a person reads. The namer answers empty for an identity
            // nothing in the manifest produced, and the raw text is then the
            // honest thing to show: it says the field names something the cook
            // did not make.
            std::string name;
            if (g_asset_namer && !text.empty()) {
                name = g_asset_namer(text);
            }
            const char* shown = "none";
            if (!name.empty()) {
                shown = name.c_str();
            } else if (!text.empty()) {
                shown = text.c_str();
            }

            ImGui::PushID(label);

            // A button rather than a text box, because an identity is derived
            // and typing one is not a thing anybody can do. It is a place to
            // drop an asset and nothing else.
            const float clear_width = ImGui::GetFrameHeight();
            const float width = ImGui::GetContentRegionAvail().x - clear_width -
                                ImGui::GetStyle().ItemSpacing.x;
            ImGui::Button(shown, ImVec2{ width > 0.0F ? width : 0.0F, 0.0F });

            if (ImGui::BeginDragDropTarget()) {
                if (const ImGuiPayload* dropped = ImGui::AcceptDragDropPayload(kAssetPayload)) {
                    text.assign(static_cast<const char*>(dropped->Data),
                                static_cast<std::size_t>(dropped->DataSize));
                    changed = true;
                }
                ImGui::EndDragDropTarget();
            }

            if (ImGui::IsItemHovered() && !text.empty()) {
                // The identity itself, for the moment somebody needs to compare
                // one against a file or a log line.
                ImGui::SetTooltip("%s", text.c_str());
            }

            ImGui::SameLine();
            ImGui::BeginDisabled(text.empty());
            if (ImGui::Button("x", ImVec2{ clear_width, 0.0F })) {
                text.clear();
                changed = true;
            }
            ImGui::EndDisabled();

            ImGui::SameLine();
            ImGui::TextUnformatted(label);

            ImGui::PopID();
            return changed;
        }

        bool edit_text_value(const char* label, std::string& text) {
            // The field the user is typing in needs a buffer that survives the
            // frame. A half-typed value does not parse, so the caller does not
            // store it. Rebuilding the buffer from the value on each frame
            // would delete what the user typed, one keystroke at a time.
            //
            // Each field keeps its own buffer, under its own item ID. One
            // shared buffer is not enough. ImGui holds one item active at a
            // time, but two fields change state in the same frame. That
            // happens when the user clicks from one straight into the other.
            // The field that takes focus seeds the buffer. When it draws
            // first, the field that gives up focus commits the wrong text.
            //
            // An entry lives only while its field is active, so the map holds
            // one string at a time in the normal case.
            static std::unordered_map<ImGuiID, std::string> pending;

            const ImGuiID id = ImGui::GetID(label);
            const auto entry = pending.find(id);

            // A field nobody is editing draws from a copy that lives for the
            // call, so it never writes over another field's buffer.
            std::string idle;
            if (entry == pending.end()) {
                idle = text;
            }
            std::string& buffer = entry == pending.end() ? idle : entry->second;

            ImGui::InputText(label, &buffer);

            if (ImGui::IsItemActivated()) {
                // This frame already drew from the copy, which holds the same.
                pending[id] = text;
            }

            bool committed = false;
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                // Read this field's own buffer, not whichever one was written
                // last. Look it up again, because the insert above can rehash.
                if (const auto own = pending.find(id); own != pending.end()) {
                    text = own->second;
                    committed = true;
                }
            }
            if (ImGui::IsItemDeactivated()) {
                pending.erase(id);
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
