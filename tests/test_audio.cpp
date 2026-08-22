// M11.1. The audio device.
//
// Every case here runs on the silent device, because a CI runner has no sound
// card and a developer machine must not open theirs to run a test. That is the
// point of the fallback rather than a limitation of the test: the silent device
// is the path every machine without hardware takes, so it is the path that has
// to be checked.

#include "audio/device.h"

#include "check.h"

#include <chrono>
#include <memory>
#include <thread>

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

} // namespace

int main() {
    test_silent_device_opens();
    test_silent_device_mixes();
    test_rate_is_read_back();
    return test::report();
}
