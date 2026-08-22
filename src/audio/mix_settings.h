#pragma once

/**
 * @file
 * @brief What a player set the volumes to.
 *
 * These are the numbers a settings screen writes and a file keeps between runs.
 * They belong to the application rather than to a level, so no scene file holds
 * them.
 *
 * The struct is reflected, so one description serves the inspector and the JSON
 * both, the way every other described type does. It is not behind
 * `ENGINE_WITH_AUDIO`: a settings file written by a build with sound has to read
 * back in one without it.
 */

#include "audio/bus.h"
#include "reflect/attributes.h"
#include "reflect/reflect.h"

namespace engine::audio {

    /// @brief What one bus is set to.
    struct BusSettings {
        /**
         * @brief A straight multiplier on everything the bus carries.
         *
         * One is as mixed. Zero is silence, and it still costs what the sounds
         * cost: a muted bus is quiet rather than absent.
         */
        float volume = 1.0F;

        /**
         * @brief Silence the bus without losing the volume it was at.
         *
         * A person who mutes and unmutes expects the slider to be where they
         * left it, so this is a separate field rather than a volume of zero.
         */
        bool mute = false;
    };

    /**
     * @brief Every bus, in one thing a file can hold.
     *
     * @see engine::audio::Bus for what each one carries.
     */
    struct MixSettings {
        BusSettings master;  ///< Everything. The other two feed into it.
        BusSettings music;   ///< Music alone.
        BusSettings effects; ///< Everything the world makes.
    };

    /**
     * @brief The settings for one bus.
     * @param settings Every bus.
     * @param bus Which one to read.
     * @return That bus's volume and mute.
     */
    [[nodiscard]] const BusSettings& settings_for(const MixSettings& settings, Bus bus);

} // namespace engine::audio

/// @brief Describes one bus for the inspector and for JSON.
template <>
struct engine::reflect::Describe<engine::audio::BusSettings> {
    /// @brief The type name a file stores.
    static constexpr const char* name = "BusSettings";

    /// @brief The two fields.
    /// @return A tuple of field descriptors.
    static constexpr auto fields() {
        return std::make_tuple(
            ENGINE_FIELD(engine::audio::BusSettings, volume,
                         engine::reflect::Range{ 0.0, 1.0, 0.01 },
                         engine::reflect::Tooltip{ "Multiplies everything the bus carries" }),
            ENGINE_FIELD(engine::audio::BusSettings, mute,
                         engine::reflect::Tooltip{ "Silence it and keep the volume it was at" }));
    }
};

/// @brief Describes every bus for the inspector and for JSON.
template <>
struct engine::reflect::Describe<engine::audio::MixSettings> {
    /// @brief The type name a file stores.
    static constexpr const char* name = "MixSettings";

    /// @brief One field for each bus.
    /// @return A tuple of field descriptors.
    static constexpr auto fields() {
        return std::make_tuple(
            ENGINE_FIELD(engine::audio::MixSettings, master,
                         engine::reflect::Tooltip{ "Everything. The other two feed into it" }),
            ENGINE_FIELD(engine::audio::MixSettings, music,
                         engine::reflect::Tooltip{ "Music alone" }),
            ENGINE_FIELD(engine::audio::MixSettings, effects,
                         engine::reflect::Tooltip{ "Everything the world makes" }));
    }
};
