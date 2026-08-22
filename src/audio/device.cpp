#include "audio/device.h"

#include "audio/miniaudio_config.h"
#include "core/log.h"

#include <miniaudio.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>

namespace engine::audio {

    namespace {

        // The mixer format. Everything above this file works in float, and
        // miniaudio converts to whatever the hardware takes.
        constexpr ma_format kMixFormat = ma_format_f32;

        /**
         * One output device, hardware or silent.
         *
         * The two cases differ by the backend list the context was opened with
         * and by nothing else. A silent device runs the same callback on its own
         * thread, at the same rate, and throws the result away. So a caller
         * above this file cannot take a different path by accident.
         */
        class MiniaudioDevice final : public IAudioDevice {
        public:
            MiniaudioDevice() = default;

            ~MiniaudioDevice() override {
                // The callback reads this object, so the device stops before any
                // member of it goes away.
                if (device_open_) {
                    ma_device_uninit(&device_);
                }
                if (context_open_) {
                    ma_context_uninit(&context_);
                }
            }

            MiniaudioDevice(const MiniaudioDevice&) = delete;
            MiniaudioDevice& operator=(const MiniaudioDevice&) = delete;
            MiniaudioDevice(MiniaudioDevice&&) = delete;
            MiniaudioDevice& operator=(MiniaudioDevice&&) = delete;

            /**
             * Open a context and a device on one set of backends.
             *
             * @param backends The backends to try, in order.
             * @param count How many of them.
             * @param desc What the caller asked for.
             * @return MA_SUCCESS, or what failed first.
             */
            ma_result open(const ma_backend* backends, std::size_t count, const DeviceDesc& desc) {
                ma_result result = ma_context_init(backends, static_cast<ma_uint32>(count), nullptr, &context_);
                if (result != MA_SUCCESS) {
                    return result;
                }
                context_open_ = true;

                ma_device_config config = ma_device_config_init(ma_device_type_playback);
                config.playback.format = kMixFormat;
                config.playback.channels = desc.channels;
                config.sampleRate = desc.sample_rate;
                config.dataCallback = &MiniaudioDevice::mix;
                config.pUserData = this;

                result = ma_device_init(&context_, &config, &device_);
                if (result != MA_SUCCESS) {
                    ma_context_uninit(&context_);
                    context_open_ = false;
                    return result;
                }
                device_open_ = true;

                result = ma_device_start(&device_);
                if (result != MA_SUCCESS) {
                    ma_device_uninit(&device_);
                    device_open_ = false;
                    ma_context_uninit(&context_);
                    context_open_ = false;
                    return result;
                }

                silent_ = context_.backend == ma_backend_null;
                return MA_SUCCESS;
            }

            [[nodiscard]] Backend backend() const override {
                return silent_ ? Backend::Silent : Backend::Hardware;
            }

            [[nodiscard]] const char* backend_name() const override {
                return ma_get_backend_name(context_.backend);
            }

            [[nodiscard]] std::uint32_t sample_rate() const override { return device_.sampleRate; }

            [[nodiscard]] std::uint32_t channels() const override { return device_.playback.channels; }

            [[nodiscard]] std::uint64_t frames_mixed() const override {
                return frames_mixed_.load(std::memory_order_relaxed);
            }

            [[nodiscard]] bool is_running() const override {
                return device_open_ && ma_device_is_started(&device_) == MA_TRUE;
            }

            bool start() override {
                if (!device_open_) {
                    return false;
                }
                if (ma_device_is_started(&device_) == MA_TRUE) {
                    return true;
                }
                return ma_device_start(&device_) == MA_SUCCESS;
            }

            void stop() override {
                if (device_open_ && ma_device_is_started(&device_) == MA_TRUE) {
                    ma_device_stop(&device_);
                }
            }

        private:
            /**
             * The mixer callback, on the device thread.
             *
             * It writes silence, because M11.1 has nothing to play yet. What it
             * does carry is the frame count, which is how a test says the thread
             * is alive on a machine where nobody can hear it.
             */
            static void mix(ma_device* device, void* output, const void* /*input*/, ma_uint32 frame_count) {
                auto* self = static_cast<MiniaudioDevice*>(device->pUserData);
                ma_silence_pcm_frames(output, frame_count, device->playback.format, device->playback.channels);
                self->frames_mixed_.fetch_add(frame_count, std::memory_order_relaxed);
            }

            ma_context context_{};
            ma_device device_{};
            std::atomic<std::uint64_t> frames_mixed_{ 0 };
            bool context_open_ = false;
            bool device_open_ = false;
            bool silent_ = false;
        };

        /**
         * The backends this platform offers, with the null backend taken out.
         *
         * miniaudio puts the null backend last in its own default order, so a
         * machine with no sound card would open it and report success. Then
         * nothing could tell a silent device from a working one. The engine asks
         * for the null backend explicitly instead, and only after the hardware
         * list failed.
         *
         * @param backends Where to write them.
         * @return How many were written.
         */
        std::size_t hardware_backends(std::array<ma_backend, MA_BACKEND_COUNT>& backends) {
            std::array<ma_backend, MA_BACKEND_COUNT> enabled{};
            std::size_t enabled_count = 0;
            if (ma_get_enabled_backends(enabled.data(), enabled.size(), &enabled_count) != MA_SUCCESS) {
                return 0;
            }

            std::size_t count = 0;
            for (std::size_t i = 0; i < enabled_count; ++i) {
                if (enabled.at(i) != ma_backend_null) {
                    backends.at(count) = enabled.at(i);
                    ++count;
                }
            }
            return count;
        }

    } // namespace

    std::unique_ptr<IAudioDevice> create_device(const DeviceDesc& desc) {
        if (!desc.force_silent) {
            auto device = std::make_unique<MiniaudioDevice>();
            std::array<ma_backend, MA_BACKEND_COUNT> backends{};
            const std::size_t count = hardware_backends(backends);

            const ma_result result = count == 0 ? MA_NO_BACKEND : device->open(backends.data(), count, desc);
            if (result == MA_SUCCESS) {
                ENGINE_LOG_INFO("Audio: {} at {} Hz, {} channels", device->backend_name(), device->sample_rate(),
                                device->channels());
                return device;
            }

            ENGINE_LOG_WARN("Audio: no device opened ({}). Running silent, so nothing will be audible.",
                            ma_result_description(result));
        }

        auto silent = std::make_unique<MiniaudioDevice>();
        const ma_backend null_backend = ma_backend_null;
        const ma_result result = silent->open(&null_backend, 1, desc);
        if (result != MA_SUCCESS) {
            ENGINE_LOG_ERROR("Audio: the silent device failed to open ({}). There is no audio at all.",
                             ma_result_description(result));
            return nullptr;
        }

        if (desc.force_silent) {
            ENGINE_LOG_INFO("Audio: silent by request, at {} Hz, {} channels", silent->sample_rate(),
                            silent->channels());
        }
        return silent;
    }

} // namespace engine::audio
