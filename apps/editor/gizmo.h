#pragma once

/**
 * @file
 * @brief The move, turn, and size handles the editor puts on the selected
 *        entity.
 *
 * This is the only file that names ImGuizmo. It lives in `apps/editor/` rather
 * than in `src/editor/` because the editor application is the only program that
 * draws a gizmo, and it is the only target that links the vendored library. See
 * `DESIGN.md` §6 and issue #308.
 *
 * **ImGuizmo wants a projection this engine never builds.** The engine renders
 * with an infinite reverse-Z projection whose Y row is negated for Vulkan clip
 * space, per `math/conventions.h`. ImGuizmo does its own clip to screen step and
 * expects the graphics-API-neutral form: Y up in normalized device coordinates,
 * and a finite far plane. gizmo_projection() builds that one, and it is used for
 * the handles alone. The picture on screen still comes from the engine matrix.
 *
 * The two agree about where a point lands, because a projection's X row is the
 * same either way and the two Y flips cancel.
 */

#include "editor/panels.h"
#include "math/conventions.h"

namespace apps {

    /**
     * @brief The far plane the gizmo projects with, in meters.
     *
     * The engine has no far plane. ImGuizmo needs one, and it is used for the
     * handles alone, so anything past the scene will do. A thousand meters is
     * far outside every scene the engine has, and near enough to keep the
     * projection well conditioned.
     */
    inline constexpr float kGizmoFarPlane = 1000.0F;

    /**
     * @brief Builds the projection the gizmo draws with.
     *
     * @param fov_degrees The vertical field of view of the view it draws over.
     * @param aspect Width divided by height of the picture. A value at or below
     * zero is refused with 1, which is square.
     * @param near_plane How near the near plane is, in meters.
     * @return A projection with Y up and a finite far plane.
     */
    [[nodiscard]] engine::Mat4 gizmo_projection(float fov_degrees, float aspect, float near_plane);

    /// @brief Starts a gizmo frame. Call once, after gfx::imgui_new_frame().
    void begin_gizmo_frame();

    /// @brief What one gizmo needs to draw itself over a picture.
    struct GizmoDesc {
        /// @brief World space to view space, from the camera the picture used.
        engine::Mat4 view{ 1.0F };
        /// @brief View space to clip space, from gizmo_projection().
        engine::Mat4 projection{ 1.0F };
        /// @brief Which handles to show, and which axes they line up with.
        engine::editor::GizmoControls controls;
        float x = 0.0F;      ///< Left edge of the picture, in screen coordinates.
        float y = 0.0F;      ///< Top edge of the picture, in screen coordinates.
        float width = 0.0F;  ///< Width of the picture, in pixels.
        float height = 0.0F; ///< Height of the picture, in pixels.
    };

    /**
     * @brief Draws the handles and reports what the user dragged them to.
     *
     * Call this inside the window the picture is in, so the handles draw into
     * that window's list and clip to it.
     *
     * @param desc The camera, the rectangle, and which handles to show.
     * @param world_matrix The pose to manipulate, in world space. Written when
     * the user drags a handle, and left alone otherwise.
     * @return True when the user moved it this frame.
     */
    [[nodiscard]] bool draw_gizmo(const GizmoDesc& desc, engine::Mat4& world_matrix);

} // namespace apps
