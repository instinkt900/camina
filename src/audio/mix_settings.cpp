#include "audio/mix_settings.h"

namespace engine::audio {

    const BusSettings& settings_for(const MixSettings& settings, Bus bus) {
        switch (bus) {
        case Bus::Music:
            return settings.music;
        case Bus::Effects:
            return settings.effects;
        case Bus::Master:
            break;
        }
        return settings.master;
    }

} // namespace engine::audio
