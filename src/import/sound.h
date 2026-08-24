#pragma once

/**
 * @file
 * @brief The cooker rule that turns a sound file into a cooked sound.
 *
 * `src/assets/sound.h` holds the file format the two sides agree on, and the
 * runtime reads that and nothing else.
 *
 * **This rule needs no audio device and no mixer.** A short effect is decoded
 * here and a long track is passed through with its encoded bytes untouched. So
 * it runs on a build machine with no sound card, and a build with `with_audio`
 * off cooks the same tree to the same bytes.
 *
 * **It does link the decoding half of miniaudio**, through
 * `engine::audio::decode_encoded`. That library is built with `MA_NO_DEVICE_IO`
 * when `with_audio` is off, so a build with no audio carries the decoders and no
 * backend. Issue #424 is why: before it, only WAV decoded at cook time, so a
 * project keeping its effects compressed got none of the benefit of the PCM path.
 *
 * **Which decoder reads a file is decided by its bytes, not by its name.** A WAV
 * goes through decode_wav() below, which is unchanged so that every cooked WAV
 * keeps the bytes it already has. FLAC and MP3 go through miniaudio.
 *
 * **Ogg Vorbis is accepted and cannot be decoded.** miniaudio 0.11 carries no
 * Vorbis decoder, so an `.ogg` marked as an effect is refused by name here, and a
 * streamed one cooks and then cannot be played either. That is issue #477 and it
 * is older than the rule above.
 */

#include "assets/meta.h"
#include "assets/sound.h"
#include "import/writer.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace engine::import {

    /**
     * @brief Whether this rule handles a file with this extension.
     * @param extension The extension, with the dot, in any letter case.
     * @return True for a sound format the engine takes.
     */
    [[nodiscard]] bool is_sound_extension(const std::string& extension);

    /// @brief Interleaved float samples, and what they are.
    struct PcmAudio {
        std::uint32_t channels = 0;    ///< Channels. One is mono and two is stereo.
        std::uint32_t sample_rate = 0; ///< Frames each second.
        /// @brief The samples, interleaved by frame. Size is frames times channels.
        std::vector<float> samples;
    };

    /**
     * @brief Decodes a WAV file into interleaved float samples.
     *
     * It reads the shapes a tool writes: 8-bit unsigned, 16, 24 and 32-bit
     * signed integers, and 32-bit floats, in the plain and the extensible form.
     * It refuses anything else by name, because a wrong guess about a sample
     * format is heard as noise rather than seen as an error.
     *
     * The samples are not resampled and not mixed down. The mixer takes the
     * rate and the channel count the file had, so cooking changes the form and
     * not the sound.
     *
     * @param bytes The whole WAV file.
     * @param out The audio to fill.
     * @param where A name for the log, usually the source path.
     * @return True when the file decoded.
     */
    [[nodiscard]] bool decode_wav(std::span<const std::byte> bytes, PcmAudio& out,
                                  std::string_view where);

    /**
     * @brief Guesses whether a sound should stream, from its file name.
     *
     * A guess is only ever written into a new sidecar. After that the sidecar
     * decides, so a wrong guess is one edit to fix and it never comes back.
     *
     * Anything that is not a WAV is streamed, because the cook-time decoder
     * reads WAV alone. A short effect in another format is then one edit away
     * from being decoded, rather than a cook that fails.
     *
     * @param source The source sound path.
     * @return What to record in a new sidecar.
     */
    [[nodiscard]] bool guess_stream(const std::filesystem::path& source);

    /**
     * @brief Cooks one sound file into one cooked sound.
     *
     * @param source The sound to read.
     * @param writer Where the cooked file goes.
     * @param cooked The cooked path, relative to the cooked root.
     * @param settings What the sidecar says to do.
     * @return True when the cooked file was written. False reports why in the
     * log, by source path.
     */
    [[nodiscard]] bool cook_sound(const std::filesystem::path& source, Writer& writer,
                                  const std::filesystem::path& cooked,
                                  const engine::assets::SoundImport& settings);

    /**
     * @brief Cooks a sound already in memory into one cooked sound.
     *
     * This is what `cook_sound` does once it has the bytes. A test drives it
     * directly, so a case needs no file on disk.
     *
     * @param bytes The source file, encoded.
     * @param writer Where the cooked file goes.
     * @param cooked The cooked path, relative to the cooked root.
     * @param settings What the sidecar says to do.
     * @param where A name for the log, usually the source path.
     * @return True when the cooked file was written.
     */
    [[nodiscard]] bool cook_sound_bytes(std::span<const std::byte> bytes, Writer& writer,
                                        const std::filesystem::path& cooked,
                                        const engine::assets::SoundImport& settings,
                                        std::string_view where);

} // namespace engine::import
