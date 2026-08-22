#pragma once

/**
 * @file
 * @brief The sound a scene makes, and where it is heard from.
 *
 * `scene::AudioSource` says what an entity plays and `scene::AudioListener` says
 * where it is heard. This is what reads them, starts a voice for each source
 * that asked to play, keeps every voice where its entity is, and tells the mixer
 * where the ears are.
 *
 * **It runs on the fixed step**, from `play::Session`, after the game and the
 * solver have moved everything and before the frame blends the poses. A pose
 * read from the blend instead would move with the frame rate, and a voice would
 * then wobble on a machine whose frame time is uneven.
 *
 * **The listener is a scene entity, never an application's camera.** A person
 * flying the editor's camera around a level therefore does not move the ears.
 * With no listener in the scene the sound comes from `scene::primary_camera`,
 * which is what a game wants almost always.
 */

#include "assets/asset_source.h"
#include "audio/mixer.h"
#include "core/entt.h"
#include "core/guid.h"

#include <entt/entity/entity.hpp>
#include <entt/entity/fwd.hpp>

#include <cstddef>
#include <map>

namespace engine::scene {
    class World;
}

namespace engine::audio {

    /**
     * @brief Plays what a scene says to play, where the scene says it is.
     *
     * @code
     * engine::audio::SceneAudio audio;
     * audio.bind(mixer, content);
     *
     * // Once for each fixed step.
     * audio.update(world);
     * @endcode
     */
    class SceneAudio {
    public:
        /**
         * @brief Says which mixer to play through and where the sounds are.
         *
         * Both must outlive this. Call it once, before the first update().
         *
         * @param mixer The mixer that holds the voices.
         * @param content The assets to read a sound from, cooked or imported.
         */
        void bind(Mixer& mixer, const assets::AssetSource& content);

        /**
         * @brief Starts, moves and drops voices to match the world.
         *
         * A source that asked to play on start gets a voice the first time this
         * sees it. Every voice that is still playing is moved to where its
         * entity is now. A voice whose entity or component has gone is stopped,
         * because a sound outliving the thing making it is heard as a fault.
         *
         * @param world The world to read.
         */
        void update(scene::World& world);

        /// @brief Stops every voice this started. The mixer keeps its sounds.
        void stop_all();

        /// @return How many voices this is holding right now.
        [[nodiscard]] std::size_t playing() const;

    private:
        /**
         * What this knows about one entity that carries a source.
         *
         * The entry outlives the voice on purpose. `play_on_start` means once,
         * and an entry that went away when its voice ended would let the next
         * update start the sound again. That reads as a loop nobody asked for,
         * and only an ear or a voice count would find it.
         */
        struct Entry {
            VoiceId voice = 0; ///< The live voice, or zero once it has ended.
            Guid sound;        ///< What was started, so a changed sound starts again.
        };

        /// Tells the mixer where the ears are. See update().
        void place_listener(scene::World& world);

        /// Stops and forgets every entry whose entity or component has gone.
        void drop_gone(scene::World& world);

        /// Moves a voice that is still playing, or lets go of one that ended.
        void keep(Entry& entry, const Vec3& position);

        Mixer* mixer_ = nullptr;
        const assets::AssetSource* content_ = nullptr;

        /// One entry for each entity carrying a source. An entity plays one sound.
        std::map<entt::entity, Entry> entries_;

        /// The camera the sound falls back to. Held because
        /// `scene::primary_camera` reports its choice on every call.
        entt::entity camera_ = entt::null;
    };

} // namespace engine::audio
