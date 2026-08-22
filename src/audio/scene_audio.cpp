#include "audio/scene_audio.h"

#include "core/log.h"
#include "scene/camera.h"
#include "scene/components.h"
#include "scene/world.h"

#include <vector>

namespace engine::audio {

    namespace {

        /// Where an entity is, from the world matrix World::update composed.
        [[nodiscard]] Vec3 position_of(const Mat4& matrix) { return Vec3{ matrix[3] }; }

        /**
         * Which way an entity faces.
         *
         * Column 2 of a world matrix is the entity's +Z in world space, and
         * DESIGN.md section 3 says forward is −Z. So forward is that column
         * negated, the same way a camera reads it.
         */
        [[nodiscard]] Vec3 forward_of(const Mat4& matrix) { return -Vec3{ matrix[2] }; }

        /// Which way is up for an entity, which is column 1.
        [[nodiscard]] Vec3 up_of(const Mat4& matrix) { return Vec3{ matrix[1] }; }

        /**
         * The earliest entity that carries a primary AudioListener.
         *
         * Earliest by entity, not by the order the pool hands them over, for
         * the reason `scene::primary_camera` gives: a view iterates a pool, and
         * that order is neither creation order nor stable across a component
         * being added. A scene file builds entities in the order it lists them,
         * so the smallest entity is the first listener somebody wrote.
         */
        [[nodiscard]] entt::entity earliest_listener(const scene::World& world) {
            entt::entity best = entt::null;
            const auto view = world.registry().view<const scene::AudioListener>();
            for (const entt::entity entity : view) {
                if (!view.get<const scene::AudioListener>(entity).primary) {
                    continue;
                }
                if (best == entt::null || entity < best) {
                    best = entity;
                }
            }
            return best;
        }

        /// The play settings a source component asks for.
        [[nodiscard]] PlayDesc desc_of(const scene::AudioSource& source, const Vec3& position) {
            return PlayDesc{ .volume = source.volume,
                             .pitch = source.pitch,
                             .looping = source.looping,
                             .spatial = source.spatial,
                             .position = position,
                             .attenuation = source.attenuation,
                             .min_distance = source.min_distance,
                             .max_distance = source.max_distance,
                             .rolloff = source.rolloff };
        }

    } // namespace

    void SceneAudio::bind(Mixer& mixer, const assets::AssetSource& content) {
        mixer_ = &mixer;
        content_ = &content;
    }

    void SceneAudio::place_listener(scene::World& world) {
        // Where the sound is heard from. A scene that names no listener is
        // heard from the camera it plays through, which is what a game wants
        // almost always.
        entt::entity ears = earliest_listener(world);
        if (ears == entt::null) {
            // primary_camera reports what it chose on every call, so it is
            // asked only when the answer this holds has stopped being one.
            if (!world.registry().valid(camera_) ||
                !world.registry().all_of<scene::Camera>(camera_)) {
                camera_ = scene::primary_camera(world);
            }
            ears = camera_;
        }
        if (ears == entt::null || !world.registry().valid(ears)) {
            return;
        }

        const Mat4& matrix = world.world_matrix(ears);
        mixer_->set_listener(position_of(matrix), forward_of(matrix), up_of(matrix));
    }

    void SceneAudio::drop_gone(scene::World& world) {
        // A sound that outlives the thing making it is heard as a fault, and a
        // scene reload recycles entity numbers, so this cannot wait for the
        // entity to be reused.
        //
        // The entry goes with it, so an entity built again does start again.
        std::vector<entt::entity> gone;
        for (const auto& [entity, entry] : entries_) {
            if (!world.registry().valid(entity) ||
                !world.registry().all_of<scene::AudioSource>(entity)) {
                mixer_->stop(entry.voice);
                gone.push_back(entity);
            }
        }
        for (const entt::entity entity : gone) {
            entries_.erase(entity);
        }
    }

    void SceneAudio::update(scene::World& world) {
        if (mixer_ == nullptr || content_ == nullptr) {
            return;
        }

        place_listener(world);
        drop_gone(world);

        const auto view =
            world.registry().view<const scene::AudioSource, const scene::WorldTransform>();
        for (const entt::entity entity : view) {
            const auto& source = view.get<const scene::AudioSource>(entity);
            const Vec3 position = position_of(world.world_matrix(entity));

            const auto held = entries_.find(entity);
            if (held != entries_.end()) {
                // The sound this entity names changed, so what is playing is
                // the wrong one. That is an edit in the editor, or a game
                // writing the field. Anything else keeps the entry it has.
                if (held->second.sound == source.sound) {
                    keep(held->second, position);
                    continue;
                }
                mixer_->stop(held->second.voice);
                entries_.erase(held);
            }

            Entry entry{ .voice = 0, .sound = source.sound };
            if (source.play_on_start && source.sound.valid()) {
                entry.voice = mixer_->play(*content_, source.sound, desc_of(source, position));
            }
            entries_.emplace(entity, entry);
        }
    }

    void SceneAudio::keep(Entry& entry, const Vec3& position) {
        // A one-shot the mixer freed leaves an entry with no voice. The entry
        // stays, because play_on_start means once.
        if (!mixer_->playing(entry.voice)) {
            entry.voice = 0;
            return;
        }
        mixer_->set_voice_position(entry.voice, position);
    }

    std::size_t SceneAudio::playing() const {
        std::size_t live = 0;
        for (const auto& [entity, entry] : entries_) {
            if (entry.voice != 0 && mixer_ != nullptr && mixer_->playing(entry.voice)) {
                ++live;
            }
        }
        return live;
    }

    void SceneAudio::stop_all() {
        if (mixer_ != nullptr) {
            for (const auto& [entity, entry] : entries_) {
                mixer_->stop(entry.voice);
            }
        }
        entries_.clear();
    }

} // namespace engine::audio
