#include "gizmo.h"


// imgui.h first. ImGuizmo.h names ImDrawList and ImGuiContext and includes
// nothing itself, so the other order does not compile.
#include <imgui.h>

#include <ImGuizmo.h>

#include <glm/gtc/type_ptr.hpp>

namespace apps {

    namespace {

        /// Turns the panel's choice into what ImGuizmo asks for.
        [[nodiscard]] ImGuizmo::OPERATION to_operation(engine::editor::GizmoOperation operation) {
            switch (operation) {
            case engine::editor::GizmoOperation::Rotate:
                return ImGuizmo::ROTATE;
            case engine::editor::GizmoOperation::Scale:
                return ImGuizmo::SCALE;
            case engine::editor::GizmoOperation::Translate:
                break;
            }
            return ImGuizmo::TRANSLATE;
        }

        /**
         * The same, for the space.
         *
         * **Scale is always local.** ImGuizmo refuses a world-space scale, and
         * so would the maths: scaling along a world axis that the entity is
         * turned away from is a shear, and a transform holds no shear.
         */
        [[nodiscard]] ImGuizmo::MODE to_mode(const engine::editor::GizmoControls& controls) {
            if (controls.operation == engine::editor::GizmoOperation::Scale) {
                return ImGuizmo::LOCAL;
            }
            return controls.space == engine::editor::GizmoSpace::Local ? ImGuizmo::LOCAL
                                                                       : ImGuizmo::WORLD;
        }

    } // namespace

    engine::Mat4 gizmo_projection(float fov_degrees, float aspect, float near_plane) {
        return glm::perspective(glm::radians(fov_degrees), aspect > 0.0F ? aspect : 1.0F,
                                near_plane, kGizmoFarPlane);
    }

    void begin_gizmo_frame() { ImGuizmo::BeginFrame(); }

    bool gizmo_has_mouse() { return ImGuizmo::IsOver() || ImGuizmo::IsUsing(); }

    bool gizmo_is_dragging() { return ImGuizmo::IsUsing(); }

    bool draw_gizmo(const GizmoDesc& desc, engine::Mat4& world_matrix) {
        // Into the window that is open, so the handles clip to the picture
        // rather than drawing over the panels beside it.
        ImGuizmo::SetDrawlist(ImGui::GetWindowDrawList());
        ImGuizmo::SetRect(desc.x, desc.y, desc.width, desc.height);

        return ImGuizmo::Manipulate(glm::value_ptr(desc.view), glm::value_ptr(desc.projection),
                                    to_operation(desc.controls.operation), to_mode(desc.controls),
                                    glm::value_ptr(world_matrix));
    }

} // namespace apps
