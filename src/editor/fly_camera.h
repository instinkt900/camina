#pragma once

/**
 * @file
 * @brief The free-fly camera both applications steer with the keyboard.
 *
 * WASD moves, Q and E go down and up, shift is faster, and the right mouse
 * button holds the look. It lived in `apps/runtime/main.cpp` until M9.5, where
 * it flew a struct of view settings. The scene owns its camera now, so this
 * flies whatever the caller points it at.
 *
 * It keeps yaw and pitch of its own rather than reading them back out of a
 * rotation each frame. A quaternion does not answer "which way is up for this
 * camera" the same way twice: reading Euler angles out of one and writing them
 * back drifts, and it gimbals near the poles. So the angles are the state and
 * the rotation is what they produce.
 *
 * This names no ImGui type and no window type. `platform::Window` arrives only
 * so the look can capture the pointer.
 */

#include "math/conventions.h"
#include "math/transform.h"

namespace engine::platform {
    class Input;
    class Window;
} // namespace engine::platform

namespace engine::editor {

    /// @brief The action names a fly camera reads. One name for one thing, per §3.
    namespace fly_action {
        inline constexpr const char* kForward = "move_forward"; ///< Along the line of sight.
        inline constexpr const char* kBack = "move_back";       ///< Against it.
        inline constexpr const char* kLeft = "move_left";       ///< Sideways, level.
        inline constexpr const char* kRight = "move_right";     ///< Sideways, level.
        inline constexpr const char* kUp = "move_up";           ///< Along world up.
        inline constexpr const char* kDown = "move_down";       ///< Against world up.
        inline constexpr const char* kSprint = "sprint";        ///< Multiplies the speed.
        inline constexpr const char* kLook = "look";            ///< Held while turning.
    } // namespace fly_action

    /**
     * @brief Binds the keys a fly camera reads.
     *
     * The camera belongs to the application, so the keys do too. The game's own
     * actions are `sandbox::bind_actions`, and the two sets never meet: this
     * binds on the input the frame clock reads, and the game binds on the one
     * the fixed step reads.
     *
     * @param input The input to bind on.
     */
    void bind_fly_actions(platform::Input& input);

    /**
     * @brief Where a fly camera is and how fast it goes.
     *
     * The pose is in the space the caller works in. A camera entity with no
     * parent makes that world space, and a parented one flies in its parent's
     * space, because that is what its local transform means.
     */
    struct FlyCamera {
        /// @brief Where it stands, in meters.
        Vec3 position{ 0.0F, 0.0F, 0.0F };
        /// @brief Degrees around up. Zero looks down -Z, per DESIGN.md §3.
        float yaw = 0.0F;
        /// @brief Degrees above the horizon. Clamped short of straight up.
        float pitch = 0.0F;
        /// @brief Meters each second. Shift multiplies this.
        float move_speed = 6.0F;
        /// @brief Degrees for each mouse count.
        float look_sensitivity = 0.12F;
    };

    /**
     * @brief Where a fly camera stands when a scene carries no camera.
     *
     * Both applications draw a camera-less scene from here, so a scene with no
     * `scene::Camera` looks the same in the editor and in the runtime. Two
     * copies of these numbers would drift with no build error, and the picture
     * would then depend on which program opened the scene.
     *
     * They are the `ViewSettings` defaults the engine used before M9.5a moved
     * the camera into the scene.
     */
    inline constexpr Vec3 kFallbackPosition{ 0.0F, 2.8F, 6.0F };

    /// @brief The pitch of that fallback view, in degrees below the horizon.
    inline constexpr float kFallbackPitch = -8.0F;

    /// @brief The vertical field of view of that fallback view, in degrees.
    inline constexpr float kFallbackFov = 60.0F;

    /// @brief A fly camera at the fallback pose.
    /// @return The camera to draw a scene with no camera of its own through.
    [[nodiscard]] inline FlyCamera fallback_fly_camera() {
        return FlyCamera{ .position = kFallbackPosition, .pitch = kFallbackPitch };
    }

    /// @brief Which way a fly camera looks.
    /// @param camera The camera to read.
    /// @return A unit vector in the camera's space.
    [[nodiscard]] Vec3 fly_forward(const FlyCamera& camera);

    /**
     * @brief Takes the position and the angles out of a transform.
     *
     * Call this once, when the camera to fly is chosen. A rotation with roll in
     * it loses the roll, because a fly camera has none: it is two angles and a
     * point.
     *
     * @param camera The camera to fill.
     * @param transform The pose to read.
     */
    void seed_fly_camera(FlyCamera& camera, const Transform& transform);

    /// @brief The pose a fly camera is in, for writing back to an entity.
    /// @param camera The camera to read.
    /// @return A transform with unit scale.
    [[nodiscard]] Transform fly_transform(const FlyCamera& camera);

    /**
     * @brief The camera of a fly camera, as clip space from world space.
     *
     * For a view that has no entity behind it: the fallback an application
     * draws with when a scene carries no camera, and the editor's own view. A
     * camera that is an entity goes through `scene::clip_from_world` instead,
     * which reads the pose the scene authored.
     *
     * @param camera The camera to look through.
     * @param aspect Width divided by height of the image. A value at or below
     * zero is refused with 1, which is square.
     * @param fov_degrees The vertical field of view.
     * @param near_plane How near the near plane is, in meters. There is no far
     * plane, per math/conventions.h.
     * @return The matrix the passes take as the camera.
     */
    [[nodiscard]] Mat4 fly_clip_from_world(const FlyCamera& camera, float aspect,
                                           float fov_degrees, float near_plane);

    /**
     * @brief Moves and turns the camera from the keyboard and the mouse.
     *
     * The right mouse button holds the look. While it is down the pointer is
     * captured, so a drag never runs out of screen.
     *
     * ImGui gets first refusal on both devices, so typing in a field does not
     * fly the camera away. That gate is applied in `platform::sample`, so this
     * sees a frame with the taken parts already cleared.
     *
     * @param camera The camera to move.
     * @param window The window to capture the pointer in.
     * @param input The actions to read, on the frame clock.
     * @param delta_seconds How much wall time this frame took.
     * @return True when the camera moved or turned, so a caller knows whether
     * it has anything to write back.
     */
    bool update_fly_camera(FlyCamera& camera, const platform::Window& window,
                           const platform::Input& input, float delta_seconds);

} // namespace engine::editor
