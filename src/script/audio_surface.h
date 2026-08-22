#pragma once

/**
 * @file
 * @brief What a script may do to the sound.
 *
 * M11.6. `src/script/` sits in `engine_core` and must build with
 * `ENGINE_WITH_AUDIO` off, so the binding cannot name a mixer. This interface is
 * the seam: `src/script/` calls it, and `engine::audio::ScriptAudio` in
 * `src/audio/` is the one implementation that knows what a voice is. It is the
 * same shape as `script::UiSurface`.
 *
 * A build with no audio passes no surface. Every call in the `audio` table then
 * answers false or zero, the same way an action reads false when nobody bound an
 * input module. See `script::Services`.
 *
 * **A sound is named by its source path**, the way a prefab is. `"sounds/click.wav"`
 * names the file a person put in the content tree, and nothing hands a script a
 * GUID: an identity is not a thing anybody can type.
 *
 * **Nothing here reports what the mixer is doing.** A script can start a voice
 * and stop it, and it cannot ask whether one is still playing. The mixer runs on
 * its own thread against the real clock, so an answer from it would put wall
 * time into a game that has to be reproducible. `DESIGN.md` §9 holds that rule
 * and issue #245 is where it came from.
 */

#include "core/entt.h"

#include <cstdint>
#include <string>
#include <string_view>

#include <entt/entity/entity.hpp>
#include <entt/entity/fwd.hpp>

namespace engine::script {

    /// @brief One voice a script started. Zero is no voice, and never a live one.
    using ScriptVoice = std::uint64_t;

    /// @brief What a script asked to play.
    struct ScriptSound {
        /// @brief The source path of the sound, as the content tree holds it.
        std::string name;

        /// @brief A straight multiplier on the samples. One is as cooked.
        float volume = 1.0F;

        /// @brief Speed, and pitch with it. One is as cooked.
        float pitch = 1.0F;

        /// @brief Start again at the end, forever. Whoever starts it must stop it.
        bool looping = false;

        /**
         * @brief Whether it has a place in the world.
         *
         * A script that gives a point sets this. One that does not gets a sound
         * heard the same wherever the listener stands, which is what a menu
         * click wants.
         */
        bool spatial = false;

        float x = 0.0F; ///< Where it is, when @ref spatial. Meters.
        float y = 0.0F; ///< Where it is, when @ref spatial. Meters.
        float z = 0.0F; ///< Where it is, when @ref spatial. Meters.

        /// @brief Which bus, by name. Empty takes the default for the shape.
        std::string bus;
    };

    /**
     * @brief The calls the `audio` table makes.
     *
     * Every one takes the entity whose script is calling. That is what makes a
     * reload safe: a script restarts, and the voices the old one started are
     * stopped rather than left running with nobody holding them.
     */
    class AudioSurface {
    public:
        AudioSurface() = default;
        virtual ~AudioSurface() = default;

        AudioSurface(const AudioSurface&) = delete;
        AudioSurface& operator=(const AudioSurface&) = delete;
        AudioSurface(AudioSurface&&) = delete;
        AudioSurface& operator=(AudioSurface&&) = delete;

        /**
         * @brief Starts a sound.
         * @param owner The entity whose script asked. It owns the voice.
         * @param sound What to play.
         * @return The voice, or zero when nothing is playing.
         */
        [[nodiscard]] virtual ScriptVoice play(entt::entity owner, const ScriptSound& sound) = 0;

        /**
         * @brief Stops one voice a script started.
         * @param voice The voice to stop. Zero, or one that ended, does nothing.
         * @return True when a live voice was stopped.
         */
        virtual bool stop(ScriptVoice voice) = 0;

        /**
         * @brief Stops every voice one entity's script started.
         *
         * The host calls this when an instance goes: a reload, a script
         * changed, or the entity destroyed. A looping sound would otherwise
         * play forever with nothing left that knows about it.
         *
         * @param owner The entity whose voices go.
         */
        virtual void stop_owned_by(entt::entity owner) = 0;

        /**
         * @brief Sets a bus volume.
         * @param bus The bus name, such as "music". The case does not matter.
         * @param volume A multiplier. One is as mixed.
         * @return True when @p bus names a bus.
         */
        virtual bool set_bus_volume(std::string_view bus, float volume) = 0;

        /**
         * @brief Silences a bus, or lets it back.
         * @param bus The bus name, such as "music". The case does not matter.
         * @param mute True to silence it.
         * @return True when @p bus names a bus.
         */
        virtual bool set_bus_mute(std::string_view bus, bool mute) = 0;
    };

} // namespace engine::script
