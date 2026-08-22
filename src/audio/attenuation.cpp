#include "audio/attenuation.h"

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

    std::string to_text(const Attenuation& value) {
        switch (value) {
        case Attenuation::Linear:
            return "Linear";
        case Attenuation::Exponential:
            return "Exponential";
        case Attenuation::None:
            return "None";
        case Attenuation::Inverse:
            break;
        }
        return "Inverse";
    }

    bool from_text(std::string_view text, Attenuation& out) {
        if (same_word(text, "Inverse")) {
            out = Attenuation::Inverse;
            return true;
        }
        if (same_word(text, "Linear")) {
            out = Attenuation::Linear;
            return true;
        }
        if (same_word(text, "Exponential")) {
            out = Attenuation::Exponential;
            return true;
        }
        if (same_word(text, "None")) {
            out = Attenuation::None;
            return true;
        }
        return false;
    }

} // namespace engine::audio
