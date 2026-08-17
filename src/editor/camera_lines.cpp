#include "editor/camera_lines.h"

#include "scene/camera.h"
#include "scene/components.h"
#include "scene/world.h"

#include <array>
#include <cmath>

namespace engine::editor {

    namespace {

        /// How much of the far end the up bar rises above, as a fraction of the
        /// half height. A quarter is tall enough to read and short enough to
        /// stay out of the picture.
        constexpr float kUpBarRise = 0.25F;

        void add_line(std::vector<physics::DebugLine>& out, const Vec3& from, const Vec3& to) {
            out.push_back(physics::DebugLine{ .from = from, .to = to, .color = kCameraLinesColor });
        }

    } // namespace

    void camera_lines(const scene::World& world, entt::entity camera, float aspect,
                      std::vector<physics::DebugLine>& out) {
        out.clear();

        const scene::Camera& settings = world.registry().get<const scene::Camera>(camera);
        const Mat4& matrix = world.world_matrix(camera);

        // The basis of the camera, from its world matrix. A camera looks down
        // its own -Z with its own +Y up, per DESIGN.md section 3, so this needs
        // no convention of its own.
        const Vec3 apex{ matrix[3] };
        const Vec3 right{ glm::normalize(Vec3{ matrix[0] }) };
        const Vec3 up{ glm::normalize(Vec3{ matrix[1] }) };
        const Vec3 forward{ glm::normalize(-Vec3{ matrix[2] }) };

        // The far end. The field of view is the vertical one, so the height is
        // exact and the width is the aspect the caller asked for.
        const float half_height =
            kCameraLinesLength * std::tan(glm::radians(settings.fov_degrees) * 0.5F);
        const float half_width = half_height * (aspect > 0.0F ? aspect : 1.0F);
        const Vec3 centre = apex + (forward * kCameraLinesLength);

        const std::array<Vec3, 4> corners{
            centre + (up * half_height) - (right * half_width),
            centre + (up * half_height) + (right * half_width),
            centre - (up * half_height) + (right * half_width),
            centre - (up * half_height) - (right * half_width),
        };

        // Four edges from the camera, and the rectangle they end on.
        for (std::size_t i = 0; i < corners.size(); ++i) {
            add_line(out, apex, corners.at(i));
            add_line(out, corners.at(i), corners.at((i + 1) % corners.size()));
        }

        // A bar over the top, so a person can tell which way up the camera is.
        // A pyramid alone is the same shape rolled by any angle.
        const Vec3 peak = centre + (up * half_height * (1.0F + kUpBarRise));
        add_line(out, corners.at(0), peak);
        add_line(out, corners.at(1), peak);
    }

} // namespace engine::editor
