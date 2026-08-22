#include "import/sound.h"

#include "core/log.h"
#include "import/rules.h"

#include <cstring>
#include <fstream>
#include <limits>

namespace engine::import {

    namespace {

        namespace as = engine::assets;

        /// The WAV format tags this reader knows. The rest are refused by name.
        constexpr std::uint16_t kFormatPcm = 1;
        constexpr std::uint16_t kFormatFloat = 3;
        constexpr std::uint16_t kFormatExtensible = 0xFFFE;

        /// The smallest a RIFF header can be: "RIFF", a size, and "WAVE".
        constexpr std::size_t kRiffHeaderBytes = 12;

        /// Every chunk opens with a four byte name and a four byte size.
        constexpr std::size_t kChunkHeaderBytes = 8;

        /// What one `fmt ` chunk says, before any extension it carries.
        struct WaveFormat {
            std::uint16_t tag = 0;
            std::uint16_t channels = 0;
            std::uint32_t sample_rate = 0;
            std::uint16_t bits = 0;
        };

        /// Where the samples are, and how many bytes of them there are.
        struct WaveData {
            std::size_t offset = 0;
            std::size_t size = 0;
        };

        /// Reads a little endian 16-bit number. WAV is little endian whatever
        /// the machine is, so the bytes are assembled rather than copied.
        [[nodiscard]] std::uint16_t read_u16(std::span<const std::byte> bytes, std::size_t at) {
            return static_cast<std::uint16_t>(static_cast<std::uint16_t>(bytes[at]) |
                                              static_cast<std::uint16_t>(
                                                  static_cast<std::uint16_t>(bytes[at + 1]) << 8U));
        }

        /// Reads a little endian 32-bit number, for the same reason.
        [[nodiscard]] std::uint32_t read_u32(std::span<const std::byte> bytes, std::size_t at) {
            std::uint32_t value = 0;
            for (std::size_t i = 0; i < 4; ++i) {
                value |= static_cast<std::uint32_t>(bytes[at + i]) << (8U * i);
            }
            return value;
        }

        /// Whether four bytes at an offset spell this name.
        [[nodiscard]] bool is_chunk(std::span<const std::byte> bytes, std::size_t at,
                                    const char* name) {
            for (std::size_t i = 0; i < 4; ++i) {
                if (static_cast<char>(bytes[at + i]) != name[i]) {
                    return false;
                }
            }
            return true;
        }

        /// Reads one `fmt ` chunk, including the tag an extensible one hides.
        [[nodiscard]] bool read_format(std::span<const std::byte> bytes, std::size_t at,
                                       std::uint32_t size, WaveFormat& out,
                                       std::string_view where) {
            constexpr std::uint32_t kMinFormatBytes = 16;
            if (size < kMinFormatBytes) {
                ENGINE_LOG_ERROR("{}: its fmt chunk is {} bytes, and a WAV needs 16.", where,
                                 size);
                return false;
            }

            out.tag = read_u16(bytes, at);
            out.channels = read_u16(bytes, at + 2);
            out.sample_rate = read_u32(bytes, at + 4);
            out.bits = read_u16(bytes, at + 14);

            if (out.tag != kFormatExtensible) {
                return true;
            }

            // An extensible chunk keeps the real tag in the first two bytes of
            // a sub-format GUID, 24 bytes in. A tool writes this form whenever
            // the file has more than two channels, and rejecting it would
            // refuse ordinary files.
            constexpr std::uint32_t kExtensibleBytes = 40;
            constexpr std::size_t kSubFormatAt = 24;
            if (size < kExtensibleBytes) {
                ENGINE_LOG_ERROR("{}: it says extensible and its fmt chunk is only {} bytes.",
                                 where, size);
                return false;
            }
            out.tag = read_u16(bytes, at + kSubFormatAt);
            return true;
        }

        /// Walks the chunks of a RIFF file and picks out `fmt ` and `data`.
        [[nodiscard]] bool find_chunks(std::span<const std::byte> bytes, WaveFormat& format,
                                       WaveData& data, std::string_view where) {
            bool have_format = false;
            std::size_t at = kRiffHeaderBytes;

            while (at + kChunkHeaderBytes <= bytes.size()) {
                const std::uint32_t size = read_u32(bytes, at + 4);
                const std::size_t body = at + kChunkHeaderBytes;
                if (size > bytes.size() - body) {
                    ENGINE_LOG_ERROR("{}: a chunk says {} bytes and the file has {} left.",
                                     where, size, bytes.size() - body);
                    return false;
                }

                if (is_chunk(bytes, at, "fmt ")) {
                    if (!read_format(bytes, body, size, format, where)) {
                        return false;
                    }
                    have_format = true;
                } else if (is_chunk(bytes, at, "data")) {
                    if (!have_format) {
                        ENGINE_LOG_ERROR("{}: its data chunk comes before its fmt chunk.",
                                         where);
                        return false;
                    }
                    data.offset = body;
                    data.size = size;
                    // The samples are found. Nothing after them is read, so a
                    // trailing chunk a tool added costs nothing.
                    return true;
                }

                // A chunk body is padded to an even length, and the pad byte is
                // not counted in the size.
                at = body + size + (size % 2);
            }

            ENGINE_LOG_ERROR("{}: it holds no data chunk.", where);
            return false;
        }

        /// Turns one integer sample of any width into a float in [-1, 1].
        [[nodiscard]] float integer_sample(std::span<const std::byte> bytes, std::size_t at,
                                           std::uint16_t bits) {
            constexpr float kU8Middle = 128.0F;
            if (bits == 8) {
                // 8-bit WAV is unsigned, with silence at 128. Every wider form
                // is signed, with silence at zero.
                return (static_cast<float>(bytes[at]) - kU8Middle) / kU8Middle;
            }

            // Assemble the sample, then shift it up so its sign sits in bit 31.
            // Every width then divides by the same number and none of them
            // needs a case of its own. Sign extending in the file's own width
            // instead works too, and it reads as if it does not: at 32 bits it
            // relies on the subtraction wrapping to zero and on the cast having
            // already extended the sign.
            const std::size_t width = bits / 8U;
            std::uint32_t raw = 0;
            for (std::size_t i = 0; i < width; ++i) {
                raw |= static_cast<std::uint32_t>(bytes[at + i]) << (8U * i);
            }
            raw <<= (32U - bits);

            constexpr float kFullScale = 2147483648.0F;
            return static_cast<float>(static_cast<std::int32_t>(raw)) / kFullScale;
        }

        /// Turns the data chunk into interleaved floats.
        [[nodiscard]] bool convert(std::span<const std::byte> bytes, const WaveFormat& format,
                                   const WaveData& data, PcmAudio& out) {
            const std::size_t width = format.bits / 8U;
            const std::size_t count = data.size / width;
            out.samples.resize(count);

            if (format.tag == kFormatFloat) {
                for (std::size_t i = 0; i < count; ++i) {
                    std::uint32_t raw = read_u32(bytes, data.offset + (i * width));
                    float value = 0.0F;
                    std::memcpy(&value, &raw, sizeof(value));
                    out.samples[i] = value;
                }
                return true;
            }

            for (std::size_t i = 0; i < count; ++i) {
                out.samples[i] = integer_sample(bytes, data.offset + (i * width), format.bits);
            }
            return true;
        }

        /// Whether this reader can turn samples of this shape into floats.
        [[nodiscard]] bool format_is_readable(const WaveFormat& format, std::string_view where) {
            if (format.tag != kFormatPcm && format.tag != kFormatFloat) {
                ENGINE_LOG_ERROR("{}: its samples are format {}, and this reads integer PCM "
                                 "and float only.",
                                 where, format.tag);
                return false;
            }
            if (format.tag == kFormatFloat && format.bits != 32) {
                ENGINE_LOG_ERROR("{}: it holds {}-bit floats, and this reads 32-bit floats.",
                                 where, format.bits);
                return false;
            }
            if (format.tag == kFormatPcm && format.bits != 8 && format.bits != 16 &&
                format.bits != 24 && format.bits != 32) {
                ENGINE_LOG_ERROR("{}: it holds {}-bit samples, and this reads 8, 16, 24 and 32.",
                                 where, format.bits);
                return false;
            }
            if (format.channels == 0 || format.sample_rate == 0) {
                ENGINE_LOG_ERROR("{}: it says {} channels at {} Hz, which cannot be played.",
                                 where, format.channels, format.sample_rate);
                return false;
            }
            return true;
        }

    } // namespace

    bool is_sound_extension(const std::string& extension) {
        return extension == ".wav" || extension == ".ogg" || extension == ".mp3" ||
               extension == ".flac";
    }

    bool decode_wav(std::span<const std::byte> bytes, PcmAudio& out, std::string_view where) {
        if (bytes.size() < kRiffHeaderBytes || !is_chunk(bytes, 0, "RIFF") ||
            !is_chunk(bytes, 8, "WAVE")) {
            ENGINE_LOG_ERROR("{}: it does not open as a RIFF WAVE file.", where);
            return false;
        }

        WaveFormat format;
        WaveData data;
        if (!find_chunks(bytes, format, data, where)) {
            return false;
        }
        if (!format_is_readable(format, where)) {
            return false;
        }

        const std::size_t frame_bytes = static_cast<std::size_t>(format.channels) *
                                        (format.bits / 8U);
        if (frame_bytes == 0 || data.size % frame_bytes != 0) {
            ENGINE_LOG_ERROR("{}: its {} bytes of samples do not divide into whole frames.",
                             where, data.size);
            return false;
        }

        out.channels = format.channels;
        out.sample_rate = format.sample_rate;
        return convert(bytes, format, data, out);
    }

    bool guess_stream(const std::filesystem::path& source) {
        return lowered_extension(source) != ".wav";
    }

    bool cook_sound_bytes(std::span<const std::byte> bytes, Writer& writer,
                          const std::filesystem::path& cooked, const as::SoundImport& settings,
                          std::string_view where) {
        as::SoundHeader header;

        if (settings.stream) {
            // The bytes go through untouched and the runtime decoder reads
            // them. Nothing here knows what is in them, so the header says
            // nothing about the audio and the decoder answers at load time.
            if (bytes.size() > std::numeric_limits<std::uint32_t>::max()) {
                ENGINE_LOG_ERROR("{}: it is {} bytes, which is more than a cooked sound "
                                 "records.",
                                 where, bytes.size());
                return false;
            }
            header.storage = static_cast<std::uint32_t>(as::SoundStorage::Encoded);
            header.payload_size = static_cast<std::uint32_t>(bytes.size());
            return write_with_header(writer, cooked, header, bytes);
        }

        PcmAudio audio;
        if (!decode_wav(bytes, audio, where)) {
            return false;
        }

        const std::size_t payload = audio.samples.size() * sizeof(float);
        if (payload > std::numeric_limits<std::uint32_t>::max()) {
            ENGINE_LOG_ERROR("{}: decoded it is {} bytes, which is more than a cooked sound "
                             "records. Mark it streamed.",
                             where, payload);
            return false;
        }

        header.storage = static_cast<std::uint32_t>(as::SoundStorage::Pcm);
        header.channels = audio.channels;
        header.sample_rate = audio.sample_rate;
        header.frame_count = static_cast<std::uint32_t>(audio.samples.size() / audio.channels);
        header.payload_size = static_cast<std::uint32_t>(payload);

        return write_with_header(writer, cooked, header, std::as_bytes(std::span(audio.samples)));
    }

    bool cook_sound(const std::filesystem::path& source, Writer& writer,
                    const std::filesystem::path& cooked, const as::SoundImport& settings) {
        if (!settings.stream && lowered_extension(source) != ".wav") {
            ENGINE_LOG_ERROR("{}: only a WAV decodes at cook time. Set stream in its sidecar, "
                             "or give it as a WAV.",
                             source.string());
            return false;
        }

        std::ifstream file(source, std::ios::binary | std::ios::ate);
        if (!file) {
            ENGINE_LOG_ERROR("{}: could not open it to cook.", source.string());
            return false;
        }
        const auto size = static_cast<std::streamsize>(file.tellg());
        file.seekg(0);
        std::vector<std::byte> bytes(static_cast<std::size_t>(size));
        if (size > 0 && !file.read(reinterpret_cast<char*>(bytes.data()), size)) {
            ENGINE_LOG_ERROR("{}: the read failed part way through.", source.string());
            return false;
        }

        return cook_sound_bytes(bytes, writer, cooked, settings, source.string());
    }

} // namespace engine::import
