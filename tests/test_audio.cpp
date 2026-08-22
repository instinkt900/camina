// M11.1. The audio device.
//
// Every case here runs on the silent device, because a CI runner has no sound
// card and a developer machine must not open theirs to run a test. That is the
// point of the fallback rather than a limitation of the test: the silent device
// is the path every machine without hardware takes, so it is the path that has
// to be checked.

#include "assets/asset_source.h"
#include "assets/sound.h"
#include "audio/device.h"
#include "audio/mixer.h"
#include "core/guid.h"

#include "check.h"

#include <chrono>
#include <cstddef>
#include <cstring>
#include <memory>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

    using engine::audio::Backend;
    using engine::audio::create_device;
    using engine::audio::DeviceDesc;

    // How long to wait for the mixer thread to produce something. The null
    // backend paces itself against the real clock, so this is wall time. It is
    // far longer than the wait needs, because a busy machine may take a while
    // to schedule the thread and a test that fails on a loaded machine is worse
    // than a test that takes a moment.
    constexpr auto kMixTimeout = std::chrono::seconds(2);

    // Waits until the mixer has produced a frame, or the timeout runs out.
    bool wait_for_frames(const engine::audio::IAudioDevice& device) {
        const auto deadline = std::chrono::steady_clock::now() + kMixTimeout;
        while (std::chrono::steady_clock::now() < deadline) {
            if (device.frames_mixed() > 0) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return false;
    }

    void test_silent_device_opens() {
        test::section("A device asked for silence opens silent");

        DeviceDesc desc;
        desc.force_silent = true;
        const auto device = create_device(desc);

        test::check(device != nullptr, "create_device gives a device");
        if (device == nullptr) {
            return;
        }

        test::check(device->backend() == Backend::Silent, "it reports Silent rather than Hardware");
        test::check(device->backend_name() != nullptr, "it names its backend");
        test::check(device->is_running(), "it is running when it comes back");
        test::check(device->sample_rate() > 0, "it reports a sample rate");
        test::check(device->channels() > 0, "it reports a channel count");
    }

    void test_silent_device_mixes() {
        test::section("The silent mixer thread runs");

        DeviceDesc desc;
        desc.force_silent = true;
        const auto device = create_device(desc);
        if (device == nullptr) {
            test::check(false, "create_device gives a device");
            return;
        }

        // This is the case the whole fallback exists for. A silent device that
        // opened but never mixed would look identical from above until the
        // first sound failed to play.
        test::check(wait_for_frames(*device), "it produces frames with nobody listening");

        const auto mixed = device->frames_mixed();
        device->stop();
        test::check(!device->is_running(), "stop() stops it");

        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        test::check(device->frames_mixed() == mixed, "a stopped device mixes nothing more");

        test::check(device->start(), "start() runs it again");
        test::check(device->is_running(), "and it reports itself running");
    }

    void test_rate_is_read_back() {
        test::section("A device answers with the rate it got, not the rate it was asked for");

        DeviceDesc desc;
        desc.force_silent = true;
        desc.sample_rate = 44100;
        desc.channels = 1;
        const auto device = create_device(desc);
        if (device == nullptr) {
            test::check(false, "create_device gives a device");
            return;
        }

        // The null backend takes what it is given. Hardware need not, which is
        // why every caller reads these back rather than keeping what it asked
        // for.
        test::check(device->sample_rate() == 44100, "the silent device took the rate");
        test::check(device->channels() == 1, "the silent device took the channel count");
    }

    /// A cooked PCM sound, built by hand. This needs no cooker and no WAV: the
    /// format is one header and the samples, and both sides read it from
    /// assets/sound.h.
    std::vector<std::byte> cooked_tone(std::uint32_t channels, std::uint32_t rate,
                                       std::uint32_t frames) {
        engine::assets::SoundHeader header;
        header.storage = static_cast<std::uint32_t>(engine::assets::SoundStorage::Pcm);
        header.channels = channels;
        header.sample_rate = rate;
        header.frame_count = frames;
        const std::size_t samples = static_cast<std::size_t>(frames) * channels;
        header.payload_size = static_cast<std::uint32_t>(samples * sizeof(float));

        std::vector<std::byte> out(sizeof(header) + header.payload_size);
        std::memcpy(out.data(), &header, sizeof(header));
        const std::vector<float> values(samples, 0.5F);
        std::memcpy(out.data() + sizeof(header), values.data(), header.payload_size);
        return out;
    }

    /// One sound, handed out by identity. The mixer asks a project for bytes.
    class OneSound final : public engine::assets::AssetSource {
    public:
        OneSound(engine::Guid guid, std::vector<std::byte> bytes)
            : guid_(guid)
            , bytes_(std::move(bytes)) {}

        [[nodiscard]] bool assets_for(std::string_view /*source*/,
                                      std::vector<engine::assets::AssetRecord>& /*out*/) const override {
            return false;
        }

        [[nodiscard]] bool assets_of_kind(
            std::string_view /*suffix*/,
            std::vector<engine::assets::AssetRecord>& /*out*/) const override {
            return true;
        }

        [[nodiscard]] bool read(engine::Guid guid, std::vector<std::byte>& out) const override {
            if (guid != guid_) {
                return false;
            }
            out = bytes_;
            return true;
        }

    private:
        engine::Guid guid_;
        std::vector<std::byte> bytes_;
    };

    void test_the_device_pulls_from_the_mixer() {
        test::section("A device attached to a mixer pulls its frames from it");

        DeviceDesc desc;
        desc.force_silent = true;
        const auto device = create_device(desc);
        if (device == nullptr) {
            test::check(false, "create_device gives a device");
            return;
        }

        engine::audio::Mixer mixer;
        test::check(mixer.create(device->channels(), device->sample_rate()),
                    "the mixer builds for the device's shape");

        const engine::Guid guid{ .high = 1, .low = 2 };
        const OneSound project(guid, cooked_tone(device->channels(), device->sample_rate(),
                                                 device->sample_rate()));

        // This is the one link the mixer's own test cannot cover. That test
        // pumps mix() by hand, which is what makes it deterministic, and so it
        // never asks whether a real device would call it at all.
        device->set_source(&mixer);
        test::check(device->is_running(), "the device is still running after it was attached");

        test::check(mixer.play(project, guid) != 0, "a sound plays");
        test::check(wait_for_frames(*device), "and the device thread is pulling frames");

        // Off the device before either goes. set_source stops the device to
        // change it, so nothing is inside the mixer after this line.
        device->set_source(nullptr);
        mixer.destroy();
        test::check(device->is_running(), "and taking it off leaves the device running");
    }

} // namespace

int main() {
    test_silent_device_opens();
    test_silent_device_mixes();
    test_rate_is_read_back();
    test_the_device_pulls_from_the_mixer();
    return test::report();
}
