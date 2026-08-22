#include "audio/bus.h"

#include <algorithm>

namespace engine::audio {

    namespace {

        /// Matches two words without regard to letter case.
        [[nodiscard]] bool same_word(std::string_view a, std::string_view b) {
            return std::ranges::equal(a, b, [](char left, char right) {
                const auto lower = [](char c) {
                    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
                };
                return lower(left) == lower(right);
            });
        }

    } // namespace

    std::string to_text(const Bus& value) {
        switch (value) {
        case Bus::Music:
            return "Music";
        case Bus::Effects:
            return "Effects";
        case Bus::Master:
            break;
        }
        return "Master";
    }

    bool from_text(std::string_view text, Bus& out) {
        if (same_word(text, "Master")) {
            out = Bus::Master;
            return true;
        }
        if (same_word(text, "Music")) {
            out = Bus::Music;
            return true;
        }
        if (same_word(text, "Effects")) {
            out = Bus::Effects;
            return true;
        }
        return false;
    }

} // namespace engine::audio
