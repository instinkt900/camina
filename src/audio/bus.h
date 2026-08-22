#pragma once

/**
 * @file
 * @brief The groups a voice can play on, so a game can turn one down.
 *
 * A player who wants the music quieter does not want the footsteps quieter with
 * it. A bus is what makes that one number rather than a pass over every sound.
 *
 * **Every bus feeds the master.** So the master volume is the one that means
 * "quieter", and a bus volume means "quieter than the rest".
 *
 * This is its own header because two layers name it. `scene::AudioSource` is a
 * reflected component and carries it as a field, and `audio::Mixer` takes it
 * when a voice starts.
 */

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace engine::audio {

    /**
     * @brief Which group a voice plays on.
     *
     * The set is fixed and short on purpose. A game with a settings screen
     * needs one slider for each of these, and a set a script could add to would
     * give a screen nobody can lay out. Rule 4.6: widen it when the sandbox
     * needs it widened.
     */
    enum class Bus : std::uint8_t {
        /**
         * @brief Everything, and what the other buses feed into.
         *
         * A voice on this one goes straight to the output. That is the right
         * place for a sound that is not music and is not part of the world,
         * such as a menu click.
         */
        Master = 0,

        /// @brief Music. Turned down first, and often all the way.
        Music,

        /// @brief Everything the world makes. Footsteps, doors, a crate landing.
        Effects,
    };

    /// @brief How many buses there are, master included.
    inline constexpr std::size_t kBusCount = 3;

    /**
     * @brief The text form, for reflect/.
     *
     * Without this a scene file would hold `"bus": 1`, because reflect/ writes a
     * plain enum as its underlying number. A person reads and edits that file.
     *
     * @param value The bus to write.
     * @return "Master", "Music", or "Effects".
     */
    [[nodiscard]] std::string to_text(const Bus& value);

    /**
     * @brief Reads the text form, for reflect/.
     * @param text The word to read. The comparison ignores letter case.
     * @param out The bus to fill. It stays as it was when this fails.
     * @return True when @p text names a bus.
     */
    [[nodiscard]] bool from_text(std::string_view text, Bus& out);

} // namespace engine::audio
