#pragma once

/**
 * @file
 * @brief The audio device, and the silent one that stands in for it.
 *
 * One device owns the mixer thread and the connection to whatever the machine
 * plays sound with. Everything above it, a cache of sounds, a bus, a voice,
 * gets built on this in later increments of M11.
 *
 * **This header names no miniaudio type.** The implementation sits behind the
 * interface, the same way `platform/window.h` keeps SDL out and
 * `physics/world.h` keeps Box3D out. So miniaudio stays a PRIVATE link of
 * `engine_core`, and `scripts/check-miniaudio-containment.sh` keeps it that
 * way. DESIGN.md section 5 rejects an audio plugin ABI, so the interface holds
 * the containment line rather than carrying a second backend.
 *
 * **A machine with no sound card still runs.** A CI runner has none and an
 * offscreen capture expects none, so a device that cannot open hardware opens
 * silent instead of failing. A silent device mixes on its own thread at the
 * same rate and throws the result away, so every caller above it takes one
 * path.
 *
 * @code
 * auto device = engine::audio::create_device({});
 * if (device->backend() == engine::audio::Backend::Silent) {
 *     // Nothing is audible. Everything else works.
 * }
 * @endcode
 */

#include <cstdint>
#include <memory>

/// @brief Audio. The device, the mixer, and the sounds a game plays.
namespace engine::audio {

    /// @brief What a device was able to open.
    enum class Backend : std::uint8_t {
        Hardware, ///< The machine plays sound, through whatever miniaudio chose.
        Silent,   ///< No hardware. The mixer runs and nobody hears it.
    };

    /// @brief What to ask a device for.
    struct DeviceDesc {
        /**
         * @brief Frames each second the mixer runs at.
         *
         * The hardware may answer with another rate. Read sample_rate() back
         * rather than assuming this one.
         */
        std::uint32_t sample_rate = 48000;

        /// @brief Output channels. Two is stereo.
        std::uint32_t channels = 2;

        /**
         * @brief Open silent without asking the machine for hardware.
         *
         * A reproducible run wants this. So does a test, because a machine that
         * does have a sound card would otherwise open it.
         */
        bool force_silent = false;
    };

    /**
     * @brief One output device and the mixer thread behind it.
     *
     * The device is running when create_device() returns it. Destroying it
     * stops the thread and closes whatever it opened.
     */
    class IAudioDevice {
    public:
        IAudioDevice() = default;
        virtual ~IAudioDevice() = default;

        IAudioDevice(const IAudioDevice&) = delete;
        IAudioDevice& operator=(const IAudioDevice&) = delete;
        IAudioDevice(IAudioDevice&&) = delete;
        IAudioDevice& operator=(IAudioDevice&&) = delete;

        /// @return Whether this device reaches hardware or runs silent.
        [[nodiscard]] virtual Backend backend() const = 0;

        /**
         * @brief What the backend is called, for a log line.
         *
         * @return A name miniaudio gave, such as "ALSA", "PulseAudio", or
         *         "Null". Never null and never empty.
         */
        [[nodiscard]] virtual const char* backend_name() const = 0;

        /// @return The rate the mixer actually runs at, in frames each second.
        [[nodiscard]] virtual std::uint32_t sample_rate() const = 0;

        /// @return The number of output channels the mixer actually writes.
        [[nodiscard]] virtual std::uint32_t channels() const = 0;

        /**
         * @brief How many frames the mixer has produced since it started.
         *
         * This is what says the mixer thread is alive, on a machine where
         * nobody can hear the answer. It counts on the silent path exactly as
         * it does on the hardware one.
         *
         * @return A count that only grows. Safe to read from any thread.
         */
        [[nodiscard]] virtual std::uint64_t frames_mixed() const = 0;

        /// @return Whether the mixer thread is running.
        [[nodiscard]] virtual bool is_running() const = 0;

        /**
         * @brief Start the mixer thread again after stop().
         *
         * Starting a running device does nothing and reports success.
         *
         * @return True when the device is running afterwards.
         */
        virtual bool start() = 0;

        /**
         * @brief Stop the mixer thread and keep the device open.
         *
         * Stopping a stopped device does nothing. frames_mixed() holds its
         * value until start() runs again.
         */
        virtual void stop() = 0;
    };

    /**
     * @brief Open a device, and fall back to a silent one.
     *
     * It tries the backends the platform offers, in miniaudio's own order, and
     * never the null backend. It opens the null backend only when that failed,
     * or when the caller asked for silence. So @ref Backend answers honestly
     * rather than reporting hardware for a device nobody can hear.
     *
     * It says which backend it opened, once, and it says why when it fell back.
     *
     * @param desc What to ask for. The defaults are enough for a game.
     * @return A running device. A machine with no sound card gives a silent one
     *         rather than nothing. It gives null only when even the silent
     *         device failed, which is a broken machine rather than a quiet one.
     */
    [[nodiscard]] std::unique_ptr<IAudioDevice> create_device(const DeviceDesc& desc);

} // namespace engine::audio
