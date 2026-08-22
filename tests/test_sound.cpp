// M11.2. The sound asset: the WAV reader, the cooker rule, and the format the
// runtime reads back.
//
// Every case here builds its own WAV in memory and cooks through a
// MemoryWriter, so the test opens no file, needs no cooked tree, and needs no
// audio device. The rule decodes at cook time and links no miniaudio, which is
// what makes that possible.

#include "assets/meta.h"
#include "assets/sound.h"
#include "import/rules.h"
#include "import/sound.h"
#include "import/writer.h"

#include "check.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <vector>

namespace {

    namespace as = engine::assets;
    namespace im = engine::import;

    // The plain and the extensible format tags. A WAV writer picks the second
    // one whenever the file has more than two channels.
    constexpr std::uint16_t kPcm = 1;
    constexpr std::uint16_t kFloat = 3;
    constexpr std::uint16_t kExtensible = 0xFFFE;

    void put_u16(std::vector<std::byte>& out, std::uint16_t value) {
        out.push_back(static_cast<std::byte>(value & 0xFFU));
        out.push_back(static_cast<std::byte>((value >> 8U) & 0xFFU));
    }

    void put_u32(std::vector<std::byte>& out, std::uint32_t value) {
        for (int i = 0; i < 4; ++i) {
            out.push_back(static_cast<std::byte>((value >> (8U * static_cast<unsigned>(i))) &
                                                 0xFFU));
        }
    }

    void put_name(std::vector<std::byte>& out, const char* name) {
        for (int i = 0; i < 4; ++i) {
            out.push_back(static_cast<std::byte>(name[i]));
        }
    }

    /// What one generated WAV holds.
    struct WavSpec {
        std::uint16_t tag = kPcm;
        std::uint16_t bits = 16;
        std::uint16_t channels = 1;
        std::uint32_t sample_rate = 48000;
        bool extensible = false;  ///< Write the 40-byte fmt chunk with a sub-format.
        bool extra_chunk = false; ///< Put a LIST chunk between fmt and data.
        bool data_first = false;  ///< Write data before fmt, which is not legal.
        std::vector<std::byte> data;
    };

    /// Builds a WAV file in memory from a spec.
    std::vector<std::byte> make_wav(const WavSpec& spec) {
        std::vector<std::byte> fmt;
        put_u16(fmt, spec.extensible ? kExtensible : spec.tag);
        put_u16(fmt, spec.channels);
        put_u32(fmt, spec.sample_rate);
        const auto block = static_cast<std::uint16_t>(spec.channels * (spec.bits / 8));
        put_u32(fmt, spec.sample_rate * block);
        put_u16(fmt, block);
        put_u16(fmt, spec.bits);
        if (spec.extensible) {
            put_u16(fmt, 22);        // cbSize
            put_u16(fmt, spec.bits); // valid bits
            put_u32(fmt, 0);         // channel mask
            put_u16(fmt, spec.tag);  // the real tag, first in the sub-format GUID
            for (int i = 0; i < 14; ++i) {
                fmt.push_back(std::byte{ 0 });
            }
        }

        std::vector<std::byte> body;
        const auto chunk = [&body](const char* name, const std::vector<std::byte>& bytes) {
            put_name(body, name);
            put_u32(body, static_cast<std::uint32_t>(bytes.size()));
            body.insert(body.end(), bytes.begin(), bytes.end());
            if (bytes.size() % 2 != 0) {
                body.push_back(std::byte{ 0 });
            }
        };

        put_name(body, "WAVE");
        if (spec.data_first) {
            chunk("data", spec.data);
            chunk("fmt ", fmt);
        } else {
            chunk("fmt ", fmt);
            if (spec.extra_chunk) {
                chunk("LIST", std::vector<std::byte>(7, std::byte{ 0x41 }));
            }
            chunk("data", spec.data);
        }

        std::vector<std::byte> whole;
        put_name(whole, "RIFF");
        put_u32(whole, static_cast<std::uint32_t>(body.size()));
        whole.insert(whole.end(), body.begin(), body.end());
        return whole;
    }

    /// Samples of a given width, written little endian.
    template <typename T>
    std::vector<std::byte> samples_of(const std::vector<T>& values) {
        std::vector<std::byte> out;
        for (const T value : values) {
            const auto raw = static_cast<std::uint64_t>(static_cast<std::make_unsigned_t<T>>(
                value));
            for (std::size_t i = 0; i < sizeof(T); ++i) {
                out.push_back(static_cast<std::byte>((raw >> (8U * i)) & 0xFFU));
            }
        }
        return out;
    }

    bool near(float a, float b) { return std::fabs(a - b) < 0.0005F; }

    void test_decode_16_bit() {
        test::section("A 16-bit WAV decodes to floats");

        WavSpec spec;
        spec.data = samples_of<std::int16_t>({ 0, 16384, -16384, 32767 });
        im::PcmAudio audio;
        test::check(im::decode_wav(make_wav(spec), audio, "16-bit"), "it decoded");
        test::check(audio.channels == 1, "one channel");
        test::check(audio.sample_rate == 48000, "48000 Hz");
        test::check(audio.samples.size() == 4, "four samples");
        if (audio.samples.size() == 4) {
            test::check(near(audio.samples[0], 0.0F), "silence is zero");
            test::check(near(audio.samples[1], 0.5F), "half scale is 0.5");
            test::check(near(audio.samples[2], -0.5F), "negative half scale is -0.5");
            test::check(audio.samples[3] > 0.999F && audio.samples[3] <= 1.0F,
                        "full scale reaches 1 without passing it");
        }
    }

    void test_decode_every_width() {
        test::section("Every sample width this reader claims gives the same value");

        // Half scale in each form. They must all decode to 0.5, because the
        // whole point of the reader is that the width stops mattering here.
        {
            WavSpec spec;
            spec.bits = 8;
            spec.data = { std::byte{ 192 } }; // 8-bit WAV is unsigned, silence at 128.
            im::PcmAudio audio;
            test::check(im::decode_wav(make_wav(spec), audio, "8-bit") &&
                            audio.samples.size() == 1 && near(audio.samples[0], 0.5F),
                        "8-bit unsigned");
        }
        {
            WavSpec spec;
            spec.bits = 24;
            spec.data = { std::byte{ 0 }, std::byte{ 0 }, std::byte{ 0x40 } };
            im::PcmAudio audio;
            test::check(im::decode_wav(make_wav(spec), audio, "24-bit") &&
                            audio.samples.size() == 1 && near(audio.samples[0], 0.5F),
                        "24-bit signed");
        }
        {
            WavSpec spec;
            spec.bits = 32;
            spec.data = samples_of<std::int32_t>({ 1073741824 });
            im::PcmAudio audio;
            test::check(im::decode_wav(make_wav(spec), audio, "32-bit") &&
                            audio.samples.size() == 1 && near(audio.samples[0], 0.5F),
                        "32-bit signed");
        }
        {
            // A 32-bit signed sample at the bottom of the range. This is the one
            // that a sign extension by shifting in the file's own width gets
            // wrong, because that shift is 32 and undefined.
            WavSpec spec;
            spec.bits = 32;
            spec.data = samples_of<std::int32_t>({ -2147483647 - 1 });
            im::PcmAudio audio;
            test::check(im::decode_wav(make_wav(spec), audio, "32-bit floor") &&
                            audio.samples.size() == 1 && near(audio.samples[0], -1.0F),
                        "32-bit signed at the floor");
        }
        {
            WavSpec spec;
            spec.tag = kFloat;
            spec.bits = 32;
            const float value = 0.5F;
            std::vector<std::byte> bytes(sizeof(value));
            std::memcpy(bytes.data(), &value, sizeof(value));
            spec.data = bytes;
            im::PcmAudio audio;
            test::check(im::decode_wav(make_wav(spec), audio, "float") &&
                            audio.samples.size() == 1 && near(audio.samples[0], 0.5F),
                        "32-bit float");
        }
    }

    void test_decode_shapes() {
        test::section("The shapes a WAV writer produces");

        {
            WavSpec spec;
            spec.channels = 2;
            spec.data = samples_of<std::int16_t>({ 16384, -16384 });
            im::PcmAudio audio;
            test::check(im::decode_wav(make_wav(spec), audio, "stereo") && audio.channels == 2 &&
                            audio.samples.size() == 2,
                        "stereo gives interleaved samples and one frame");
        }
        {
            WavSpec spec;
            spec.extensible = true;
            spec.channels = 2;
            spec.data = samples_of<std::int16_t>({ 16384, 16384 });
            im::PcmAudio audio;
            test::check(im::decode_wav(make_wav(spec), audio, "extensible") &&
                            audio.samples.size() == 2 && near(audio.samples[0], 0.5F),
                        "the extensible form reads its real tag");
        }
        {
            // A chunk the reader knows nothing about sits between fmt and data
            // in plenty of real files, and its odd length is padded.
            WavSpec spec;
            spec.extra_chunk = true;
            spec.data = samples_of<std::int16_t>({ 16384 });
            im::PcmAudio audio;
            test::check(im::decode_wav(make_wav(spec), audio, "extra chunk") &&
                            audio.samples.size() == 1 && near(audio.samples[0], 0.5F),
                        "a chunk it does not know is stepped over, padding included");
        }
    }

    void test_decode_refusals() {
        test::section("A file it cannot read is refused by name, never guessed at");

        im::PcmAudio audio;
        {
            const std::vector<std::byte> not_riff(64, std::byte{ 0x7F });
            test::check(!im::decode_wav(not_riff, audio, "not riff"), "a file that is not RIFF");
        }
        {
            WavSpec spec;
            spec.data_first = true;
            spec.data = samples_of<std::int16_t>({ 1 });
            test::check(!im::decode_wav(make_wav(spec), audio, "data first"),
                        "data before fmt, which leaves the samples undescribed");
        }
        {
            WavSpec spec;
            spec.bits = 12;
            spec.data = samples_of<std::int16_t>({ 1 });
            test::check(!im::decode_wav(make_wav(spec), audio, "12-bit"),
                        "a width it does not read");
        }
        {
            WavSpec spec;
            spec.channels = 2;
            spec.bits = 16;
            // Three samples of 16 bits is one and a half stereo frames.
            spec.data = samples_of<std::int16_t>({ 1, 2, 3 });
            test::check(!im::decode_wav(make_wav(spec), audio, "half frame"),
                        "samples that do not divide into whole frames");
        }
        {
            std::vector<std::byte> truncated = make_wav([] {
                WavSpec spec;
                spec.data = samples_of<std::int16_t>({ 1, 2, 3, 4 });
                return spec;
            }());
            truncated.resize(truncated.size() - 4);
            test::check(!im::decode_wav(truncated, audio, "truncated"),
                        "a chunk that says more bytes than the file holds");
        }
    }

    void test_cook_pcm() {
        test::section("Cooking to PCM, and reading it back");

        WavSpec spec;
        spec.channels = 2;
        spec.sample_rate = 44100;
        spec.data = samples_of<std::int16_t>({ 0, 16384, -16384, 32767 });

        im::MemoryWriter writer;
        const as::SoundImport settings{ .stream = false };
        test::check(im::cook_sound_bytes(make_wav(spec), writer, "click.wav.snd", settings,
                                         "click.wav"),
                    "it cooked");

        const auto found = writer.files().find("click.wav.snd");
        test::check(found != writer.files().end(), "it wrote the cooked name it was given");
        if (found == writer.files().end()) {
            return;
        }

        as::SoundView view;
        test::check(as::read_sound(found->second, view, "click.wav.snd"), "it reads back");
        test::check(view.storage == as::SoundStorage::Pcm, "it is stored as PCM");
        test::check(view.channels == 2, "two channels");
        test::check(view.sample_rate == 44100, "44100 Hz");
        test::check(view.frame_count == 2, "two frames of two channels");
        test::check(view.payload.size() == 4 * sizeof(float), "four float samples");

        std::vector<float> samples(4);
        std::memcpy(samples.data(), view.payload.data(), view.payload.size());
        test::check(near(samples[1], 0.5F), "the samples survived the round trip");

        test::check(near(as::sound_seconds(view), 2.0F / 44100.0F), "it reports its length");
    }

    void test_cook_streamed() {
        test::section("Cooking a streamed sound keeps the encoded bytes");

        // Not a WAV at all. The streamed path never looks inside, which is what
        // lets an .ogg through a cooker that carries no decoder.
        const std::vector<std::byte> encoded(1000, std::byte{ 0x5A });

        im::MemoryWriter writer;
        const as::SoundImport settings{ .stream = true };
        test::check(im::cook_sound_bytes(encoded, writer, "music.ogg.snd", settings,
                                         "music.ogg"),
                    "it cooked");

        const auto found = writer.files().find("music.ogg.snd");
        test::check(found != writer.files().end(), "it wrote the cooked file");
        if (found == writer.files().end()) {
            return;
        }

        as::SoundView view;
        test::check(as::read_sound(found->second, view, "music.ogg.snd"), "it reads back");
        test::check(view.storage == as::SoundStorage::Encoded, "it is stored encoded");
        test::check(view.payload.size() == encoded.size(), "the payload is the whole source");
        test::check(std::memcmp(view.payload.data(), encoded.data(), encoded.size()) == 0,
                    "byte for byte, so the decoder sees what the source held");
        test::check(view.channels == 0 && view.sample_rate == 0,
                    "it claims nothing about audio nobody decoded");
        test::check(near(as::sound_seconds(view), 0.0F), "and no length either");
    }

    void test_read_refusals() {
        test::section("A cooked sound that is wrong is refused rather than played");

        WavSpec spec;
        spec.data = samples_of<std::int16_t>({ 0, 16384 });
        im::MemoryWriter writer;
        const as::SoundImport settings{ .stream = false };
        test::check(im::cook_sound_bytes(make_wav(spec), writer, "a.wav.snd", settings, "a.wav"),
                    "a good one cooks");
        const std::vector<std::byte> good = writer.files().at("a.wav.snd");

        as::SoundView view;
        {
            std::vector<std::byte> bytes = good;
            bytes[0] = std::byte{ 0 };
            test::check(!as::read_sound(bytes, view, "bad magic"), "a wrong magic");
        }
        {
            std::vector<std::byte> bytes = good;
            bytes[4] = std::byte{ 99 };
            test::check(!as::read_sound(bytes, view, "bad version"), "a version it cannot read");
        }
        {
            // The frame count says more than the payload holds. A mixer trusts
            // the count, so this one has to be caught here.
            std::vector<std::byte> bytes = good;
            bytes.resize(bytes.size() - sizeof(float));
            test::check(!as::read_sound(bytes, view, "short payload"),
                        "a payload shorter than the header claims");
        }
        {
            const std::vector<std::byte> stub(4, std::byte{ 0 });
            test::check(!as::read_sound(stub, view, "stub"), "a file shorter than the header");
        }
    }

    void test_rule_and_guess() {
        test::section("What the rules say about a sound");

        test::check(im::rule_for("effects/click.wav") == im::Rule::Sound, "a .wav takes the rule");
        test::check(im::rule_for("music/theme.ogg") == im::Rule::Sound, "so does an .ogg");
        test::check(im::rule_for("effects/CLICK.WAV") == im::Rule::Sound,
                    "and it does not care how the file shouts");
        test::check(im::cooked_name("effects/click.wav", im::Rule::Sound, 0) ==
                        std::filesystem::path("effects/click.wav.snd"),
                    "the cooked name keeps the source name and adds .snd");

        test::check(!im::guess_stream("effects/click.wav"), "a WAV is guessed as decoded");
        test::check(im::guess_stream("music/theme.ogg"), "anything else is guessed as streamed");
        test::check(!im::guess_stream("effects/CLICK.WAV"), "the guess lowers the extension");
    }

} // namespace

int main() {
    test_decode_16_bit();
    test_decode_every_width();
    test_decode_shapes();
    test_decode_refusals();
    test_cook_pcm();
    test_cook_streamed();
    test_read_refusals();
    test_rule_and_guess();
    return test::report();
}
