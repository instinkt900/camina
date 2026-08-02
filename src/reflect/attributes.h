#pragma once

/**
 * @file
 * @brief The attribute types that a field descriptor can carry.
 *
 * DESIGN.md section 7 requires this list from day one, because adding it later
 * is very costly. Every attribute is a small plain struct, so a descriptor stays
 * a compile-time constant.
 *
 * The list is open. A new attribute is a new struct, and no existing consumer
 * has to change.
 */

#include <cstdint>

/// @brief Field descriptors, attributes, and the type registry.
namespace engine::reflect {

    /**
     * @brief Bounds for an editor slider.
     *
     * The inspector uses these for a slider instead of a free entry box. A
     * serializer ignores them.
     */
    struct Range {
        double min = 0.0;  ///< The lowest value the editor offers.
        double max = 1.0;  ///< The highest value the editor offers.
        double step = 0.0; ///< The increment. Zero lets the editor choose.
    };

    /// @brief Help text the editor shows when the pointer rests on the field.
    struct Tooltip {
        const char* text = ""; ///< A static string. The field does not own it.
    };

    /// @brief The group the editor puts the field in.
    struct Category {
        const char* name = ""; ///< A static string. The field does not own it.
    };

    /**
     * @brief The field is reflected but the editor does not show it.
     *
     * Serialization still writes it. Use Transient to skip serialization.
     */
    struct Hidden {};

    /// @brief The editor shows the field but does not let the user change it.
    struct ReadOnly {};

    /**
     * @brief Serialization skips the field.
     *
     * Use this for a value the program derives at load time, such as a cache or
     * a runtime handle.
     */
    struct Transient {};

    /**
     * @brief The schema version the field first appeared in.
     *
     * A reader compares this against the version stored in the file, so an old
     * file can load into a newer struct. See DESIGN.md section 7.
     */
    struct Version {
        std::uint32_t added_in = 1; ///< The first schema version that has this field.
    };

    /**
     * @brief The field exists only for the editor.
     *
     * Rule 4.3 in DESIGN.md allows `#ifdef EDITOR` for exactly this, to keep
     * editor-only metadata out of a shipping build.
     */
    struct EditorOnly {};

} // namespace engine::reflect
