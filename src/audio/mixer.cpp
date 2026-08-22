#include "audio/mixer.h"

#include "assets/sound.h"
#include "audio/miniaudio_config.h"
#include "core/log.h"

#include <miniaudio.h>

#include <map>
#include <set>
#include <utility>
#include <vector>

namespace engine::audio {

    namespace {

        namespace as = engine::assets;

        /// How long a volume change takes to arrive, in milliseconds. Long
        /// enough that no step in the samples is audible, short enough that a
        /// slider still feels like it did something.
        constexpr std::uint32_t kVolumeSmoothMs = 15;

        /// One loaded sound, kept in whichever form the cooker wrote.
        struct Sound {
            as::SoundStorage storage = as::SoundStorage::Pcm;
            std::uint32_t channels = 0;
            std::uint32_t sample_rate = 0;
            std::uint64_t frame_count = 0;
            /// PCM samples for a decoded sound, or the encoded file for a
            /// streamed one. A voice points into this, so it must not move.
            std::vector<std::byte> bytes;
        };

        /**
         * One playing sound.
         *
         * The data source lives here beside the voice, because `ma_sound` holds
         * a pointer to it rather than a copy of it. A voice therefore has to
         * stay put in memory, which is why the mixer keeps them by pointer.
         */
        struct Voice {
            ma_sound sound{};
            ma_audio_buffer_ref ref{}; ///< The cursor over a loaded PCM sound.
            ma_decoder decoder{};      ///< The decoder over a streamed sound.
            bool decodes = false;      ///< Which of the two above is in use.
            bool looping = false;
            bool spatial = false;
        };

        /// The miniaudio curve for one of ours. One name for one thing lives in
        /// audio/attenuation.h, and this is the only place that maps it.
        [[nodiscard]] ma_attenuation_model attenuation_of(Attenuation value) {
            switch (value) {
            case Attenuation::Linear:
                return ma_attenuation_model_linear;
            case Attenuation::Exponential:
                return ma_attenuation_model_exponential;
            case Attenuation::None:
                return ma_attenuation_model_none;
            case Attenuation::Inverse:
                break;
            }
            return ma_attenuation_model_inverse;
        }

    } // namespace

    /// Everything the mixer holds. It is here so the header names no miniaudio type.
    struct Mixer::State {
        ma_engine engine{};
        bool engine_open = false;
        std::uint32_t channels = 0;

        std::map<Guid, Sound> sounds;
        std::set<Guid> refused; ///< Asked for, would not load. Reported once.

        std::map<VoiceId, std::unique_ptr<Voice>> voices;
        VoiceId next_voice = 1;
        std::uint64_t started = 0;

        MixSettings mix;
        /// One group for each bus that something has played on. The master is
        /// not here: it is the engine's own output, so it needs no node.
        std::map<Bus, std::unique_ptr<ma_sound_group>> groups;

        /**
         * The mixing node for a bus, built the first time one is asked for.
         *
         * A method of State rather than of Mixer, because it names a miniaudio
         * type and the Mixer header names none.
         *
         * @param bus Which bus.
         * @return The group, or null for the master, which is the output
         *         itself.
         */
        [[nodiscard]] ma_sound_group* group_for(Bus bus);
    };

    namespace {

        /// The settings for one bus, to write. `settings_for` is the read half.
        [[nodiscard]] BusSettings& bus_of(MixSettings& settings, Bus bus) {
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

    } // namespace

    Mixer::Mixer()
        : state_(std::make_unique<State>()) {}

    Mixer::~Mixer() { destroy(); }

    bool Mixer::create(std::uint32_t channels, std::uint32_t sample_rate) {
        if (state_->engine_open) {
            ENGINE_LOG_ERROR("Audio: the mixer is already built.");
            return false;
        }
        if (channels == 0 || sample_rate == 0) {
            ENGINE_LOG_ERROR("Audio: a mixer of {} channels at {} Hz cannot play.", channels,
                             sample_rate);
            return false;
        }

        ma_engine_config config = ma_engine_config_init();
        // No device of its own. The engine advances when something reads it,
        // which is the device on its thread or a test on the spot. That is
        // also what keeps the silent device of M11.1 in charge of whether
        // anything is audible.
        config.noDevice = MA_TRUE;
        config.channels = channels;
        config.sampleRate = sample_rate;

        // A volume applied to the next sample is a step in the stream, and a
        // step is heard as a click. This ramps every volume change, on a voice
        // and on a bus alike. miniaudio defaults it to zero, which is the
        // clicking form.
        config.defaultVolumeSmoothTimeInPCMFrames = (sample_rate * kVolumeSmoothMs) / 1000U;

        const ma_result result = ma_engine_init(&config, &state_->engine);
        if (result != MA_SUCCESS) {
            ENGINE_LOG_ERROR("Audio: the mixer would not start ({}).",
                             ma_result_description(result));
            return false;
        }

        state_->engine_open = true;
        state_->channels = channels;
        return true;
    }

    void Mixer::destroy() {
        if (!state_->engine_open) {
            return;
        }
        stop_all();
        // The groups go after the voices, because a voice is attached to one.
        state_->groups.clear();
        ma_engine_uninit(&state_->engine);
        state_->engine_open = false;
        state_->sounds.clear();
        state_->refused.clear();
    }

    bool Mixer::load(const assets::AssetSource& content, Guid guid) {
        if (!state_->engine_open || !guid.valid()) {
            return false;
        }
        if (state_->sounds.contains(guid)) {
            return true;
        }
        if (state_->refused.contains(guid)) {
            return false;
        }

        std::vector<std::byte> bytes;
        if (!content.read(guid, bytes)) {
            ENGINE_LOG_ERROR("Audio: {} would not read, so it will not play.", guid.to_text());
            state_->refused.insert(guid);
            return false;
        }

        as::SoundView view;
        if (!as::read_sound(bytes, view, guid.to_text())) {
            state_->refused.insert(guid);
            return false;
        }

        Sound sound;
        sound.storage = view.storage;
        sound.channels = view.channels;
        sound.sample_rate = view.sample_rate;
        sound.frame_count = view.frame_count;
        sound.bytes.assign(view.payload.begin(), view.payload.end());
        state_->sounds.emplace(guid, std::move(sound));
        return true;
    }

    namespace {

        /// Points a voice at a loaded PCM sound, with a cursor of its own.
        [[nodiscard]] bool open_pcm(Voice& voice, const Sound& sound) {
            const ma_result result = ma_audio_buffer_ref_init(
                ma_format_f32, sound.channels, sound.bytes.data(), sound.frame_count,
                &voice.ref);
            if (result != MA_SUCCESS) {
                ENGINE_LOG_ERROR("Audio: a voice would not open ({}).",
                                 ma_result_description(result));
                return false;
            }

            // ma_audio_buffer_ref_init leaves the rate at zero, and it says so
            // with a TODO in its own source. A source of unknown rate is not
            // resampled, so a sound cooked at 44100 would play about nine
            // percent fast on a 48000 device, and only by ear.
            voice.ref.sampleRate = sound.sample_rate;
            return true;
        }

        /// Gives a voice a decoder of its own over a streamed sound.
        [[nodiscard]] bool open_encoded(Voice& voice, const Sound& sound, std::uint32_t channels,
                                        std::uint32_t sample_rate) {
            // The decoder converts to the mixer's own shape as it goes, so a
            // track of any rate or channel count arrives ready to mix. Nothing
            // decodes the whole file: a read pulls what the mixer asked for.
            ma_decoder_config config = ma_decoder_config_init(ma_format_f32, channels,
                                                              sample_rate);
            const ma_result result = ma_decoder_init_memory(sound.bytes.data(),
                                                            sound.bytes.size(), &config,
                                                            &voice.decoder);
            if (result != MA_SUCCESS) {
                ENGINE_LOG_ERROR("Audio: a streamed sound would not decode ({}).",
                                 ma_result_description(result));
                return false;
            }
            voice.decodes = true;
            return true;
        }

        /// Frees whatever a voice holds. Safe on a voice that never opened.
        void close(Voice& voice) {
            ma_sound_uninit(&voice.sound);
            if (voice.decodes) {
                ma_decoder_uninit(&voice.decoder);
            } else {
                ma_audio_buffer_ref_uninit(&voice.ref);
            }
        }

    } // namespace

    ma_sound_group* Mixer::State::group_for(Bus bus) {
        // The master is the engine's own output. A group in front of it would
        // be a node that only ever passes its input through.
        if (bus == Bus::Master) {
            return nullptr;
        }

        const auto found = groups.find(bus);
        if (found != groups.end()) {
            return found->second.get();
        }

        // Built on first use, so a bus nothing plays on costs no node and no
        // work in the mixer. Whatever set_bus() was told is applied here.
        auto group = std::make_unique<ma_sound_group>();
        const ma_result result = ma_sound_group_init(&engine, 0, nullptr, group.get());
        if (result != MA_SUCCESS) {
            ENGINE_LOG_ERROR("Audio: the {} bus would not open ({}). Its sounds play on the "
                             "master instead.",
                             to_text(bus), ma_result_description(result));
            return nullptr;
        }

        const BusSettings& settings = settings_for(mix, bus);
        ma_sound_group_set_volume(group.get(), settings.mute ? 0.0F : settings.volume);

        ma_sound_group* raw = group.get();
        groups.emplace(bus, std::move(group));
        return raw;
    }

    void Mixer::set_bus(Bus bus, const BusSettings& settings) {
        BusSettings& held = bus_of(state_->mix, bus);
        held = settings;
        if (!state_->engine_open) {
            return;
        }

        const float volume = settings.mute ? 0.0F : settings.volume;
        if (bus == Bus::Master) {
            ma_engine_set_volume(&state_->engine, volume);
            return;
        }

        // A bus nothing has played on yet is not built. The setting is kept
        // above, and group_for() applies it when the bus first exists.
        const auto found = state_->groups.find(bus);
        if (found != state_->groups.end()) {
            ma_sound_group_set_volume(found->second.get(), volume);
        }
    }

    const BusSettings& Mixer::bus_settings(Bus bus) const {
        return settings_for(state_->mix, bus);
    }

    std::size_t Mixer::buses() const { return state_->groups.size(); }

    VoiceId Mixer::play(const assets::AssetSource& content, Guid guid, const PlayDesc& desc) {
        if (!load(content, guid)) {
            return 0;
        }

        const Sound& sound = state_->sounds.at(guid);
        auto voice = std::make_unique<Voice>();

        const bool opened = sound.storage == as::SoundStorage::Pcm
                                ? open_pcm(*voice, sound)
                                : open_encoded(*voice, sound, state_->channels,
                                               ma_engine_get_sample_rate(&state_->engine));
        if (!opened) {
            return 0;
        }

        auto* source = voice->decodes ? static_cast<ma_data_source*>(&voice->decoder)
                                      : static_cast<ma_data_source*>(&voice->ref);
        // A voice that has no place in the world says so at init. Turning
        // spatialization off later still costs the work of it, and a sound that
        // is meant to be heard flat has to stay flat whatever the listener does.
        const ma_uint32 flags = desc.spatial ? 0U : MA_SOUND_FLAG_NO_SPATIALIZATION;
        const ma_result result = ma_sound_init_from_data_source(&state_->engine, source, flags,
                                                                state_->group_for(desc.bus),
                                                                &voice->sound);
        if (result != MA_SUCCESS) {
            ENGINE_LOG_ERROR("Audio: a voice would not start ({}).",
                             ma_result_description(result));
            if (voice->decodes) {
                ma_decoder_uninit(&voice->decoder);
            } else {
                ma_audio_buffer_ref_uninit(&voice->ref);
            }
            return 0;
        }

        ma_sound_set_volume(&voice->sound, desc.volume);
        ma_sound_set_pitch(&voice->sound, desc.pitch);
        ma_sound_set_looping(&voice->sound, desc.looping ? MA_TRUE : MA_FALSE);
        voice->looping = desc.looping;
        voice->spatial = desc.spatial;

        if (desc.spatial) {
            ma_sound_set_position(&voice->sound, desc.position.x, desc.position.y,
                                  desc.position.z);
            ma_sound_set_attenuation_model(&voice->sound, attenuation_of(desc.attenuation));
            ma_sound_set_min_distance(&voice->sound, desc.min_distance);
            ma_sound_set_max_distance(&voice->sound, desc.max_distance);
            ma_sound_set_rolloff(&voice->sound, desc.rolloff);
        }

        if (ma_sound_start(&voice->sound) != MA_SUCCESS) {
            close(*voice);
            return 0;
        }

        const VoiceId id = state_->next_voice;
        ++state_->next_voice;
        ++state_->started;
        state_->voices.emplace(id, std::move(voice));
        return id;
    }

    void Mixer::stop(VoiceId voice) {
        const auto found = state_->voices.find(voice);
        if (found == state_->voices.end()) {
            return;
        }
        close(*found->second);
        state_->voices.erase(found);
    }

    void Mixer::stop_all() {
        for (auto& [id, voice] : state_->voices) {
            close(*voice);
        }
        state_->voices.clear();
    }

    void Mixer::set_voice_position(VoiceId voice, const Vec3& position) {
        const auto found = state_->voices.find(voice);
        if (found == state_->voices.end() || !found->second->spatial) {
            return;
        }
        ma_sound_set_position(&found->second->sound, position.x, position.y, position.z);
    }

    void Mixer::set_listener(const Vec3& position, const Vec3& forward, const Vec3& up) {
        if (!state_->engine_open) {
            return;
        }
        // No conversion. miniaudio is right handed with forward at −Z, which is
        // what DESIGN.md section 3 says the engine is. A conversion here would
        // be the mirror it is meant to prevent.
        ma_engine_listener_set_position(&state_->engine, 0, position.x, position.y, position.z);
        ma_engine_listener_set_direction(&state_->engine, 0, forward.x, forward.y, forward.z);
        ma_engine_listener_set_world_up(&state_->engine, 0, up.x, up.y, up.z);
    }

    void Mixer::update() {
        for (auto at = state_->voices.begin(); at != state_->voices.end();) {
            // A looping voice never reaches its end, so this never frees one.
            if (ma_sound_at_end(&at->second->sound) == MA_TRUE) {
                close(*at->second);
                at = state_->voices.erase(at);
            } else {
                ++at;
            }
        }
    }

    bool Mixer::playing(VoiceId voice) const { return state_->voices.contains(voice); }

    std::size_t Mixer::voices() const { return state_->voices.size(); }

    std::uint64_t Mixer::started() const { return state_->started; }

    std::size_t Mixer::sounds() const { return state_->sounds.size(); }

    void Mixer::mix(float* output, std::uint32_t frames) {
        if (!state_->engine_open) {
            return;
        }
        ma_engine_read_pcm_frames(&state_->engine, output, frames, nullptr);
    }

} // namespace engine::audio
