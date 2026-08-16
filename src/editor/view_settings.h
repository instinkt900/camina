#pragma once

/**
 * @file
 * @brief How an application looks at the world, and the file it saves that in.
 *
 * This is the M2 demonstration, and it stayed in the runtime until M9.2 because
 * there was nowhere else to put it. Both applications need it now: the runtime
 * flies the camera through the scene, and the editor viewport uses the same
 * camera and the same exposure.
 *
 * Nothing here names a field twice. The inspector builds its widgets from the
 * descriptors below, and the serializer writes the same fields to the file.
 * Add a field to the struct and to its description, and it appears in the panel
 * and in the file.
 */

#include "core/timestep.h"
#include "math/conventions.h"
#include "reflect/attributes.h"
#include "reflect/reflect.h"

#include <cstdint>
#include <string>
#include <tuple>

/// @brief The panels and the view state both applications share. See DESIGN.md section 6.
namespace engine::editor {

    /// @brief Where the view settings are saved, next to the working directory.
    inline constexpr const char* kViewSettingsFile = "view.json";

    /**
     * @brief The camera, the exposure, and the simulation rate of one view.
     *
     * It holds how a program looks at the world and never the world itself.
     * The entities live in a scene::World that the game loads.
     *
     * @warning An offscreen run with no window reads this as well, so nothing
     * here may depend on ImGui or on a window existing.
     */
    struct ViewSettings {
        std::string name = "sandbox view";      ///< Saved with the settings.
        Vec3 clear_color{ 0.25F, 0.25F, 0.3F }; ///< Linear, not sRGB.

        /// @brief Where the camera stands. Saved, so a run opens where the last one stopped.
        Vec3 camera_position{ 0.0F, 2.8F, 6.0F };
        /// @brief Degrees around +Y. Zero looks down -Z, which is forward per DESIGN.md section 3.
        float camera_yaw = 0.0F;
        /// @brief Degrees up from the horizon. Clamped, so the camera never rolls over the top.
        float camera_pitch = -8.0F;

        float fov_degrees = 60.0F;      ///< Vertical field of view.
        float move_speed = 6.0F;        ///< Meters each second.
        float look_sensitivity = 0.12F; ///< Degrees for each mouse count.

        /**
         * @brief A linear scale on the scene before the ACES curve. One is neutral.
         *
         * It lives here rather than on the camera because it is a property of
         * the view and this struct is what the view already is. A
         * `scene::Camera` field is where it belongs once a scene carries more
         * than one camera, which nothing does yet.
         */
        float exposure = 1.0F;

        /**
         * @brief How many physics steps make one second.
         *
         * The simulation advances at this rate whatever the frame rate is, so
         * the same scene behaves the same way on two machines. See
         * engine::FixedTimestep and DESIGN.md section 9.
         */
        float physics_hz = kDefaultStepHz;

        /**
         * @brief How many steps one frame runs before it drops the time it owes.
         *
         * A frame slower than one step leaves time owed, and paying all of it
         * back makes the next frame slower still. This is the ceiling that
         * stops that. The frame report says how much time it discarded.
         */
        std::uint32_t max_physics_steps = kDefaultMaxStepsPerFrame;

        /**
         * @brief Whether to draw the physics wireframe over the frame.
         *
         * Off by default. A collider is invisible, and this is what makes one
         * that does not match its mesh a five second answer.
         */
        bool physics_debug = false;

        std::uint64_t frames_drawn = 0; ///< Read only, and never saved.
    };

    /**
     * @brief Which way the camera looks, from its yaw and its pitch.
     *
     * A yaw of zero looks down -Z, which DESIGN.md section 3 calls forward.
     * Pitch lifts from the horizon.
     *
     * @param settings The view to read.
     * @return A unit vector in world space.
     */
    [[nodiscard]] inline Vec3 camera_forward(const ViewSettings& settings) {
        const float yaw = glm::radians(settings.camera_yaw);
        const float pitch = glm::radians(settings.camera_pitch);
        const float flat = std::cos(pitch);
        return glm::normalize(
            Vec3{ -flat * std::sin(yaw), std::sin(pitch), -flat * std::cos(yaw) });
    }

    /**
     * @brief The camera of this view, as clip space from world space.
     *
     * A model matrix is not part of this: each entity supplies its own, and
     * scene::World has already composed it. Reverse-Z means the near plane maps
     * to depth 1, and perspective_reverse_z already negates the Y row for
     * Vulkan clip space.
     *
     * It takes the aspect ratio rather than a size, because the two
     * applications measure that from different things. The runtime uses the
     * swapchain and the editor uses the panel the scene draws into.
     *
     * @param settings The view to read.
     * @param aspect Width divided by height of the image the scene renders
     * into. A value at or below zero is refused with 1, which is square.
     * @return The matrix the passes take as the camera.
     */
    [[nodiscard]] inline Mat4 view_projection(const ViewSettings& settings, float aspect) {
        const Mat4 projection = perspective_reverse_z(glm::radians(settings.fov_degrees),
                                                      aspect > 0.0F ? aspect : 1.0F,
                                                      kDefaultNearPlane);
        const Mat4 view = glm::lookAt(settings.camera_position,
                                      settings.camera_position + camera_forward(settings),
                                      world_up);
        return projection * view;
    }

} // namespace engine::editor

// The numbers in a Range are the description. Naming each slider bound would
// give twelve constants that each appear once, and would push the number away
// from the field it belongs to.
// NOLINTBEGIN(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)
/// @brief Describes engine::editor::ViewSettings for the inspector and for JSON.
template <>
struct engine::reflect::Describe<engine::editor::ViewSettings> {
    /// @brief The type name the file stores.
    static constexpr const char* name = "ViewSettings";

    /// @brief Every field, in the order the panel shows them.
    /// @return A tuple of field descriptors.
    static constexpr auto fields() {
        using engine::editor::ViewSettings;
        return std::make_tuple(
            ENGINE_FIELD(ViewSettings, name, Tooltip{ "Saved with the view settings" }),
            ENGINE_FIELD(ViewSettings, clear_color, Range{ 0.0, 1.0, 0.01 },
                         Tooltip{ "Linear, not sRGB" }),
            ENGINE_FIELD(ViewSettings, camera_position, Range{ -100.0, 100.0, 0.05 },
                         Category{ "Camera" },
                         Tooltip{ "Meters. Hold the right mouse button and use WASD" }),
            ENGINE_FIELD(ViewSettings, camera_yaw, Range{ -180.0, 180.0, 0.5 },
                         Category{ "Camera" }, Tooltip{ "Degrees around up. Zero looks down -Z" }),
            ENGINE_FIELD(ViewSettings, camera_pitch, Range{ -89.0, 89.0, 0.5 },
                         Category{ "Camera" }, Tooltip{ "Degrees above the horizon" }),
            ENGINE_FIELD(ViewSettings, fov_degrees, Range{ 20.0, 120.0, 0.5 },
                         Category{ "Camera" }),
            ENGINE_FIELD(ViewSettings, move_speed, Range{ 0.5, 40.0, 0.1 }, Category{ "Camera" },
                         Tooltip{ "Meters each second. Hold shift to go faster" }),
            ENGINE_FIELD(ViewSettings, look_sensitivity, Range{ 0.01, 1.0, 0.01 },
                         Category{ "Camera" }, Tooltip{ "Degrees for each mouse count" }),
            ENGINE_FIELD(ViewSettings, exposure, Range{ 0.05, 8.0, 0.01 },
                         Tooltip{ "Scales the scene before the ACES curve. One is neutral" }),
            // Live, so a person can drag the rate down and watch the blend hold
            // the motion together. That is the fastest way to see what the
            // interpolation is for.
            ENGINE_FIELD(ViewSettings, physics_hz, Range{ 1.0, 240.0, 1.0 },
                         Category{ "Physics" },
                         Tooltip{ "Simulation steps each second, whatever the frame rate" }),
            ENGINE_FIELD(ViewSettings, max_physics_steps, Range{ 1.0, 32.0, 1.0 },
                         Category{ "Physics" },
                         Tooltip{ "Steps one frame runs before it drops the time it owes" }),
            ENGINE_FIELD(ViewSettings, physics_debug, Category{ "Physics" },
                         Tooltip{ "Draw every collider as a wireframe, from what Box3D reports" }),
            // ReadOnly keeps the editor from changing it. Transient keeps it out
            // of the file. The two attributes are read by different consumers,
            // and neither consumer knows about the other.
            ENGINE_FIELD(ViewSettings, frames_drawn, ReadOnly{}, Transient{},
                         Category{ "Debug" }));
    }
};
// NOLINTEND(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)
