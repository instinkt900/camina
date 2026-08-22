#pragma once

/**
 * @file
 * @brief The one implementation of what a script may do to the sound.
 *
 * `script::AudioSurface` is the interface and this is what stands behind it. It
 * turns a name into an identity, plays through `audio::Mixer`, and remembers
 * which entity's script started each voice.
 *
 * **A voice belongs to the script that started it.** A reload restarts a script
 * and the script table goes with it, which M8.5 settled. A voice is the same
 * kind of thing: scratch, not storage. So the host says when an instance goes
 * and every voice it started stops. Otherwise a looping sound plays on forever
 * with nothing left that knows its number, and only a restart of the game would
 * quiet it.
 *
 * A sound that has to survive a reload belongs on a `scene::AudioSource`, which
 * is a component and therefore storage.
 */

#include "assets/asset_source.h"
#include "audio/mixer.h"
#include "core/entt.h"
#include "core/guid.h"
#include "script/audio_surface.h"

#include <cstddef>
#include <map>
#include <string>
#include <string_view>

#include <entt/entity/entity.hpp>
#include <entt/entity/fwd.hpp>

namespace engine::audio {

    /**
     * @brief Plays what a script asks for, and cleans up after it.
     *
     * @code
     * engine::audio::ScriptAudio audio;
     * audio.bind(mixer, content);
     * session.set_script_audio(&audio);
     * @endcode
     */
    class ScriptAudio final : public script::AudioSurface {
    public:
        /**
         * @brief Says which mixer to play through and where the sounds are.
         *
         * Both must outlive this. It also builds the name index, so a script
         * that plays a sound does not walk the project to find it.
         *
         * @param mixer The mixer that holds the voices.
         * @param content The assets to read a sound from, cooked or imported.
         */
        void bind(Mixer& mixer, const assets::AssetSource& content);

        /**
         * @brief Reads the project again, after a cook or a reload changed it.
         *
         * The names are held from bind(), so a sound added while the game runs
         * is not found until this runs.
         */
        void rebuild_names();

        [[nodiscard]] script::ScriptVoice play(entt::entity owner,
                                               const script::ScriptSound& sound) override;
        bool stop(script::ScriptVoice voice) override;
        void stop_owned_by(entt::entity owner) override;
        bool set_bus_volume(std::string_view bus, float volume) override;
        bool set_bus_mute(std::string_view bus, bool mute) override;

        /// @return How many voices scripts are holding right now.
        [[nodiscard]] std::size_t voices() const { return owners_.size(); }

        /// @return How many sounds the project offers a script by name.
        [[nodiscard]] std::size_t names() const { return by_name_.size(); }

    private:
        Mixer* mixer_ = nullptr;
        const assets::AssetSource* content_ = nullptr;

        /// Source path to identity, so a script names a file rather than a GUID.
        std::map<std::string, Guid, std::less<>> by_name_;

        /// Which entity's script started each live voice.
        std::map<VoiceId, entt::entity> owners_;

        /// Names asked for that the project does not hold. Reported once each.
        std::map<std::string, bool, std::less<>> refused_;
    };

} // namespace engine::audio
