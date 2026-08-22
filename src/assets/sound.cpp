#include "assets/sound.h"

#include "core/log.h"

#include <cstring>

namespace engine::assets {

    float sound_seconds(const SoundView& view) {
        if (view.storage != SoundStorage::Pcm || view.sample_rate == 0) {
            return 0.0F;
        }
        return static_cast<float>(view.frame_count) / static_cast<float>(view.sample_rate);
    }

    bool read_sound(std::span<const std::byte> bytes, SoundView& out, std::string_view where) {
        if (bytes.size() < sizeof(SoundHeader)) {
            ENGINE_LOG_ERROR("{}: it is {} bytes, which is shorter than a sound header.", where,
                             bytes.size());
            return false;
        }

        SoundHeader header{};
        std::memcpy(&header, bytes.data(), sizeof(header));

        if (header.magic != kSoundMagic) {
            ENGINE_LOG_ERROR("{}: it does not start with the cooked sound magic.", where);
            return false;
        }
        if (header.version != kSoundVersion) {
            ENGINE_LOG_ERROR("{}: it is version {}, and this build reads version {}.", where,
                             header.version, kSoundVersion);
            return false;
        }
        if (header.storage > kSoundStorageMax) {
            ENGINE_LOG_ERROR("{}: it names storage {}, which this build does not know.", where,
                             header.storage);
            return false;
        }

        const std::size_t payload = bytes.size() - sizeof(SoundHeader);
        if (header.payload_size != payload) {
            ENGINE_LOG_ERROR("{}: the header says {} bytes of audio and the file holds {}.",
                             where, header.payload_size, payload);
            return false;
        }

        const auto storage = static_cast<SoundStorage>(header.storage);
        if (storage == SoundStorage::Pcm) {
            if (header.channels == 0 || header.sample_rate == 0) {
                ENGINE_LOG_ERROR("{}: PCM with {} channels at {} Hz is not playable.", where,
                                 header.channels, header.sample_rate);
                return false;
            }
            // The frame count and the payload have to agree. A mixer trusts the
            // frame count and reads the payload, so a disagreement is a read
            // past the end rather than a quiet sound.
            const std::size_t expected = static_cast<std::size_t>(header.frame_count) *
                                         header.channels * kPcmSampleBytes;
            if (expected != payload) {
                ENGINE_LOG_ERROR("{}: {} frames of {} channels need {} bytes, and the file "
                                 "holds {}.",
                                 where, header.frame_count, header.channels, expected, payload);
                return false;
            }
        }

        out.storage = storage;
        out.channels = header.channels;
        out.sample_rate = header.sample_rate;
        out.frame_count = header.frame_count;
        out.payload = bytes.subspan(sizeof(SoundHeader));
        return true;
    }

} // namespace engine::assets
