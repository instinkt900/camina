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
#include "audio/scene_audio.h"
#include "math/conventions.h"
#include "scene/components.h"
#include "scene/world.h"
#include "import/sound.h"
#include "import/writer.h"

#include "check.h"

#include <cmath>

#include <glm/gtc/constants.hpp>
#include <glm/gtx/quaternion.hpp>
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

    /// How much sound came out of each ear over a pull.
    struct Ears {
        float left = 0.0F;
        float right = 0.0F;
    };

    /// Pulls frames and sums the size of the samples in each channel.
    Ears pump_ears(au::Mixer& mixer, std::uint32_t frames) {
        std::vector<float> buffer(static_cast<std::size_t>(frames) * kMixerChannels, 0.0F);
        mixer.mix(buffer.data(), frames);
        Ears ears;
        for (std::size_t i = 0; i + 1 < buffer.size(); i += 2) {
            ears.left += std::fabs(buffer[i]);
            ears.right += std::fabs(buffer[i + 1]);
        }
        return ears;
    }

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

    void test_a_placed_sound_is_heard_from_its_side() {
        test::section("A sound to the right is heard on the right");

        // This is the check that a wrong axis cannot survive. The engine is
        // right handed, +Y up, −Z forward, and miniaudio is the same by
        // default. If either half were mirrored, or if X and Z were swapped,
        // the sound would come out of the wrong ear and nothing else in this
        // suite would notice.
        FakeAssets assets;
        const engine::Guid guid = add_sound(assets, 20, flat_wav(kMixerRate, 24000, 0.5F),
                                            false);

        const auto side_of = [&assets, guid](const engine::Vec3& at) {
            au::Mixer mixer;
            (void)mixer.create(kMixerChannels, kMixerRate);
            // At the origin, facing −Z, with +Y up. The engine's own idea of a
            // thing that has not been turned.
            mixer.set_listener({ 0.0F, 0.0F, 0.0F }, { 0.0F, 0.0F, -1.0F }, { 0.0F, 1.0F, 0.0F });
            (void)mixer.play(assets, guid,
                             { .spatial = true, .position = at, .min_distance = 1.0F, .max_distance = 100.0F });
            // Long enough that the gain smoothing miniaudio applies at the
            // start is a small part of what is measured.
            return pump_ears(mixer, 9600);
        };

        const Ears right = side_of({ 10.0F, 0.0F, 0.0F });
        test::check(right.right > right.left * 1.5F, "a sound at +X is louder in the right ear");

        const Ears left = side_of({ -10.0F, 0.0F, 0.0F });
        test::check(left.left > left.right * 1.5F, "a sound at -X is louder in the left ear");

        // Straight ahead is −Z, and it has to be even. A build that had the
        // forward axis backwards would still be even here, which is why the two
        // cases above come first.
        const Ears ahead = side_of({ 0.0F, 0.0F, -10.0F });
        test::check(near(ahead.left, ahead.right, (ahead.left * 0.2F) + 0.001F),
                    "a sound straight ahead is even in both ears");
    }

    void test_distance_makes_a_sound_quieter() {
        test::section("Distance is what makes a placed sound quiet");

        FakeAssets assets;
        const engine::Guid guid = add_sound(assets, 21, flat_wav(kMixerRate, 24000, 0.5F),
                                            false);

        const auto loudness_at = [&assets, guid](float z, au::Attenuation curve) {
            au::Mixer mixer;
            (void)mixer.create(kMixerChannels, kMixerRate);
            mixer.set_listener({ 0.0F, 0.0F, 0.0F }, { 0.0F, 0.0F, -1.0F }, { 0.0F, 1.0F, 0.0F });
            (void)mixer.play(assets, guid,
                             { .spatial = true,
                               .position = { 0.0F, 0.0F, z },
                               .attenuation = curve,
                               .min_distance = 1.0F,
                               .max_distance = 100.0F });
            const Ears ears = pump_ears(mixer, 9600);
            return ears.left + ears.right;
        };

        const float near_by = loudness_at(-1.0F, au::Attenuation::Inverse);
        const float far_off = loudness_at(-40.0F, au::Attenuation::Inverse);
        test::check(near_by > far_off * 4.0F, "the far one is much quieter than the near one");

        // The curve is a field rather than a constant, so turning it off has to
        // change the answer. Without this the two cases above pass on a build
        // that ignores the setting entirely.
        const float flat_near = loudness_at(-1.0F, au::Attenuation::None);
        const float flat_far = loudness_at(-40.0F, au::Attenuation::None);
        test::check(near(flat_near, flat_far, flat_near * 0.05F),
                    "and with no attenuation the distance changes nothing");
    }

    void test_a_flat_sound_ignores_the_listener() {
        test::section("A sound with no place in the world is heard the same everywhere");

        FakeAssets assets;
        const engine::Guid guid = add_sound(assets, 22, flat_wav(kMixerRate, 24000, 0.5F),
                                            false);

        au::Mixer mixer;
        test::check(mixer.create(kMixerChannels, kMixerRate), "the mixer builds");
        mixer.set_listener({ 0.0F, 0.0F, 0.0F }, { 0.0F, 0.0F, -1.0F }, { 0.0F, 1.0F, 0.0F });

        // Not spatial, and far away on the right. Neither fact may change it.
        const au::VoiceId voice = mixer.play(assets, guid,
                                             { .spatial = false,
                                               .position = { 500.0F, 0.0F, 0.0F } });
        test::check(voice != 0, "it plays");

        const Ears ears = pump_ears(mixer, 4800);
        test::check(near(ears.left, ears.right, (ears.left * 0.05F) + 0.001F),
                    "it is even in both ears");
        test::check(ears.left > 100.0F, "and it is at full volume however far away it was put");

        mixer.set_voice_position(voice, { -500.0F, 0.0F, 0.0F });
        const Ears moved = pump_ears(mixer, 4800);
        test::check(near(moved.left, moved.right, (moved.left * 0.05F) + 0.001F),
                    "and moving it changes nothing");
    }

    // ---------------------------------------------------------------------
    // M11.4. What a scene says to play, and where it says it is.

    /// Puts an entity at a place, with no turn.
    entt::entity add_at(engine::scene::World& world, const engine::Vec3& position) {
        const entt::entity entity = world.create();
        world.set_local(entity, engine::Transform{ .position = position });
        return entity;
    }

    /// A source that starts playing by itself, placed in the world.
    entt::entity add_source(engine::scene::World& world, const engine::Vec3& position,
                            engine::Guid sound) {
        const entt::entity entity = add_at(world, position);
        world.registry().emplace<engine::scene::AudioSource>(
            entity, engine::scene::AudioSource{ .sound = sound,
                                                .play_on_start = true,
                                                .spatial = true,
                                                .min_distance = 1.0F,
                                                .max_distance = 100.0F });
        return entity;
    }

    void test_a_scene_plays_what_it_says_to_play() {
        test::section("A source that asks to play gets a voice, and follows its entity");

        FakeAssets assets;
        const engine::Guid guid = add_sound(assets, 30, flat_wav(kMixerRate, 48000, 0.5F),
                                            false);

        au::Mixer mixer;
        test::check(mixer.create(kMixerChannels, kMixerRate), "the mixer builds");
        au::SceneAudio audio;
        audio.bind(mixer, assets);

        engine::scene::World world;
        // The ears at the origin, facing −Z, and the sound on the right.
        const entt::entity ears = add_at(world, { 0.0F, 0.0F, 0.0F });
        world.registry().emplace<engine::scene::AudioListener>(ears);
        const entt::entity source = add_source(world, { 10.0F, 0.0F, 0.0F }, guid);
        world.update();

        audio.update(world);
        test::check(audio.playing() == 1, "the source started a voice");
        test::check(mixer.voices() == 1, "and the mixer holds it");

        const Ears right = pump_ears(mixer, 9600);
        test::check(right.right > right.left * 1.5F, "and it is heard on the right");

        // Move the entity, not the voice. A voice that did not follow its
        // entity would stay on the right.
        world.set_local(source, engine::Transform{ .position = { -10.0F, 0.0F, 0.0F } });
        world.update();
        audio.update(world);

        const Ears left = pump_ears(mixer, 9600);
        test::check(left.left > left.right * 1.5F, "moving the entity moved the sound");

        // A sound that outlives the thing making it is heard as a fault.
        world.destroy(source);
        world.update();
        audio.update(world);
        test::check(audio.playing() == 0, "destroying the entity stopped the voice");
        test::check(mixer.voices() == 0, "and the mixer freed it");
        test::check(pump(mixer, 480) < 0.01F, "and the mixer went quiet");
    }

    void test_the_camera_is_the_fallback_listener() {
        test::section("With no listener, the sound is heard from the camera");

        FakeAssets assets;
        const engine::Guid guid = add_sound(assets, 31, flat_wav(kMixerRate, 48000, 0.5F),
                                            false);

        au::Mixer mixer;
        test::check(mixer.create(kMixerChannels, kMixerRate), "the mixer builds");
        au::SceneAudio audio;
        audio.bind(mixer, assets);

        engine::scene::World world;
        // A camera turned to face +Z, which is behind where it started. A sound
        // at world +X is then on that camera's left rather than its right.
        //
        // So this fails if the fallback reads the origin instead of the camera,
        // and it fails if it reads the position and forgets the turn.
        const entt::entity camera = world.create();
        world.set_local(camera,
                        engine::Transform{ .position = { 0.0F, 0.0F, 0.0F },
                                           .rotation = engine::Quat(
                                               glm::angleAxis(glm::pi<float>(),
                                                              engine::Vec3{ 0.0F, 1.0F, 0.0F })) });
        world.registry().emplace<engine::scene::Camera>(camera,
                                                        engine::scene::Camera{ .primary = true });
        (void)add_source(world, { 10.0F, 0.0F, 0.0F }, guid);
        world.update();

        audio.update(world);
        test::check(audio.playing() == 1, "the source plays");

        const Ears ears = pump_ears(mixer, 9600);
        test::check(ears.left > ears.right * 1.5F,
                    "and it is heard on the turned camera's left, so the camera's pose is what "
                    "was used");
    }

    void test_a_listener_beats_the_camera() {
        test::section("A listener in the scene is used instead of the camera");

        FakeAssets assets;
        const engine::Guid guid = add_sound(assets, 32, flat_wav(kMixerRate, 48000, 0.5F),
                                            false);

        au::Mixer mixer;
        test::check(mixer.create(kMixerChannels, kMixerRate), "the mixer builds");
        au::SceneAudio audio;
        audio.bind(mixer, assets);

        engine::scene::World world;
        // The camera is turned around and the listener is not. The sound at +X
        // is on the camera's left and on the listener's right, so which one is
        // used decides which ear hears it.
        const entt::entity camera = world.create();
        world.set_local(camera,
                        engine::Transform{ .rotation = engine::Quat(
                                               glm::angleAxis(glm::pi<float>(),
                                                              engine::Vec3{ 0.0F, 1.0F, 0.0F })) });
        world.registry().emplace<engine::scene::Camera>(camera,
                                                        engine::scene::Camera{ .primary = true });

        const entt::entity ears = add_at(world, { 0.0F, 0.0F, 0.0F });
        world.registry().emplace<engine::scene::AudioListener>(ears);

        (void)add_source(world, { 10.0F, 0.0F, 0.0F }, guid);
        world.update();
        audio.update(world);

        const Ears heard = pump_ears(mixer, 9600);
        test::check(heard.right > heard.left * 1.5F,
                    "the listener was used rather than the camera");
    }

    void test_a_source_that_does_not_ask_stays_quiet() {
        test::section("A source that did not ask to play does not play");

        FakeAssets assets;
        const engine::Guid guid = add_sound(assets, 33, flat_wav(kMixerRate, 4800, 0.5F), false);

        au::Mixer mixer;
        test::check(mixer.create(kMixerChannels, kMixerRate), "the mixer builds");
        au::SceneAudio audio;
        audio.bind(mixer, assets);

        engine::scene::World world;
        const entt::entity entity = add_at(world, { 0.0F, 0.0F, 0.0F });
        world.registry().emplace<engine::scene::AudioSource>(
            entity, engine::scene::AudioSource{ .sound = guid, .play_on_start = false });
        world.update();

        audio.update(world);
        test::check(audio.playing() == 0, "nothing started");
        test::check(pump(mixer, 480) < 0.01F, "and the mixer is quiet");

        // A source that names nothing is not an error either.
        const entt::entity empty = add_at(world, { 0.0F, 0.0F, 0.0F });
        world.registry().emplace<engine::scene::AudioSource>(
            empty, engine::scene::AudioSource{ .sound = {}, .play_on_start = true });
        world.update();
        audio.update(world);
        test::check(audio.playing() == 0, "and a source that names no sound plays nothing");
    }

    void test_a_one_shot_in_a_scene_is_let_go() {
        test::section("A one-shot in a scene is let go when it ends");

        FakeAssets assets;
        const engine::Guid guid = add_sound(assets, 34, flat_wav(kMixerRate, 480, 0.5F), false);

        au::Mixer mixer;
        test::check(mixer.create(kMixerChannels, kMixerRate), "the mixer builds");
        au::SceneAudio audio;
        audio.bind(mixer, assets);

        engine::scene::World world;
        (void)add_source(world, { 0.0F, 0.0F, -2.0F }, guid);
        world.update();

        audio.update(world);
        test::check(audio.playing() == 1, "it started");

        // Past the end of the sound, then the two updates that free it.
        (void)pump(mixer, 4800);
        mixer.update();
        audio.update(world);
        test::check(mixer.voices() == 0, "the mixer freed the voice");
        test::check(audio.playing() == 0, "and the scene let go of it");

        // It does not start again. A sound plays when something asks it to, and
        // a source that already played is not asking. Getting this wrong makes
        // every one-shot in a level a loop, which is the kind of thing that
        // only an ear finds.
        audio.update(world);
        audio.update(world);
        test::check(audio.playing() == 0, "and it did not start again on its own");
    }

    void test_a_changed_sound_starts_again() {
        test::section("A source told to play another sound plays that one");

        FakeAssets assets;
        const engine::Guid first = add_sound(assets, 35, flat_wav(kMixerRate, 48000, 0.5F),
                                             false);
        const engine::Guid second = add_sound(assets, 36, flat_wav(kMixerRate, 48000, 0.5F),
                                              false);

        au::Mixer mixer;
        test::check(mixer.create(kMixerChannels, kMixerRate), "the mixer builds");
        au::SceneAudio audio;
        audio.bind(mixer, assets);

        engine::scene::World world;
        const entt::entity entity = add_source(world, { 0.0F, 0.0F, -2.0F }, first);
        world.update();
        audio.update(world);
        test::check(audio.playing() == 1, "the first sound plays");
        test::check(mixer.sounds() == 1, "and one sound is loaded");

        // An edit in the editor, or a game writing the field. What is playing
        // is the wrong sound now, so it has to be replaced rather than left.
        world.registry().get<engine::scene::AudioSource>(entity).sound = second;
        audio.update(world);
        test::check(audio.playing() == 1, "the second sound plays");
        test::check(mixer.sounds() == 2, "and both are loaded");
        test::check(mixer.voices() == 1, "with one voice, not two");
    }

    // ---------------------------------------------------------------------
    // M11.5. The buses.

    void test_a_bus_is_built_only_when_it_is_used() {
        test::section("A bus nothing plays on is not built at all");

        FakeAssets assets;
        const engine::Guid guid = add_sound(assets, 40, flat_wav(kMixerRate, 48000, 0.5F),
                                            false);

        au::Mixer mixer;
        test::check(mixer.create(kMixerChannels, kMixerRate), "the mixer builds");
        test::check(mixer.buses() == 0, "a new mixer has built no bus");

        // The master is the output itself rather than a group in front of it,
        // so playing on it builds nothing either.
        test::check(mixer.play(assets, guid, { .bus = au::Bus::Master }) != 0,
                    "a sound plays on the master");
        test::check(mixer.buses() == 0, "and the master is still not a node");

        test::check(mixer.play(assets, guid, { .bus = au::Bus::Music }) != 0,
                    "a sound plays on the music bus");
        test::check(mixer.buses() == 1, "which built that one bus");

        test::check(mixer.play(assets, guid, { .bus = au::Bus::Music }) != 0,
                    "a second sound plays on it");
        test::check(mixer.buses() == 1, "and it was not built again");

        test::check(mixer.play(assets, guid, { .bus = au::Bus::Effects }) != 0,
                    "a sound plays on the effects bus");
        test::check(mixer.buses() == 2, "which built the second one");
    }

    void test_a_bus_volume_turns_its_own_sounds_down() {
        test::section("A bus volume moves what that bus carries, and nothing else");

        FakeAssets assets;
        const engine::Guid guid = add_sound(assets, 41, flat_wav(kMixerRate, 48000, 0.4F),
                                            false);

        au::Mixer mixer;
        test::check(mixer.create(kMixerChannels, kMixerRate), "the mixer builds");

        // One voice on each of the two buses, so the check is that one moved
        // and the other did not. A single voice would pass on a build where the
        // bus volume was applied to everything.
        test::check(mixer.play(assets, guid, { .bus = au::Bus::Music }) != 0, "music plays");
        const float both = pump(mixer, 2400);
        test::check(both > 0.1F, "and it is heard");

        mixer.set_bus(au::Bus::Music, { .volume = 0.0F, .mute = false });
        // Past the ramp, which is what makes the change inaudible as a click.
        (void)pump(mixer, 4800);
        test::check(pump(mixer, 2400) < 0.01F, "turning the music bus down silences it");

        test::check(mixer.play(assets, guid, { .bus = au::Bus::Effects }) != 0, "effects play");
        (void)pump(mixer, 4800);
        test::check(pump(mixer, 2400) > 0.1F,
                    "and the effects bus is untouched by the music setting");
    }

    void test_mute_keeps_the_volume_it_was_at() {
        test::section("Mute silences a bus and keeps its volume");

        FakeAssets assets;
        const engine::Guid guid = add_sound(assets, 42, flat_wav(kMixerRate, 48000, 0.4F),
                                            false);

        au::Mixer mixer;
        test::check(mixer.create(kMixerChannels, kMixerRate), "the mixer builds");
        test::check(mixer.play(assets, guid, { .bus = au::Bus::Effects }) != 0, "a sound plays");

        mixer.set_bus(au::Bus::Effects, { .volume = 0.8F, .mute = true });
        (void)pump(mixer, 4800);
        test::check(pump(mixer, 2400) < 0.01F, "a muted bus is silent");
        test::check(near(mixer.bus_settings(au::Bus::Effects).volume, 0.8F, 0.001F),
                    "and it still holds the volume it was at");

        // A person who mutes and unmutes expects the slider where they left it.
        // A mute written as a volume of zero would come back at zero.
        mixer.set_bus(au::Bus::Effects, { .volume = 0.8F, .mute = false });
        (void)pump(mixer, 4800);
        test::check(pump(mixer, 2400) > 0.1F, "and unmuting brings it back");
    }

    void test_the_master_carries_every_bus() {
        test::section("Every bus feeds the master");

        FakeAssets assets;
        const engine::Guid guid = add_sound(assets, 43, flat_wav(kMixerRate, 48000, 0.4F),
                                            false);

        au::Mixer mixer;
        test::check(mixer.create(kMixerChannels, kMixerRate), "the mixer builds");
        test::check(mixer.play(assets, guid, { .bus = au::Bus::Music }) != 0, "music plays");
        test::check(mixer.play(assets, guid, { .bus = au::Bus::Effects }) != 0, "effects play");
        test::check(mixer.play(assets, guid, { .bus = au::Bus::Master }) != 0,
                    "and something plays on the master itself");

        (void)pump(mixer, 2400);
        test::check(pump(mixer, 2400) > 0.1F, "all three are heard");

        mixer.set_bus(au::Bus::Master, { .volume = 0.0F, .mute = false });
        (void)pump(mixer, 4800);
        test::check(pump(mixer, 2400) < 0.01F, "and the master turns down all of them");
    }

    void test_a_volume_change_is_a_ramp() {
        test::section("A volume change arrives as a ramp, not as a step");

        // A volume applied to the next sample is a discontinuity in the stream,
        // and a discontinuity is a click. It is loud, it is on every change, and
        // it is the thing a person notices first about a settings screen.
        FakeAssets assets;
        const engine::Guid guid = add_sound(assets, 44, flat_wav(kMixerRate, 48000, 0.5F),
                                            false);

        au::Mixer mixer;
        test::check(mixer.create(kMixerChannels, kMixerRate), "the mixer builds");
        test::check(mixer.play(assets, guid, { .bus = au::Bus::Music }) != 0, "a sound plays");
        (void)pump(mixer, 4800);

        mixer.set_bus(au::Bus::Music, { .volume = 0.0F, .mute = false });

        // One millisecond after the change. A step would already be silent
        // here, and the ramp is set to take about fifteen.
        const float just_after = pump(mixer, 48);
        test::check(just_after > 0.05F, "it is still sounding a millisecond later");

        // And it does arrive. A ramp that never finished would be a volume
        // control that does not work.
        (void)pump(mixer, 4800);
        test::check(pump(mixer, 2400) < 0.01F, "and it has arrived by the time it should");
    }

    void test_a_scene_source_plays_on_its_bus() {
        test::section("A source plays on the bus its component names");

        FakeAssets assets;
        const engine::Guid guid = add_sound(assets, 45, flat_wav(kMixerRate, 48000, 0.5F),
                                            false);

        au::Mixer mixer;
        test::check(mixer.create(kMixerChannels, kMixerRate), "the mixer builds");
        au::SceneAudio audio;
        audio.bind(mixer, assets);

        engine::scene::World world;
        const entt::entity entity = add_source(world, { 0.0F, 0.0F, -2.0F }, guid);
        world.registry().get<engine::scene::AudioSource>(entity).bus = au::Bus::Music;
        world.update();

        audio.update(world);
        test::check(audio.playing() == 1, "the source plays");
        test::check(mixer.buses() == 1, "and it built the bus it named");

        // Turning that bus down has to reach it, which is the whole point of
        // the field being on the component.
        mixer.set_bus(au::Bus::Music, { .volume = 0.0F, .mute = false });
        (void)pump(mixer, 4800);
        test::check(pump(mixer, 2400) < 0.01F, "and the bus setting reaches it");
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
    test_a_placed_sound_is_heard_from_its_side();
    test_distance_makes_a_sound_quieter();
    test_a_flat_sound_ignores_the_listener();
    test_a_scene_plays_what_it_says_to_play();
    test_the_camera_is_the_fallback_listener();
    test_a_listener_beats_the_camera();
    test_a_source_that_does_not_ask_stays_quiet();
    test_a_one_shot_in_a_scene_is_let_go();
    test_a_changed_sound_starts_again();
    test_a_bus_is_built_only_when_it_is_used();
    test_a_bus_volume_turns_its_own_sounds_down();
    test_mute_keeps_the_volume_it_was_at();
    test_the_master_carries_every_bus();
    test_a_volume_change_is_a_ramp();
    test_a_scene_source_plays_on_its_bus();
    return test::report();
}
