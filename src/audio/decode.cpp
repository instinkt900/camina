#include "audio/decode.h"

// The config first, always. It carries the macros miniaudio is built with, and
// the declarations and the implementation have to agree about them. It looks
// unused because it only defines macros. Removing it changes what a ma_decoder
// is in this file and nothing else, which links cleanly and goes wrong at run
// time.
// NOLINTNEXTLINE(misc-include-cleaner)
#include "audio/miniaudio_config.h"

#include "core/log.h"

#include <miniaudio.h>

#include <limits>

namespace engine::audio {

    namespace {

        /// Frees a decoder however the function returns.
        class Decoder {
        public:
            Decoder() = default;

            Decoder(const Decoder&) = delete;
            Decoder& operator=(const Decoder&) = delete;
            Decoder(Decoder&&) = delete;
            Decoder& operator=(Decoder&&) = delete;

            ~Decoder() {
                if (open_) {
                    ma_decoder_uninit(&decoder_);
                }
            }

            /// Opens the bytes. False when miniaudio does not recognize them.
            [[nodiscard]] bool open(std::span<const std::byte> bytes, ma_result& result) {
                // Float output, and the source rate and channel count kept.
                // Zero means "whatever the file says", which is the promise the
                // header makes about not resampling.
                ma_decoder_config config = ma_decoder_config_init(ma_format_f32, 0, 0);
                result = ma_decoder_init_memory(bytes.data(), bytes.size(), &config, &decoder_);
                open_ = result == MA_SUCCESS;
                return open_;
            }

            [[nodiscard]] ma_decoder& get() { return decoder_; }

        private:
            ma_decoder decoder_{};
            bool open_ = false;
        };

    } // namespace

    bool decode_encoded(std::span<const std::byte> bytes, DecodedAudio& out,
                        std::string_view where) {
        if (bytes.empty()) {
            ENGINE_LOG_ERROR("{}: it holds no bytes, so there is nothing to decode.", where);
            return false;
        }

        Decoder decoder;
        ma_result result = MA_SUCCESS;
        if (!decoder.open(bytes, result)) {
            // miniaudio reads the format out of the bytes, so this is also what
            // catches a file whose extension says one thing and whose contents
            // say another.
            ENGINE_LOG_ERROR("{}: it does not open as Ogg Vorbis, MP3 or FLAC ({}).", where,
                             static_cast<int>(result));
            return false;
        }

        ma_uint64 frames = 0;
        result = ma_decoder_get_length_in_pcm_frames(&decoder.get(), &frames);
        if (result != MA_SUCCESS) {
            // A stream with no length miniaudio can work out. Nothing here can
            // size a buffer for it, and the streamed path takes it instead.
            ENGINE_LOG_ERROR("{}: its length could not be read, so it cannot be decoded at "
                             "cook time. Mark it streamed.",
                             where);
            return false;
        }

        const ma_uint32 channels = decoder.get().outputChannels;
        const ma_uint32 sample_rate = decoder.get().outputSampleRate;
        if (channels == 0 || sample_rate == 0) {
            ENGINE_LOG_ERROR("{}: it says {} channels at {} Hz, which cannot be played.", where,
                             channels, sample_rate);
            return false;
        }
        if (frames == 0) {
            ENGINE_LOG_ERROR("{}: it decodes to no frames at all.", where);
            return false;
        }

        // Guarded before the allocation rather than after it. The product is
        // what gets allocated, and a file that claims an absurd length would
        // otherwise ask for that much memory before anything checked.
        const ma_uint64 samples = frames * channels;
        if (samples > std::numeric_limits<std::uint32_t>::max()) {
            ENGINE_LOG_ERROR("{}: it decodes to {} samples, which is more than a cooked sound "
                             "records. Mark it streamed.",
                             where, samples);
            return false;
        }

        std::vector<float> decoded(static_cast<std::size_t>(samples));
        ma_uint64 read = 0;
        result = ma_decoder_read_pcm_frames(&decoder.get(), decoded.data(), frames, &read);
        if (result != MA_SUCCESS && result != MA_AT_END) {
            ENGINE_LOG_ERROR("{}: it stopped decoding after {} of {} frames ({}).", where, read,
                             frames, static_cast<int>(result));
            return false;
        }
        if (read != frames) {
            // Short is not an error the way a failed read is: the length was an
            // estimate for some formats. Keep what arrived and say so, rather
            // than writing the silence the rest of the buffer holds.
            ENGINE_LOG_WARN("{}: it said {} frames and gave {}. Keeping what it gave.", where,
                            frames, read);
            decoded.resize(static_cast<std::size_t>(read) * channels);
        }
        if (decoded.empty()) {
            ENGINE_LOG_ERROR("{}: it decoded to nothing.", where);
            return false;
        }

        out.channels = channels;
        out.sample_rate = sample_rate;
        out.samples = std::move(decoded);
        return true;
    }

} // namespace engine::audio
