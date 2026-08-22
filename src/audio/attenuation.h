#pragma once

/**
 * @file
 * @brief How a sound gets quieter with distance.
 *
 * This is its own header because two layers name it. `scene::AudioSource` is a
 * reflected component and carries it as a field, and `audio::Mixer` takes it
 * when a voice starts. One name for one thing, and neither side includes the
 * other to get it.
 */

#include <cstdint>
#include <string>
#include <string_view>

namespace engine::audio {

    /**
     * @brief The curve between the near distance and the far one.
     *
     * A sound is at full volume inside the near distance and it stops getting
     * quieter past the far one. These say what happens in between.
     */
    enum class Attenuation : std::uint32_t {
        /**
         * @brief The physical one, and the default.
         *
         * Volume falls with one over the distance, so it drops fast near the
         * source and slowly far away. This is how sound behaves, and it is what
         * a listener reads as a real distance.
         */
        Inverse = 0,

        /**
         * @brief A straight line from full to nothing.
         *
         * Not physical, and easier to reason about. A sound reaches exactly
         * nothing at the far distance rather than approaching it, which is
         * useful when a sound must not be heard past a place.
         */
        Linear,

        /// @brief Falls faster than inverse. For a sound that must stay local.
        Exponential,

        /**
         * @brief The same volume everywhere.
         *
         * The sound still moves between the ears, so it is placed without being
         * distant. Music that follows the player is this.
         */
        None,
    };

    /// @brief The largest ::Attenuation value, so a reader can reject the rest.
    inline constexpr std::uint32_t kAttenuationMax = static_cast<std::uint32_t>(
        Attenuation::None);

    /**
     * @brief The text form, for reflect/.
     *
     * Without this a scene file would hold `"attenuation": 1`, because reflect/
     * writes a plain enum as its underlying number. A person reads and edits
     * that file, so it holds `"Linear"` instead.
     *
     * @param value The curve to write.
     * @return "Inverse", "Linear", "Exponential", or "None".
     */
    [[nodiscard]] std::string to_text(const Attenuation& value);

    /**
     * @brief Reads the text form, for reflect/.
     * @param text The word to read. The comparison ignores letter case.
     * @param out The curve to fill. It stays as it was when this fails.
     * @return True when @p text names a curve.
     */
    [[nodiscard]] bool from_text(std::string_view text, Attenuation& out);

} // namespace engine::audio
