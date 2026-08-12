#include "scene/step_motion.h"

#include "scene/world.h"

#include <algorithm>

#include <glm/gtx/quaternion.hpp>

namespace engine::scene {

    void StepMotion::begin_step(World& world) {
        for (const auto& [entity, pose] : m_poses) {
            world.set_local(entity, pose.current);
        }
    }

    void StepMotion::record(const World& world, entt::entity entity) {
        const Transform& local = world.local(entity);

        const auto found = m_poses.find(entity);
        if (found == m_poses.end()) {
            // Both halves start at the same pose. A first frame that blended
            // against a default-constructed transform would swing the entity
            // in from the origin.
            m_poses.emplace(entity, Pose{ .previous = local, .current = local });
            return;
        }

        found->second.previous = found->second.current;
        found->second.current = local;
    }

    void StepMotion::interpolate(World& world, float alpha) {
        const float weight = std::clamp(alpha, 0.0F, 1.0F);

        for (const auto& [entity, pose] : m_poses) {
            Transform drawn = pose.current;
            drawn.position = glm::mix(pose.previous.position, pose.current.position, weight);
            // slerp rather than a straight blend, for the reason
            // physics::Simulation::interpolate gives: two quaternions read as
            // two points on a sphere, and mixing them moves at an uneven rate
            // and leaves a length that is not one.
            drawn.rotation = glm::slerp(pose.previous.rotation, pose.current.rotation, weight);
            drawn.scale = glm::mix(pose.previous.scale, pose.current.scale, weight);
            world.set_local(entity, drawn);
        }
    }

    void StepMotion::forget(entt::entity entity) {
        m_poses.erase(entity);
    }

    void StepMotion::clear() {
        m_poses.clear();
    }

    std::size_t StepMotion::tracked() const {
        return m_poses.size();
    }

} // namespace engine::scene
