#pragma once

/**
 * @file
 * @brief The cooked sound format, shared by the cooker and the runtime.
 *
 * A cooked sound is one file: a header, then the audio. The header says which
 * of the two forms the audio is in, and DESIGN.md section 10 M11 says why there
 * are two.
 *
 * **A short effect is stored as PCM.** The cooker decodes it once, so the
 * runtime decodes nothing on the load path and a sound that has to play the
 * moment a button is pressed is ready to mix.
 *
 * **A long track keeps its encoded bytes and streams.** Decoding a piece of
 * music at cook time turns a few megabytes into a few tens of megabytes in the
 * cooked tree, for a file that is played once and never in a hurry.
 *
 * The sidecar picks, and `src/import/sound.h` is the rule that reads it. This
 * header holds only what both sides must agree on, so a change here is a format
 * change and it moves the version below.
 */

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace engine::assets {

    /// @brief The name a cooked sound file carries after the source name.
    inline constexpr const char* kSoundExtension = ".snd";

    /**
     * @brief The first four bytes of a cooked sound file.
     *
     * The value spells "CSND" when a person opens the file in a hex viewer.
     * Reading it first turns a wrong file into a named error rather than into a
     * mixer reading nonsense as samples, which is a noise nobody wants to hear
     * twice.
     */
    inline constexpr std::uint32_t kSoundMagic = 0x444E5343U;

    /// @brief The format version this build writes and reads.
    inline constexpr std::uint32_t kSoundVersion = 1;

    /// @brief How the audio after the header is stored.
    enum class SoundStorage : std::uint32_t {
        /**
         * @brief Interleaved 32-bit float samples, ready to mix.
         *
         * Float rather than 16-bit integer, because the mixer works in float
         * and a conversion on the play path is work for every frame of every
         * voice. The cost is memory, and this form is for short sounds.
         */
        Pcm = 0,

        /**
         * @brief The encoded bytes of the source file, unchanged.
         *
         * The runtime decodes these as it plays. What the file holds is
         * whatever the source was, and the decoder works it out from the bytes
         * rather than from a name.
         */
        Encoded,
    };

    /// @brief The largest ::SoundStorage value, so a reader can reject the rest.
    inline constexpr std::uint32_t kSoundStorageMax = static_cast<std::uint32_t>(
        SoundStorage::Encoded);

    /// @brief Bytes one PCM sample takes. One float for each channel of a frame.
    inline constexpr std::uint32_t kPcmSampleBytes = 4;

    /**
     * @brief The fixed-size header at the start of a cooked sound file.
     *
     * Every field is a 32-bit unsigned integer, so the layout is the same on
     * both platforms and the struct needs no packing attribute.
     */
    struct SoundHeader {
        std::uint32_t magic = kSoundMagic;     ///< ::kSoundMagic. Checked first.
        std::uint32_t version = kSoundVersion; ///< ::kSoundVersion when written.
        std::uint32_t storage = 0;             ///< A ::SoundStorage value.
        /**
         * @brief Channels in the audio. One is mono and two is stereo.
         *
         * Zero for an encoded sound, because the cooker does not decode one and
         * so has nothing to report. The decoder answers at load time.
         */
        std::uint32_t channels = 0;
        /// @brief Frames each second. Zero for an encoded sound, for the same reason.
        std::uint32_t sample_rate = 0;
        /// @brief How many frames the PCM holds. Zero for an encoded sound.
        std::uint32_t frame_count = 0;
        /// @brief Bytes of audio after this header.
        std::uint32_t payload_size = 0;
    };

    /**
     * @brief A cooked sound that a caller already holds in memory.
     *
     * The bytes belong to the caller. This only points into them, so it stays
     * valid exactly as long as the buffer behind it does.
     */
    struct SoundView {
        SoundStorage storage = SoundStorage::Pcm; ///< Which form @ref payload is in.
        std::uint32_t channels = 0;               ///< Channels, or 0 when encoded.
        std::uint32_t sample_rate = 0;            ///< Frames each second, or 0 when encoded.
        std::uint32_t frame_count = 0;            ///< Frames of PCM, or 0 when encoded.
        std::span<const std::byte> payload;       ///< The audio, in the form @ref storage says.
    };

    /**
     * @brief How long a PCM sound lasts.
     * @param view A view of a cooked sound.
     * @return The length in seconds, or 0 for an encoded sound, whose length is
     * not known until something decodes it.
     */
    [[nodiscard]] float sound_seconds(const SoundView& view);

    /**
     * @brief Reads the header of a cooked sound and points at its audio.
     *
     * This checks the magic, the version, and that the payload is exactly the
     * size the header calls for. A PCM file whose frame count disagrees with
     * its payload is refused by name, because the alternative is a mixer
     * reading past the end of the buffer.
     *
     * @param bytes The whole file, as read from disk.
     * @param out The view to fill. It points into @p bytes.
     * @param where A name for the log, usually the asset path.
     * @return True when the file is a cooked sound this build understands.
     */
    [[nodiscard]] bool read_sound(std::span<const std::byte> bytes, SoundView& out,
                                  std::string_view where);

} // namespace engine::assets
