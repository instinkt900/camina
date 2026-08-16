#include "editor/fly_camera.h"

#include "platform/input.h"
#include "platform/window.h"

#include <algorithm>
#include <cmath>

namespace engine::editor {

    namespace {

        /// Straight up would make the forward vector and world up parallel, and
        /// a view has no basis to build from a pair like that.
        constexpr float kLowestPitch = -89.0F;
        constexpr float kHighestPitch = 89.0F;

        /// Keeps the yaw in a readable range as it turns.
        constexpr float kFullTurnDegrees = 360.0F;

        /// Shorter than this is no move at all, and normalizing it would divide
        /// by nearly zero.
        constexpr float kShortestMove = 1.0e-4F;

        /// How much faster shift makes the camera.
        constexpr float kSprintFactor = 4.0F;

    } // namespace

    void bind_fly_actions(platform::Input& input) {
        using platform::Key;
        using platform::MouseButton;

        input.bind(fly_action::kForward, Key::W);
        input.bind(fly_action::kBack, Key::S);
        input.bind(fly_action::kLeft, Key::A);
        input.bind(fly_action::kRight, Key::D);
        input.bind(fly_action::kUp, Key::E);
        input.bind(fly_action::kDown, Key::Q);
        input.bind(fly_action::kSprint, Key::LeftShift);
        input.bind(fly_action::kSprint, Key::RightShift);
        input.bind(fly_action::kLook, MouseButton::Right);
    }

    Vec3 fly_forward(const FlyCamera& camera) {
        const float yaw = glm::radians(camera.yaw);
        const float pitch = glm::radians(camera.pitch);
        const float flat = std::cos(pitch);
        return glm::normalize(
            Vec3{ -flat * std::sin(yaw), std::sin(pitch), -flat * std::cos(yaw) });
    }

    void seed_fly_camera(FlyCamera& camera, const Transform& transform) {
        camera.position = transform.position;

        // The forward is the -Z axis of the rotation, per DESIGN.md section 3.
        const Vec3 forward = glm::normalize(transform.rotation * Vec3{ 0.0F, 0.0F, -1.0F });
        camera.pitch = glm::degrees(std::asin(std::clamp(forward.y, -1.0F, 1.0F)));
        camera.pitch = std::clamp(camera.pitch, kLowestPitch, kHighestPitch);
        camera.yaw = glm::degrees(std::atan2(-forward.x, -forward.z));
    }

    Transform fly_transform(const FlyCamera& camera) {
        // Yaw about up, then pitch about the axis that yaw left pointing right.
        // The other order would roll the camera as it turns.
        const Quat yaw = glm::angleAxis(glm::radians(camera.yaw), world_up);
        const Quat pitch = glm::angleAxis(glm::radians(camera.pitch), Vec3{ 1.0F, 0.0F, 0.0F });
        return Transform{ .position = camera.position, .rotation = yaw * pitch };
    }

    Mat4 fly_clip_from_world(const FlyCamera& camera, float aspect, float fov_degrees,
                             float near_plane) {
        const Mat4 projection = perspective_reverse_z(glm::radians(fov_degrees),
                                                      aspect > 0.0F ? aspect : 1.0F, near_plane);
        const Mat4 view =
            glm::lookAt(camera.position, camera.position + fly_forward(camera), world_up);
        return projection * view;
    }

    bool update_fly_camera(FlyCamera& camera, const platform::Window& window,
                           const platform::Input& input, float delta_seconds) {
        const bool looking = input.held(fly_action::kLook);
        bool moved = false;

        // The pointer stays put while the look is held, so the drag has no edge.
        platform::set_relative_mouse(window, looking);

        if (looking) {
            const Vec2 delta = input.mouse_delta();
            if (delta.x != 0.0F || delta.y != 0.0F) {
                camera.yaw -= delta.x * camera.look_sensitivity;
                camera.pitch -= delta.y * camera.look_sensitivity;
                camera.pitch = std::clamp(camera.pitch, kLowestPitch, kHighestPitch);
                camera.yaw = std::remainder(camera.yaw, kFullTurnDegrees);
                moved = true;
            }
        }

        const Vec3 forward = fly_forward(camera);
        const Vec3 right = glm::normalize(glm::cross(forward, world_up));

        Vec3 wanted{ 0.0F, 0.0F, 0.0F };
        if (input.held(fly_action::kForward)) {
            wanted += forward;
        }
        if (input.held(fly_action::kBack)) {
            wanted -= forward;
        }
        if (input.held(fly_action::kRight)) {
            wanted += right;
        }
        if (input.held(fly_action::kLeft)) {
            wanted -= right;
        }
        if (input.held(fly_action::kUp)) {
            wanted += world_up;
        }
        if (input.held(fly_action::kDown)) {
            wanted -= world_up;
        }

        if (glm::length(wanted) < kShortestMove) {
            return moved;
        }

        const float speed = camera.move_speed * (input.held(fly_action::kSprint) ? kSprintFactor
                                                                                 : 1.0F);
        camera.position += glm::normalize(wanted) * speed * delta_seconds;
        return true;
    }

} // namespace engine::editor
