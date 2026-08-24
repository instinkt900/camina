#pragma once

/**
 * @file
 * @brief Turning encoded audio bytes into samples, with no device and no mixer.
 *
 * This is the decoding half of miniaudio on its own. It opens nothing, plays
 * nothing, and it works in a build with `with_audio` off, because a cook has to
 * produce the same bytes whatever that option says.
 *
 * `src/import/sound.cpp` is the caller. A short effect is decoded once here at
 * cook time so that nothing decodes while it plays. Before M11 this reached WAV
 * alone, and an effect supplied as an `.ogg`, an `.mp3` or a `.flac` had to be
 * streamed. See issue #424.
 *
 * **WAV does not come through here.** `import::decode_wav` reads it, and it
 * stays that way because it already exists, it is tested, and moving it would
 * change the bytes every cooked WAV in every project already has.
 *
 * This header names no miniaudio type, which is what keeps miniaudio inside
 * `src/audio/`. See `scripts/check-miniaudio-containment.sh`.
 */

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace engine::audio {

    /// @brief Interleaved float samples, and what they are.
    struct DecodedAudio {
        std::uint32_t channels = 0;    ///< Channels. One is mono and two is stereo.
        std::uint32_t sample_rate = 0; ///< Frames each second.
        /// @brief The samples, interleaved by frame. Size is frames times channels.
        std::vector<float> samples;
    };

    /**
     * @brief Decodes Ogg Vorbis, MP3 or FLAC bytes into interleaved floats.
     *
     * The format is worked out from the bytes rather than from a file name, so
     * a file with the wrong extension is refused rather than read wrongly.
     *
     * Nothing is resampled and nothing is mixed down. The rate and the channel
     * count are the ones the file carried, so this changes the form and not the
     * sound. That is the same promise `import::decode_wav` makes.
     *
     * @param bytes The whole encoded file.
     * @param out Receives the samples. Untouched when this fails.
     * @param where A name for the log, normally the source path.
     * @return True when it decoded. False is reported by name first.
     *
     * @warning **The whole file is decoded into memory.** A long track must be
     * streamed instead. `import::cook_sound_bytes` decides which, and a sidecar
     * overrides it.
     */
    [[nodiscard]] bool decode_encoded(std::span<const std::byte> bytes, DecodedAudio& out,
                                      std::string_view where);

} // namespace engine::audio
