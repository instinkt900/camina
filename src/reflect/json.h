#pragma once

/**
 * @file
 * @brief JSON reading and writing, driven by the field descriptors.
 *
 * This is one of the two consumers DESIGN.md section 7 asks for. It reads the
 * same descriptors the inspector reads, and it never adds a second descriptor
 * system. Rule 4.5.
 *
 * The consumer interface is for_each_field(), so a later libclang codegen step
 * can replace the hand-written descriptors without touching this file.
 *
 * Nothing here throws. The parser runs with exceptions off and reports a bad
 * document through the return value.
 */

#include "core/log.h"
#include "math/conventions.h"
#include "reflect/attributes.h"
#include "reflect/reflect.h"
#include "reflect/traits.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <type_traits>

namespace engine::reflect {

    /**
     * @brief The key that carries the schema version in a written document.
     *
     * The name starts with two underscores so it cannot collide with a field
     * name that a C++ struct can hold.
     */
    inline constexpr const char* kVersionKey = "__version";

    /**
     * @brief The oldest schema. A document with no version key counts as this.
     */
    inline constexpr std::uint32_t kFirstSchemaVersion = 1;

    template <typename T>
        requires Described<std::remove_cv_t<T>>
    [[nodiscard]] nlohmann::json to_json(const T& object);

    template <Described T>
    [[nodiscard]] bool from_json(const nlohmann::json& document, T& object);

    /// @cond
    // Doxygen documents the interface above and below. These helpers are an
    // implementation detail, and describing every branch of the type dispatch
    // adds noise without adding meaning.
    namespace detail {

        /// The schema version a single field first appeared in.
        template <typename FieldType>
        constexpr std::uint32_t added_in(const FieldType& field) {
            if constexpr (has_attribute_v<Version, FieldType>) {
                return field.template attribute<Version>().added_in;
            } else {
                return kFirstSchemaVersion;
            }
        }

        template <typename V>
        void write_value(nlohmann::json& out, const V& value);

        template <typename V>
        [[nodiscard]] bool read_value(const nlohmann::json& in, V& value,
                                      const char* where);

        template <typename V>
        void write_value(nlohmann::json& out, const V& value) {
            using Value = std::remove_cvref_t<V>;

            if constexpr (Described<Value>) {
                out = to_json(value);
            } else if constexpr (TextValue<Value>) {
                out = to_text(value);
            } else if constexpr (GlmVector<Value>::value) {
                out = nlohmann::json::array();
                for (glm::length_t i = 0; i < GlmVector<Value>::length; ++i) {
                    out.push_back(value[i]);
                }
            } else if constexpr (GlmQuat<Value>::value) {
                // wxyz order, per DESIGN.md section 3. The members are named, so
                // the order does not depend on the glm storage layout.
                out = nlohmann::json::array({ value.w, value.x, value.y, value.z });
            } else if constexpr (StdVector<Value>::value) {
                out = nlohmann::json::array();
                for (const auto& element : value) {
                    nlohmann::json item;
                    write_value(item, element);
                    out.push_back(std::move(item));
                }
            } else if constexpr (std::is_enum_v<Value>) {
                out = static_cast<std::underlying_type_t<Value>>(value);
            } else {
                // Arithmetic types and std::string. nlohmann handles these.
                out = value;
            }
        }

        /// Reports a value the reader cannot use, and keeps the default in place.
        inline bool wrong_type(const char* where, const char* wanted,
                               const nlohmann::json& in) {
            ENGINE_LOG_ERROR("{}: expected {} but the document holds {}", where, wanted,
                             in.type_name());
            return false;
        }

        /// Reads a fixed-length array of numbers into a glm vector.
        template <typename V>
        [[nodiscard]] bool read_glm_vector(const nlohmann::json& in, V& value,
                                           const char* where) {
            constexpr auto length = static_cast<std::size_t>(GlmVector<V>::length);
            if (!in.is_array() || in.size() != length) {
                return wrong_type(where, "an array of the vector length", in);
            }
            for (std::size_t i = 0; i < length; ++i) {
                if (!in[i].is_number()) {
                    return wrong_type(where, "a number in every element", in);
                }
                value[static_cast<glm::length_t>(i)] =
                    in[i].template get<typename GlmVector<V>::Element>();
            }
            return true;
        }

        /// Reads four numbers in wxyz order into a quaternion.
        template <typename V>
        [[nodiscard]] bool read_glm_quat(const nlohmann::json& in, V& value,
                                         const char* where) {
            constexpr std::size_t kQuatLength = 4;
            if (!in.is_array() || in.size() != kQuatLength) {
                return wrong_type(where, "an array of four numbers", in);
            }
            for (std::size_t i = 0; i < kQuatLength; ++i) {
                if (!in[i].is_number()) {
                    return wrong_type(where, "a number in every element", in);
                }
            }
            using Element = typename GlmQuat<V>::Element;
            value.w = in[0].template get<Element>();
            value.x = in[1].template get<Element>();
            value.y = in[2].template get<Element>();
            value.z = in[3].template get<Element>();
            return true;
        }

        /// Reads a value that carries its own text form, such as a Guid.
        template <typename V>
        [[nodiscard]] bool read_text(const nlohmann::json& in, V& value, const char* where) {
            if (!in.is_string()) {
                return wrong_type(where, "a string", in);
            }
            if (!from_text(in.template get_ref<const std::string&>(), value)) {
                ENGINE_LOG_ERROR("{}: \"{}\" is not a valid value for this field", where,
                                 in.template get_ref<const std::string&>());
                return false;
            }
            return true;
        }

        /// Reads a plain number, a boolean, a string, or an enumerator.
        template <typename V>
        [[nodiscard]] bool read_simple(const nlohmann::json& in, V& value, const char* where) {
            if constexpr (std::is_enum_v<V>) {
                if (!in.is_number_integer()) {
                    return wrong_type(where, "an integer", in);
                }
                value = static_cast<V>(in.template get<std::underlying_type_t<V>>());
            } else if constexpr (std::is_same_v<V, bool>) {
                if (!in.is_boolean()) {
                    return wrong_type(where, "a boolean", in);
                }
                value = in.template get<bool>();
            } else if constexpr (std::is_same_v<V, std::string>) {
                if (!in.is_string()) {
                    return wrong_type(where, "a string", in);
                }
                value = in.template get<std::string>();
            } else if constexpr (std::is_arithmetic_v<V>) {
                if (!in.is_number()) {
                    return wrong_type(where, "a number", in);
                }
                value = in.template get<V>();
            } else {
                static_assert(sizeof(V) == 0,
                              "reflect::from_json does not handle this field type. Add a "
                              "branch to read_value in reflect/json.h.");
                return false;
            }
            return true;
        }

        /// Reads a list, one element at a time.
        template <typename V>
        [[nodiscard]] bool read_list(const nlohmann::json& in, V& value, const char* where) {
            if (!in.is_array()) {
                return wrong_type(where, "an array", in);
            }
            V parsed;
            parsed.reserve(in.size());
            for (const auto& item : in) {
                typename StdVector<V>::Element element{};
                if (!read_value(item, element, where)) {
                    return false;
                }
                parsed.push_back(std::move(element));
            }
            value = std::move(parsed);
            return true;
        }

        template <typename V>
        [[nodiscard]] bool read_value(const nlohmann::json& in, V& value,
                                      const char* where) {
            using Value = std::remove_cvref_t<V>;

            if constexpr (Described<Value>) {
                return from_json(in, value);
            } else if constexpr (TextValue<Value>) {
                return read_text(in, value, where);
            } else if constexpr (GlmVector<Value>::value) {
                return read_glm_vector(in, value, where);
            } else if constexpr (GlmQuat<Value>::value) {
                return read_glm_quat(in, value, where);
            } else if constexpr (StdVector<Value>::value) {
                return read_list(in, value, where);
            } else {
                return read_simple(in, value, where);
            }
        }

    } // namespace detail
    /// @endcond

    /**
     * @brief The schema version a type writes.
     *
     * This is the largest `Version` attribute on any field, and 1 when no field
     * carries one. Adding a field with `Version{ n }` therefore raises the
     * version of the whole type, and an older document still loads.
     *
     * @tparam T A described type.
     * @return The schema version.
     */
    template <Described T>
    [[nodiscard]] constexpr std::uint32_t schema_version() {
        std::uint32_t version = kFirstSchemaVersion;
        std::apply(
            [&version](const auto&... fields) {
                ((version = std::max(version, detail::added_in(fields))), ...);
            },
            Describe<T>::fields());
        return version;
    }

    /**
     * @brief Writes an object to a JSON value.
     *
     * Every field goes in under its own name, except a field marked Transient.
     * The document also carries the schema version under kVersionKey.
     *
     * @tparam T A described type, deduced.
     * @param object The object to write.
     * @return The JSON object.
     *
     * @code
     * const nlohmann::json document = engine::reflect::to_json(player);
     * @endcode
     */
    template <typename T>
        requires Described<std::remove_cv_t<T>>
    [[nodiscard]] nlohmann::json to_json(const T& object) {
        nlohmann::json out = nlohmann::json::object();
        out[kVersionKey] = schema_version<std::remove_cv_t<T>>();

        for_each_field(object, [&out](const auto& field, const auto& value) {
            if constexpr (!has_attribute_v<Transient, decltype(field)>) {
                nlohmann::json item;
                detail::write_value(item, value);
                out[field.name()] = std::move(item);
            }
        });

        return out;
    }

    /**
     * @brief Reads an object back from a JSON value.
     *
     * A field marked Transient is skipped. A missing field keeps whatever the
     * object already holds, which is the C++ default when the caller passed a
     * fresh object.
     *
     * Version drives the migration. When the document is older than the version
     * a field was added in, the field is expected to be missing and the reader
     * stays quiet. When the document is new enough, a missing field is a
     * warning, because it points at a truncated or hand-edited file.
     *
     * @tparam T A described type.
     * @param document The JSON object to read.
     * @param object The object to fill.
     * @return True when every present field was read. False when the document is
     * not an object, or a field holds the wrong type.
     */
    template <Described T>
    [[nodiscard]] bool from_json(const nlohmann::json& document, T& object) {
        if (!document.is_object()) {
            ENGINE_LOG_ERROR("{}: the document is {}, not an object", type_name<T>(),
                             document.type_name());
            return false;
        }

        std::uint32_t document_version = kFirstSchemaVersion;
        if (const auto version = document.find(kVersionKey); version != document.end()) {
            // A parser reads "1" as unsigned, but a document a program built in
            // memory can hold a plain int. Accept either, and reject a negative
            // number and a number with a fraction.
            if (!version->is_number_integer() || version->get<std::int64_t>() < 0) {
                ENGINE_LOG_ERROR("{}: {} is not a version number", type_name<T>(), kVersionKey);
                return false;
            }
            document_version = version->get<std::uint32_t>();
        }

        bool ok = true;
        for_each_field(object, [&](const auto& field, auto& value) {
            if constexpr (!has_attribute_v<Transient, decltype(field)>) {
                const auto entry = document.find(field.name());
                if (entry == document.end()) {
                    if (document_version >= detail::added_in(field)) {
                        ENGINE_LOG_WARN("{}: field {} is missing from a version {} document",
                                        type_name<T>(), field.name(), document_version);
                    }
                    // Older than the field, or simply absent. The default stands.
                    return;
                }
                if (!detail::read_value(*entry, value, field.name())) {
                    ok = false;
                }
            }
        });

        return ok;
    }

    /**
     * @brief Writes an object to a file as JSON.
     * @tparam T A described type, deduced.
     * @param path Where to write. The parent directory must exist.
     * @param object The object to write.
     * @return True when the file was written.
     */
    template <typename T>
        requires Described<std::remove_cv_t<T>>
    [[nodiscard]] bool save_json(const std::filesystem::path& path, const T& object) {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        if (!file) {
            ENGINE_LOG_ERROR("Could not open {} for writing.", path.string());
            return false;
        }

        constexpr int kIndent = 2;
        file << to_json(object).dump(kIndent) << '\n';
        if (!file) {
            ENGINE_LOG_ERROR("Could not write {}.", path.string());
            return false;
        }
        return true;
    }

    /**
     * @brief Reads an object back from a JSON file.
     *
     * The object keeps its current value for any field the file leaves out, so
     * pass a default-constructed object unless you want the file to patch what
     * you already hold.
     *
     * @tparam T A described type.
     * @param path The file to read.
     * @param object The object to fill.
     * @return True when the file parsed and every present field was read.
     */
    template <Described T>
    [[nodiscard]] bool load_json(const std::filesystem::path& path, T& object) {
        std::ifstream file(path, std::ios::binary);
        if (!file) {
            ENGINE_LOG_ERROR("Could not open {} for reading.", path.string());
            return false;
        }

        // Exceptions off. A bad document comes back as a discarded value.
        const nlohmann::json document = nlohmann::json::parse(file, nullptr, false);
        if (document.is_discarded()) {
            ENGINE_LOG_ERROR("{} is not valid JSON.", path.string());
            return false;
        }

        return from_json(document, object);
    }

} // namespace engine::reflect
