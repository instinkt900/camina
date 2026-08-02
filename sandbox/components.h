#pragma once

/**
 * @file
 * @brief The components the sandbox game defines for itself.
 *
 * These live next to the game, not in the engine. The engine never names them.
 * A scene file carries them because the game registers them with the same
 * component registry the engine uses, and reflect/json.h reads the same
 * descriptors. That is rule 4.5 working from the outside.
 */

#include "math/conventions.h"
#include "reflect/attributes.h"
#include "reflect/reflect.h"

#include <tuple>

/// @brief The small game that sets the scope of the engine. See rule 4.6.
namespace sandbox {

    /// @brief How long one turn takes when a scene file does not say.
    inline constexpr float kDefaultSecondsPerTurn = 4.0F;

    /// @brief The slowest turn the inspector offers, in seconds.
    inline constexpr double kSlowestTurn = 60.0;

    /// @brief How far one drag of the turn slider moves, in seconds.
    inline constexpr double kTurnStep = 0.1;

    /**
     * @brief Turns an entity on an axis, once every so many seconds.
     *
     * The angle follows the elapsed time rather than adding up each frame, so
     * two runs of the same length agree and a slow frame does not drift.
     */
    struct Spin {
        /// @brief The axis to turn on, in parent space. It does not have to be a unit vector.
        engine::Vec3 axis{ 0.0F, 1.0F, 0.0F };
        /// @brief Seconds for one full turn. A value of zero or less stops the entity.
        float seconds_per_turn = kDefaultSecondsPerTurn;
    };

} // namespace sandbox

/**
 * @brief Describes Spin for the inspector and for scene files.
 *
 * The game describes its own types. Nothing in the engine changes to carry a
 * component the game invented.
 */
template <>
struct engine::reflect::Describe<sandbox::Spin> {
    static constexpr const char* name = "Spin"; ///< The name a scene file stores.
    /// @brief The fields, in the order an editor shows them.
    /// @return A tuple of field descriptors.
    static constexpr auto fields() {
        return std::make_tuple(
            ENGINE_FIELD(sandbox::Spin, axis, Tooltip{ "Turns on this axis, in parent space" }),
            ENGINE_FIELD(sandbox::Spin, seconds_per_turn,
                         Range{ 0.0, sandbox::kSlowestTurn, sandbox::kTurnStep },
                         Tooltip{ "Seconds for one full turn. Zero stops it" }));
    }
};
