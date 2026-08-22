#include "audio/script_audio.h"

#include "assets/sound.h"
#include "audio/bus.h"
#include "core/log.h"

#include <utility>
#include <vector>

namespace engine::audio {

    void ScriptAudio::bind(Mixer& mixer, const assets::AssetSource& content) {
        mixer_ = &mixer;
        content_ = &content;
        rebuild_names();
    }

    void ScriptAudio::rebuild_names() {
        by_name_.clear();
        refused_.clear();
        if (content_ == nullptr) {
            return;
        }

        // Every cooked sound the project holds, keyed by the source path a
        // person wrote. That is what a script names, the same way it names a
        // prefab.
        std::vector<assets::AssetRecord> records;
        if (!content_->assets_of_kind(assets::kSoundExtension, records)) {
            return;
        }
        for (const assets::AssetRecord& record : records) {
            by_name_.emplace(record.source, record.guid);
        }
    }

    script::ScriptVoice ScriptAudio::play(entt::entity owner, const script::ScriptSound& sound) {
        if (mixer_ == nullptr || content_ == nullptr) {
            return 0;
        }

        const auto found = by_name_.find(sound.name);
        if (found == by_name_.end()) {
            // Once for each name. A script that plays a missing sound on every
            // step would otherwise fill the log and hide what else went wrong.
            if (!refused_.contains(sound.name)) {
                refused_.emplace(sound.name, true);
                ENGINE_LOG_ERROR("audio.play: this project holds no sound called {}.",
                                 sound.name);
            }
            return 0;
        }

        Bus bus = sound.spatial ? Bus::Effects : Bus::Master;
        if (!sound.bus.empty() && !from_text(sound.bus, bus)) {
            ENGINE_LOG_WARN("audio.play: {} is not a bus. Playing {} on {} instead.", sound.bus,
                            sound.name, to_text(bus));
        }

        const PlayDesc desc{ .volume = sound.volume,
                             .pitch = sound.pitch,
                             .looping = sound.looping,
                             .spatial = sound.spatial,
                             .position = { sound.x, sound.y, sound.z },
                             .bus = bus };

        const VoiceId voice = mixer_->play(*content_, found->second, desc);
        if (voice == 0) {
            return 0;
        }

        owners_.emplace(voice, owner);
        return voice;
    }

    bool ScriptAudio::stop(script::ScriptVoice voice) {
        const auto held = owners_.find(voice);
        if (held == owners_.end()) {
            return false;
        }
        if (mixer_ != nullptr) {
            mixer_->stop(voice);
        }
        owners_.erase(held);
        return true;
    }

    void ScriptAudio::stop_owned_by(entt::entity owner) {
        for (auto at = owners_.begin(); at != owners_.end();) {
            if (at->second != owner) {
                ++at;
                continue;
            }
            if (mixer_ != nullptr) {
                mixer_->stop(at->first);
            }
            at = owners_.erase(at);
        }
    }

    bool ScriptAudio::set_bus_volume(std::string_view bus, float volume) {
        Bus which = Bus::Master;
        if (mixer_ == nullptr || !from_text(bus, which)) {
            return false;
        }
        BusSettings settings = mixer_->bus_settings(which);
        settings.volume = volume;
        mixer_->set_bus(which, settings);
        return true;
    }

    bool ScriptAudio::set_bus_mute(std::string_view bus, bool mute) {
        Bus which = Bus::Master;
        if (mixer_ == nullptr || !from_text(bus, which)) {
            return false;
        }
        BusSettings settings = mixer_->bus_settings(which);
        settings.mute = mute;
        mixer_->set_bus(which, settings);
        return true;
    }

} // namespace engine::audio
