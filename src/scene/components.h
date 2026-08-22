#pragma once

/**
 * @file
 * @brief The components the transform hierarchy stores on every entity.
 *
 * `World` in scene/world.h keeps these consistent. Read them freely. Change
 * them only through the World interface, or the hierarchy links and the dirty
 * flags stop agreeing with each other.
 */

#include "audio/attenuation.h"
#include "core/entt.h"
#include "core/guid.h"
#include "math/conventions.h"
#include "reflect/attributes.h"
#include "reflect/reflect.h"

#include <entt/entity/entity.hpp>
#include <entt/entity/fwd.hpp>

#include <cstddef>
#include <string>
#include <tuple>

/// @brief The entity world, the transform hierarchy, and scene files.
namespace engine::scene {

    /**
     * @brief The parent link and the child list, stored inside the entities.
     *
     * The children form a doubly linked list, so attaching and detaching cost
     * the same whether an entity has two children or two thousand. No entity
     * allocates a container for its children.
     *
     * A new child joins at the end, so the list keeps the order the caller
     * attached in. A scene file relies on that: without it, saving and loading
     * would reverse every sibling list.
     *
     * A root has no parent. A leaf has no first child.
     */
    struct Hierarchy {
        /// @brief The parent, or `entt::null` for a root.
        entt::entity parent = entt::null;
        /// @brief The first child, or `entt::null` for a leaf.
        entt::entity first_child = entt::null;
        /// @brief The last child, so attaching at the end costs nothing.
        entt::entity last_child = entt::null;
        /// @brief The next child of the same parent, or `entt::null` at the end.
        entt::entity next_sibling = entt::null;
        /// @brief The previous child of the same parent, or `entt::null` at the start.
        entt::entity prev_sibling = entt::null;
        /// @brief How many direct children this entity has. Grandchildren do not count.
        std::size_t child_count = 0;
    };

    /**
     * @brief The composed world matrix, and whether it is stale.
     *
     * `World::update()` rebuilds a matrix only when `dirty` is true, and clears
     * the flag. A frame that moved nothing therefore rebuilds nothing.
     *
     * A new entity starts dirty, because its matrix has never been composed.
     */
    struct WorldTransform {
        /// @brief Local to world. Valid after the next World::update().
        Mat4 matrix{ 1.0F };
        /// @brief True while the matrix does not match the local transform.
        bool dirty = true;
    };

    /**
     * @brief A label for one entity.
     *
     * Nothing in the engine reads this. A person reads it, in the editor and in
     * a scene file, and that is enough reason to keep it.
     */
    struct Name {
        std::string value; ///< Free text. It does not have to be unique.
    };

    /**
     * @brief The mesh an entity draws, named by identity rather than by path.
     *
     * The GUID is what the cooker gave the mesh, which for a glTF file is the
     * identity `Guid::derive` worked out for that one mesh inside it. A rename
     * inside the content tree therefore changes nothing here.
     *
     * An entity with this and a WorldTransform is what MeshPass draws.
     */
    struct MeshRenderer {
        /// @brief The cooked mesh. A null GUID draws nothing.
        Guid mesh;
    };

    /**
     * @brief A sound this entity plays, and where the sound is.
     *
     * The place is the entity's world transform, so a sound moves by moving the
     * entity that carries it. Nothing here holds a position of its own.
     *
     * **This component exists in a build with no audio.** It is data, and a
     * scene that carries one has to load in every build, or a level authored
     * with sound in it would refuse to open in a build without it. What the
     * option removes is `audio::SceneAudio`, which is the thing that plays it.
     */
    struct AudioSource {
        /// @brief The cooked sound. A null GUID plays nothing.
        Guid sound;

        /// @brief A straight multiplier on the samples. One is as cooked.
        float volume = 1.0F;

        /// @brief Speed, and so pitch with it. One is as cooked.
        float pitch = 1.0F;

        /// @brief Start again at the end, forever.
        bool looping = false;

        /// @brief Start playing as soon as the entity has this component.
        bool play_on_start = false;

        /**
         * @brief Whether the sound has a place in the world.
         *
         * Off is a sound with no direction and no distance, heard the same
         * wherever the listener stands. Music and a menu click are that.
         */
        bool spatial = true;

        /// @brief The curve between @ref min_distance and @ref max_distance.
        audio::Attenuation attenuation = audio::Attenuation::Inverse;

        /**
         * @brief Nearer than this, the sound is at full volume, in meters.
         *
         * A source with a very small value gets loud very fast as a listener
         * walks into it, because the curve is one over the distance.
         */
        float min_distance = 1.0F;

        /// @brief Past this the sound stops getting quieter, in meters.
        float max_distance = 50.0F;

        /// @brief How sharply the curve falls between the two distances.
        float rolloff = 1.0F;
    };

    /**
     * @brief Where the sound is heard from.
     *
     * The pose is the entity's transform, the same way a camera's is. So the
     * ears face the entity's forward, which is its local −Z in world space, and
     * its up is the entity's +Y. See `DESIGN.md` §3.
     *
     * **A scene needs none of these.** With no listener the sound is heard from
     * `scene::primary_camera`, which is what a game wants almost always. A
     * listener entity is for the case where it is not: a camera that watches
     * from far away while the player hears from where they stand.
     */
    struct AudioListener {
        /**
         * @brief Whether the sound is heard from this one.
         *
         * The rule is the camera's rule. The earliest entity that has this set
         * wins, and a scene with several reports the choice.
         */
        bool primary = true;
    };

    /**
     * @brief A light with no position, only a direction. The sun.
     *
     * The direction is the entity's forward, which is its local −Z turned into
     * world space. See DESIGN.md section 3. So a light is aimed by turning it,
     * the same way a camera is, and it needs no direction field of its own.
     *
     * Moving one does nothing, which is correct for a light that is infinitely
     * far away.
     */
    struct DirectionalLight {
        /// @brief The color it emits. Linear, not sRGB.
        Vec3 color{ 1.0F, 1.0F, 1.0F };
        /// @brief How bright it is. The color is multiplied by this.
        float intensity = 1.0F;
    };

    /// @brief How far a new PointLight reaches, in meters.
    inline constexpr float kDefaultLightRange = 10.0F;

    /**
     * @brief A light at a point, shining in every direction.
     *
     * The position is the entity's world position, so a point light is moved by
     * moving its entity. Turning one does nothing.
     */
    struct PointLight {
        /// @brief The color it emits. Linear, not sRGB.
        Vec3 color{ 1.0F, 1.0F, 1.0F };
        /// @brief How bright it is at the source.
        float intensity = 1.0F;
        /**
         * @brief How far it reaches, in meters.
         *
         * The falloff is the inverse square, windowed so it reaches zero at this
         * distance rather than going on forever. Without the window every light
         * would touch every surface, and a scene could not cull one.
         */
        float range = kDefaultLightRange;
    };

    /**
     * @brief The identity of an entity, which outlives the entity itself.
     *
     * An `entt::entity` is a slot number that EnTT hands out again, so nothing
     * that outlives one edit can name an entity with one. An undo entry, a
     * selection, and one day an entity naming another entity all need something
     * that survives the entity being destroyed and built again. This is it.
     *
     * The scene file stores it beside the parent link rather than among the
     * components, because it is what an entity **is** rather than something it
     * carries. So it stays out of the component registry, the same way
     * `Hierarchy` and `WorldTransform` do, and out of the inspector: nobody
     * chooses an identity.
     *
     * `World::create` gives every entity one. A prefab instance gives its
     * members derived ones, so an instance stays one record in the file. See
     * `DESIGN.md` §10 M12.
     */
    struct Id {
        /// @brief The identity. Generated, or read out of a scene file.
        Guid value;
    };

    /// @brief The vertical field of view a new Camera opens with, in degrees.
    inline constexpr float kDefaultFovDegrees = 60.0F;

    /**
     * @brief The camera a scene plays through.
     *
     * The pose is the entity's world transform, so a camera is moved and turned
     * by moving and turning its entity, the same way a light is. It looks down
     * its own -Z and its up is its own +Y, which is what `DESIGN.md` §3 calls
     * forward and up. This component carries what a transform cannot.
     *
     * **A scene owns its camera, and the editor does not.** Until M9.5 the only
     * camera was `editor::ViewSettings` in a file beside the executable, which
     * meant a level could not carry its own viewpoint. The editor flies a free
     * view of its own now, and it never writes to this.
     *
     * A scale on a camera entity reaches the view matrix, because the view is
     * the inverse of the world matrix. That is a strange thing to author and
     * nothing refuses it.
     *
     * @warning Adding this to an entity does not make the game play through it.
     * `primary_camera()` picks one, and `primary` is how a scene with several
     * says which.
     */
    struct Camera {
        /// @brief The vertical field of view, in degrees.
        float fov_degrees = kDefaultFovDegrees;

        /**
         * @brief How near the near plane is, in meters.
         *
         * There is no far plane. The projection is an infinite reverse-Z one,
         * which is what gives the depth range its precision. See
         * `math/conventions.h`.
         */
        float near_plane = kDefaultNearPlane;

        /**
         * @brief A linear scale on the scene before the ACES curve. One is
         *        neutral.
         *
         * It is on the camera because it is what that camera makes of the light
         * it receives, and because a level has to be able to ship its own.
         * `runtime --exposure` still overrides it for one run.
         */
        float exposure = 1.0F;

        /**
         * @brief Whether the game plays through this one.
         *
         * A scene with one camera needs no thought about this. A scene with
         * several plays through the first one that has it set, and reports the
         * choice, because silently picking one of several is how a person ends
         * up debugging the wrong camera.
         */
        bool primary = true;
    };

    /**
     * @brief The cubemap every surface in the scene reflects.
     *
     * The cooker turns an equirectangular `.hdr` panorama into this. One scene
     * has one of them, and the first entity that carries it wins, because the
     * shader binds a single cubemap for the whole frame.
     *
     * It sits on an entity rather than on the world so that a prefab can carry
     * it, and so the inspector edits it the way it edits every other component.
     * The transform of that entity means nothing. An environment has no place.
     *
     * @warning Sampling this directly is not image based lighting. Issue #109
     * replaces the one sample with the split sum form, and only then does a
     * rough metal read correctly.
     */
    struct Environment {
        /// @brief The cooked cubemap. A null GUID gives the grey fallback.
        Guid cubemap;
    };

} // namespace engine::scene

// The numbers in a Range are the description, the same as every other component
// below.
// NOLINTBEGIN(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)
/// @brief Describes Camera for the inspector and for scene files.
template <>
struct engine::reflect::Describe<engine::scene::Camera> {
    /// @brief The type name a scene file stores.
    static constexpr const char* name = "Camera";

    /// @brief Every field, in the order the inspector shows them.
    /// @return A tuple of field descriptors.
    static constexpr auto fields() {
        using engine::scene::Camera;
        return std::make_tuple(
            ENGINE_FIELD(Camera, fov_degrees, Range{ 20.0, 120.0, 0.5 },
                         Tooltip{ "Vertical field of view, in degrees" }),
            ENGINE_FIELD(Camera, near_plane, Range{ 0.01, 10.0, 0.01 },
                         Tooltip{ "Meters. There is no far plane" }),
            ENGINE_FIELD(Camera, exposure, Range{ 0.05, 8.0, 0.01 },
                         Tooltip{ "Scales the scene before the ACES curve. One is neutral" }),
            ENGINE_FIELD(Camera, primary,
                         Tooltip{ "The game plays through the first camera that has this" }));
    }
};
// NOLINTEND(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)

// NOLINTBEGIN(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)
/// @brief Field descriptors for a sound an entity plays.
template <>
struct engine::reflect::Describe<engine::scene::AudioSource> {
    /// @brief The type name a scene file stores.
    static constexpr const char* name = "AudioSource";

    /// @brief Every field, in the order the inspector shows them.
    /// @return A tuple of field descriptors.
    static constexpr auto fields() {
        using engine::scene::AudioSource;
        return std::make_tuple(
            ENGINE_FIELD(AudioSource, sound, engine::reflect::AssetRef{},
                         Tooltip{ "The cooked sound this entity plays." }),
            ENGINE_FIELD(AudioSource, volume, Range{ 0.0, 4.0, 0.01 },
                         Tooltip{ "Multiplies the samples. One is as cooked" }),
            ENGINE_FIELD(AudioSource, pitch, Range{ 0.25, 4.0, 0.01 },
                         Tooltip{ "Speed, and pitch with it. One is as cooked" }),
            ENGINE_FIELD(AudioSource, looping, Tooltip{ "Start again at the end, forever" }),
            ENGINE_FIELD(AudioSource, play_on_start,
                         Tooltip{ "Start as soon as the entity carries this" }),
            ENGINE_FIELD(AudioSource, spatial,
                         Tooltip{ "Off is heard the same wherever the listener stands" }),
            ENGINE_FIELD(AudioSource, attenuation,
                         Tooltip{ "The curve between the two distances" }),
            ENGINE_FIELD(AudioSource, min_distance, Range{ 0.01, 100.0, 0.05 },
                         Tooltip{ "Meters. Nearer than this it is at full volume" }),
            ENGINE_FIELD(AudioSource, max_distance, Range{ 0.1, 1000.0, 0.5 },
                         Tooltip{ "Meters. Past this it stops getting quieter" }),
            ENGINE_FIELD(AudioSource, rolloff, Range{ 0.1, 8.0, 0.05 },
                         Tooltip{ "How sharply it falls between the two distances" }));
    }
};
// NOLINTEND(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)

/// @brief Field descriptors for where the sound is heard from.
template <>
struct engine::reflect::Describe<engine::scene::AudioListener> {
    /// @brief The type name a scene file stores.
    static constexpr const char* name = "AudioListener";

    /// @brief The one field. The pose comes from the transform.
    /// @return A tuple of field descriptors.
    static constexpr auto fields() {
        return std::make_tuple(ENGINE_FIELD(
            engine::scene::AudioListener, primary,
            engine::reflect::Tooltip{ "Sound is heard from the first listener that has this. "
                                      "With no listener at all it is heard from the camera" }));
    }
};

/// @brief Describes Name for the inspector and for scene files.
template <>
struct engine::reflect::Describe<engine::scene::Name> {
    static constexpr const char* name = "Name"; ///< The name a scene file stores.
    /// @brief The one field.
    /// @return A tuple of field descriptors.
    static constexpr auto fields() {
        return std::make_tuple(ENGINE_FIELD(engine::scene::Name, value));
    }
};

/// @brief Field descriptors for the mesh an entity draws.
template <>
struct engine::reflect::Describe<engine::scene::MeshRenderer> {
    static constexpr const char* name = "MeshRenderer"; ///< The name a scene file stores.
    /// @brief The one field.
    /// @return A tuple of field descriptors.
    static constexpr auto fields() {
        return std::make_tuple(
            ENGINE_FIELD(engine::scene::MeshRenderer, mesh,
                         engine::reflect::AssetRef{},
                         engine::reflect::Tooltip{ "The cooked mesh this entity draws." }));
    }
};

/// @brief Field descriptors for a directional light.
template <>
struct engine::reflect::Describe<engine::scene::DirectionalLight> {
    static constexpr const char* name = "DirectionalLight"; ///< The name a scene file stores.
    /// @brief The two fields. The direction comes from the transform.
    /// @return A tuple of field descriptors.
    static constexpr auto fields() {
        return std::make_tuple(
            ENGINE_FIELD(engine::scene::DirectionalLight, color,
                         engine::reflect::Tooltip{ "The color it emits. Linear, not sRGB." }),
            ENGINE_FIELD(engine::scene::DirectionalLight, intensity,
                         engine::reflect::Range{ 0.0, 20.0, 0.01 },
                         engine::reflect::Tooltip{
                             "Aim it by turning the entity. Its direction is local −Z." }));
    }
};

/// @brief Field descriptors for a point light.
template <>
struct engine::reflect::Describe<engine::scene::PointLight> {
    static constexpr const char* name = "PointLight"; ///< The name a scene file stores.
    /// @brief The three fields. The position comes from the transform.
    /// @return A tuple of field descriptors.
    static constexpr auto fields() {
        return std::make_tuple(
            ENGINE_FIELD(engine::scene::PointLight, color,
                         engine::reflect::Tooltip{ "The color it emits. Linear, not sRGB." }),
            ENGINE_FIELD(engine::scene::PointLight, intensity,
                         engine::reflect::Range{ 0.0, 100.0, 0.01 },
                         engine::reflect::Tooltip{ "How bright it is at the source." }),
            ENGINE_FIELD(engine::scene::PointLight, range,
                         engine::reflect::Range{ 0.0, 100.0, 0.05 },
                         engine::reflect::Tooltip{ "How far it reaches, in meters." }));
    }
};

/// @brief Field descriptors for the environment cubemap.
template <>
struct engine::reflect::Describe<engine::scene::Environment> {
    static constexpr const char* name = "Environment"; ///< The name a scene file stores.
    /// @brief The one field.
    /// @return A tuple of field descriptors.
    static constexpr auto fields() {
        return std::make_tuple(ENGINE_FIELD(
            engine::scene::Environment, cubemap,
            engine::reflect::AssetRef{},
            engine::reflect::Tooltip{ "The cooked cubemap every surface reflects." }));
    }
};
