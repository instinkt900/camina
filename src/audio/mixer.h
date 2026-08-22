#pragma once

/**
 * @file
 * @brief What turns a cooked sound into something a device can play.
 *
 * The mixer holds one sound for each identity that was asked for, and one voice
 * for each sound that is playing. `audio::IAudioDevice` pulls frames from it,
 * and a game asks it to play.
 *
 * **This header names no miniaudio type**, the same way `audio/device.h` does
 * not. `scripts/check-miniaudio-containment.sh` keeps it that way.
 *
 * **A mixer needs no device.** It fills a buffer when something reads it, and a
 * device is only the usual thing doing the reading. So a test drives mix()
 * itself, pulls an exact number of frames, and reads what came out. That is a
 * check with no sound card, no thread and no wall clock in it.
 *
 * @code
 * engine::audio::Mixer mixer;
 * mixer.create(device->channels(), device->sample_rate());
 * device->set_source(&mixer);
 *
 * const auto voice = mixer.play(content, click_guid);
 *
 * // Once each frame. A one-shot that has ended is freed here.
 * mixer.update();
 * @endcode
 */

#include "assets/asset_source.h"
#include "audio/device.h"
#include "core/guid.h"

#include <cstddef>
#include <cstdint>
#include <memory>

namespace engine::audio {

    /// @brief One playing sound. Zero is no voice, and never a live one.
    using VoiceId = std::uint64_t;

    /// @brief How to play a sound.
    struct PlayDesc {
        /// @brief A straight multiplier on the samples. One is as cooked.
        float volume = 1.0F;

        /**
         * @brief Speed, and so pitch with it. One is as cooked.
         *
         * This is the same knob a tape speed is. Two plays twice as fast and an
         * octave up.
         */
        float pitch = 1.0F;

        /**
         * @brief Start again at the end, forever.
         *
         * A looping voice never ends on its own, so update() never frees it.
         * Whoever started it has to stop it.
         */
        bool looping = false;
    };

    /**
     * @brief Holds every sound that was asked for, and every voice playing one.
     *
     * A sound is loaded once, however many voices play it. A PCM sound is kept
     * as the samples the cooker wrote, and each voice reads them with a cursor
     * of its own. An encoded sound is kept as the bytes the cooker wrote, and
     * each voice decodes as it plays, so a long track never exists decoded.
     */
    class Mixer final : public IAudioSource {
    public:
        Mixer();
        ~Mixer() override;

        Mixer(const Mixer&) = delete;
        Mixer& operator=(const Mixer&) = delete;
        Mixer(Mixer&&) = delete;
        Mixer& operator=(Mixer&&) = delete;

        /**
         * @brief Builds the mixer for one output shape.
         *
         * Call this once, before anything else. It is separate from the
         * constructor because it can fail. Take the numbers from the device, and
         * read them back off the device rather than assuming what was asked for.
         *
         * @param channels Output channels. Two is stereo.
         * @param sample_rate Output frames each second.
         * @return True when the mixer is ready to play.
         */
        [[nodiscard]] bool create(std::uint32_t channels, std::uint32_t sample_rate);

        /**
         * @brief Stops every voice and frees every sound.
         *
         * Take the mixer off the device before this runs, with
         * `IAudioDevice::set_source(nullptr)`. The destructor calls this.
         */
        void destroy();

        /**
         * @brief Loads a sound now rather than when it is first played.
         *
         * A first play has to read the asset, and reading it is the slow part.
         * A game that cares when that happens loads ahead of time.
         *
         * @param content The assets to read from, cooked or imported.
         * @param guid The sound identity.
         * @return True when the sound is loaded and playable.
         */
        [[nodiscard]] bool load(const assets::AssetSource& content, Guid guid);

        /**
         * @brief Plays a sound, loading it the first time it is asked for.
         *
         * A GUID that will not load is remembered as a failure, so a game that
         * plays a missing sound on every step reports once rather than forever.
         *
         * @param content The assets to read from, cooked or imported.
         * @param guid The sound identity.
         * @param desc How to play it.
         * @return The voice, or zero when nothing is playing.
         */
        [[nodiscard]] VoiceId play(const assets::AssetSource& content, Guid guid,
                                   const PlayDesc& desc = {});

        /**
         * @brief Stops one voice and frees it.
         *
         * A voice that has already ended, or that never existed, is not an
         * error. Nothing happens.
         *
         * @param voice The voice to stop.
         */
        void stop(VoiceId voice);

        /// @brief Stops every voice and frees them all. The sounds stay loaded.
        void stop_all();

        /**
         * @brief Frees every voice that has reached its end.
         *
         * Call this once each frame. A one-shot holds a decoder or a cursor
         * until it is freed, and nothing else frees one: the device thread must
         * not, because freeing takes a lock and allocates.
         *
         * A looping voice never ends, so this never frees one.
         */
        void update();

        /// @return How many voices are playing right now.
        [[nodiscard]] std::size_t voices() const;

        /**
         * @brief How many voices have ever been started.
         *
         * A voice count that only grows is a leak, and the count alone cannot
         * say whether it is one. This is the other half: started() climbing
         * while voices() stays flat is a game playing sounds and cleaning up
         * after them.
         *
         * @return The count since create().
         */
        [[nodiscard]] std::uint64_t started() const;

        /// @return How many distinct sounds are loaded.
        [[nodiscard]] std::size_t sounds() const;

        /**
         * @brief Fills a buffer with the next frames. See IAudioSource.
         *
         * A device calls this on its own thread. A test calls it directly, and
         * then the mixer advances by exactly what the test asked for.
         *
         * @param output Interleaved float samples. It holds @p frames times the
         *        channel count create() was given.
         * @param frames How many frames to write.
         */
        void mix(float* output, std::uint32_t frames) override;

    private:
        struct State;
        std::unique_ptr<State> state_;
    };

} // namespace engine::audio
