// M11.3. Playing a sound.
//
// Every case pumps the mixer by hand. With no device attached the engine only
// advances when something reads it, so a test asks for an exact number of
// frames and reads exactly what came out. No sound card, no thread, and no wall
// clock, which is what makes these repeatable.
//
// The sounds are cooked through the real rule from M11.2, so this drives the
// whole chain: a WAV, the cooker, the cooked format, and the mixer.

#include "assets/asset_source.h"
#include "assets/sound.h"
#include "audio/mixer.h"
#include "import/sound.h"
#include "import/writer.h"

#include "check.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace {

    namespace as = engine::assets;
    namespace au = engine::audio;
    namespace im = engine::import;

    constexpr std::uint32_t kMixerRate = 48000;
    constexpr std::uint32_t kMixerChannels = 2;

    /// The assets a case built, held in memory and handed out by identity.
    class FakeAssets final : public as::AssetSource {
    public:
        void add(engine::Guid guid, std::vector<std::byte> bytes) {
            bytes_.emplace(guid, std::move(bytes));
        }

        [[nodiscard]] bool assets_for(std::string_view /*source*/, std::vector<as::AssetRecord>& /*out*/) const override {
            return false;
        }

        [[nodiscard]] bool assets_of_kind(std::string_view /*suffix*/,
                                          std::vector<as::AssetRecord>& /*out*/) const override {
            return true;
        }

        [[nodiscard]] bool read(engine::Guid guid, std::vector<std::byte>& out) const override {
            const auto found = bytes_.find(guid);
            if (found == bytes_.end()) {
                return false;
            }
            out = found->second;
            return true;
        }

    private:
        std::map<engine::Guid, std::vector<std::byte>> bytes_;
    };

    /// A WAV of a steady value, so what comes out of the mixer is recognizable.
    std::vector<std::byte> flat_wav(std::uint32_t rate, std::uint32_t frames, float value) {
        std::vector<std::byte> out;
        const auto put_u16 = [&out](std::uint16_t v) {
            out.push_back(static_cast<std::byte>(v & 0xFFU));
            out.push_back(static_cast<std::byte>((v >> 8U) & 0xFFU));
        };
        const auto put_u32 = [&out](std::uint32_t v) {
            for (unsigned i = 0; i < 4; ++i) {
                out.push_back(static_cast<std::byte>((v >> (8U * i)) & 0xFFU));
            }
        };
        const auto put_name = [&out](const char* name) {
            for (int i = 0; i < 4; ++i) {
                out.push_back(static_cast<std::byte>(name[i]));
            }
        };

        const std::uint32_t data_bytes = frames * 2; // mono, 16-bit
        put_name("RIFF");
        put_u32(36 + data_bytes);
        put_name("WAVE");
        put_name("fmt ");
        put_u32(16);
        put_u16(1);
        put_u16(1);
        put_u32(rate);
        put_u32(rate * 2);
        put_u16(2);
        put_u16(16);
        put_name("data");
        put_u32(data_bytes);
        const auto sample = static_cast<std::int16_t>(value * 32767.0F);
        for (std::uint32_t i = 0; i < frames; ++i) {
            put_u16(static_cast<std::uint16_t>(sample));
        }
        return out;
    }

    /// Cooks a WAV the way the cooker would, and puts it in the fake project.
    engine::Guid add_sound(FakeAssets& assets, std::uint64_t id,
                           const std::vector<std::byte>& wav, bool stream) {
        im::MemoryWriter writer;
        const as::SoundImport settings{ .stream = stream };
        if (!im::cook_sound_bytes(wav, writer, "sound.snd", settings, "sound")) {
            return {};
        }
        const engine::Guid guid{ .high = id, .low = id };
        assets.add(guid, writer.files().at("sound.snd"));
        return guid;
    }

    /// Pulls frames and reports the loudest sample in them.
    float pump(au::Mixer& mixer, std::uint32_t frames) {
        std::vector<float> buffer(static_cast<std::size_t>(frames) * kMixerChannels, 0.0F);
        mixer.mix(buffer.data(), frames);
        float loudest = 0.0F;
        for (const float sample : buffer) {
            loudest = std::max(loudest, std::fabs(sample));
        }
        return loudest;
    }

    /// Pulls frames until the mixer goes quiet, and says how many that took.
    std::uint32_t frames_until_quiet(au::Mixer& mixer, std::uint32_t block,
                                     std::uint32_t give_up) {
        std::uint32_t pulled = 0;
        while (pulled < give_up) {
            if (pump(mixer, block) < 0.01F) {
                return pulled;
            }
            pulled += block;
        }
        return give_up;
    }

    bool near(float a, float b, float tolerance) { return std::fabs(a - b) < tolerance; }

    void test_a_sound_reaches_the_output() {
        test::section("A sound played is a sound in the buffer");

        FakeAssets assets;
        const engine::Guid guid = add_sound(assets, 1, flat_wav(kMixerRate, 4800, 0.5F), false);

        au::Mixer mixer;
        test::check(mixer.create(kMixerChannels, kMixerRate), "the mixer builds");
        test::check(near(pump(mixer, 128), 0.0F, 0.001F), "and it is silent before anything plays");

        const au::VoiceId voice = mixer.play(assets, guid);
        test::check(voice != 0, "the sound plays");
        test::check(mixer.voices() == 1, "and there is one voice");
        test::check(mixer.sounds() == 1, "and one sound is loaded");

        // The samples the cooker wrote come out of the mixer. A mixer that
        // opened, reported a voice and produced silence would pass every check
        // above this one.
        test::check(near(pump(mixer, 480), 0.5F, 0.02F), "and the samples reach the buffer");
    }

    void test_a_one_shot_frees_itself() {
        test::section("A one-shot ends, and update() is what frees it");

        FakeAssets assets;
        const engine::Guid guid = add_sound(assets, 2, flat_wav(kMixerRate, 480, 0.5F), false);

        au::Mixer mixer;
        test::check(mixer.create(kMixerChannels, kMixerRate), "the mixer builds");
        test::check(mixer.play(assets, guid) != 0, "the sound plays");

        // Ten milliseconds of sound, pulled in one go, and then some.
        (void)pump(mixer, 960);
        test::check(mixer.voices() == 1, "the voice is still there before update()");
        mixer.update();
        test::check(mixer.voices() == 0, "and update() frees it once it has ended");
        test::check(mixer.started() == 1, "started() still counts the one that played");
        test::check(mixer.sounds() == 1, "and the sound stays loaded for the next one");
    }

    void test_a_loop_never_ends() {
        test::section("A looping voice is never freed by update()");

        FakeAssets assets;
        const engine::Guid guid = add_sound(assets, 3, flat_wav(kMixerRate, 240, 0.5F), false);

        au::Mixer mixer;
        test::check(mixer.create(kMixerChannels, kMixerRate), "the mixer builds");
        const au::VoiceId voice = mixer.play(assets, guid, { .looping = true });
        test::check(voice != 0, "the loop plays");

        // Far past the end of the sound.
        test::check(near(pump(mixer, 4800), 0.5F, 0.02F), "it is still making sound");
        mixer.update();
        test::check(mixer.voices() == 1, "and update() leaves it alone");

        mixer.stop(voice);
        test::check(mixer.voices() == 0, "stop() is the only way it ends");
        test::check(near(pump(mixer, 480), 0.0F, 0.001F), "and then the mixer is quiet");
    }

    void test_the_source_rate_is_honored() {
        test::section("A sound cooked at another rate plays for the right length");

        // Half the mixer's rate. It must therefore take about twice as many
        // output frames as it holds source frames.
        //
        // This is the case that catches ma_audio_buffer_ref_init leaving the
        // sample rate at zero, which its own source marks with a TODO. A source
        // of unknown rate is not resampled: it would play at about twice the
        // speed here, and nothing but a length or an ear would say so.
        FakeAssets assets;
        const engine::Guid guid = add_sound(assets, 4, flat_wav(kMixerRate / 2, 2400, 0.5F),
                                            false);

        au::Mixer mixer;
        test::check(mixer.create(kMixerChannels, kMixerRate), "the mixer builds");
        test::check(mixer.play(assets, guid) != 0, "the sound plays");

        const std::uint32_t frames = frames_until_quiet(mixer, 240, 48000);
        // 2400 source frames at half rate is 4800 output frames, and the block
        // size is what the answer is rounded to.
        test::check(frames >= 4560 && frames <= 5040,
                    "it lasted twice its own frame count, so it was resampled");
    }

    void test_a_streamed_sound_plays() {
        test::section("A streamed sound plays without ever being decoded in full");

        // Stored encoded, so the mixer opens a decoder over it rather than a
        // cursor into samples. A WAV is what this test can encode, and the
        // mixer neither knows nor cares what the bytes are.
        FakeAssets assets;
        const engine::Guid guid = add_sound(assets, 5, flat_wav(kMixerRate, 2400, 0.5F), true);

        au::Mixer mixer;
        test::check(mixer.create(kMixerChannels, kMixerRate), "the mixer builds");
        test::check(mixer.play(assets, guid) != 0, "the streamed sound plays");
        test::check(near(pump(mixer, 480), 0.5F, 0.02F), "and its samples reach the buffer");

        const std::uint32_t frames = frames_until_quiet(mixer, 240, 24000);
        test::check(frames >= 1920 && frames <= 2640, "and it lasted the length it holds");
        mixer.update();
        test::check(mixer.voices() == 0, "and it freed its decoder when it ended");
    }

    void test_two_voices_of_one_sound() {
        test::section("Two voices of one sound each keep their own place");

        FakeAssets assets;
        const engine::Guid guid = add_sound(assets, 6, flat_wav(kMixerRate, 2400, 0.4F), false);

        au::Mixer mixer;
        test::check(mixer.create(kMixerChannels, kMixerRate), "the mixer builds");
        test::check(mixer.play(assets, guid) != 0, "the first voice plays");

        // Half way through the first one, start a second.
        (void)pump(mixer, 1200);
        test::check(mixer.play(assets, guid) != 0, "the second voice plays");
        test::check(mixer.voices() == 2, "there are two voices");
        test::check(mixer.sounds() == 1, "and the sound was loaded once for both");

        // Past the end of the first voice, the second is still going, because
        // its cursor is its own. One shared cursor would end both together.
        (void)pump(mixer, 1440);
        mixer.update();
        test::check(mixer.voices() == 1, "the first ended and the second did not");
        test::check(near(pump(mixer, 240), 0.4F, 0.02F), "and the second is still making sound");
    }

    void test_volume_and_pitch() {
        test::section("Volume scales the samples, and pitch changes the length");

        FakeAssets assets;
        const engine::Guid guid = add_sound(assets, 7, flat_wav(kMixerRate, 2400, 0.8F), false);

        {
            au::Mixer mixer;
            test::check(mixer.create(kMixerChannels, kMixerRate), "the mixer builds");
            test::check(mixer.play(assets, guid, { .volume = 0.5F }) != 0, "it plays at half");
            test::check(near(pump(mixer, 480), 0.4F, 0.02F), "and the samples are halved");
        }
        {
            au::Mixer mixer;
            test::check(mixer.create(kMixerChannels, kMixerRate), "the mixer builds again");
            test::check(mixer.play(assets, guid, { .pitch = 2.0F }) != 0, "it plays at twice the speed");
            const std::uint32_t frames = frames_until_quiet(mixer, 120, 24000);
            test::check(frames >= 960 && frames <= 1440, "and it lasted half as long");
        }
    }

    void test_a_missing_sound_is_refused() {
        test::section("A sound that is not there is refused, and reported once");

        FakeAssets assets;
        au::Mixer mixer;
        test::check(mixer.create(kMixerChannels, kMixerRate), "the mixer builds");

        const engine::Guid missing{ .high = 99, .low = 99 };
        test::check(mixer.play(assets, missing) == 0, "playing it gives no voice");
        test::check(mixer.play(assets, missing) == 0, "and asking again still gives none");
        test::check(mixer.voices() == 0 && mixer.sounds() == 0, "nothing was kept");
        test::check(mixer.started() == 0, "and nothing counts as started");

        test::check(mixer.play(assets, engine::Guid{}) == 0, "a null identity plays nothing");
    }

    void test_stop_all() {
        test::section("stop_all() ends every voice and keeps the sounds");

        FakeAssets assets;
        const engine::Guid guid = add_sound(assets, 8, flat_wav(kMixerRate, 4800, 0.5F), false);

        au::Mixer mixer;
        test::check(mixer.create(kMixerChannels, kMixerRate), "the mixer builds");
        test::check(mixer.play(assets, guid) != 0 && mixer.play(assets, guid) != 0,
                    "two voices play");
        mixer.stop_all();
        test::check(mixer.voices() == 0, "stop_all() ends both");
        test::check(mixer.sounds() == 1, "and the sound stays loaded");
        test::check(near(pump(mixer, 480), 0.0F, 0.001F), "and the mixer is quiet");

        mixer.stop(9999);
        test::check(mixer.voices() == 0, "stopping a voice that never existed does nothing");
    }

} // namespace

int main() {
    test_a_sound_reaches_the_output();
    test_a_one_shot_frees_itself();
    test_a_loop_never_ends();
    test_the_source_rate_is_honored();
    test_a_streamed_sound_plays();
    test_two_voices_of_one_sound();
    test_volume_and_pitch();
    test_a_missing_sound_is_refused();
    test_stop_all();
    return test::report();
}
