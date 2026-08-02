#include "sandbox/game.h"

#include "core/log.h"
#include "math/transform.h"
#include "sandbox/components.h"
#include "scene/scene_file.h"

#include <cmath>

namespace sandbox {

    namespace {

        constexpr float kTwoPi = 6.2831853F;

        /// Below this an axis is too short to normalize without losing meaning.
        constexpr float kShortestAxis = 1.0e-6F;

    } // namespace

    std::filesystem::path default_content_directory() {
        // SANDBOX_CONTENT_DIR is the source directory, and CMake sets it. M4
        // replaces this with a cooked content directory next to the executable.
        return { SANDBOX_CONTENT_DIR };
    }

    void register_components(engine::scene::ComponentRegistry& registry) {
        registry.add<Spin>();
    }

    bool load(const std::filesystem::path& content, engine::scene::World& world,
              const engine::scene::ComponentRegistry& registry,
              engine::scene::PrefabLibrary& library) {
        // The prefabs go in first. A scene that names one the library does not
        // hold cannot build its entities.
        const std::filesystem::path prefab =
            content / (std::string(kCratePrefab) + ".prefab");
        if (!library.add_file(kCratePrefab, prefab)) {
            ENGINE_LOG_ERROR("The sandbox could not read {}.", prefab.string());
            return false;
        }

        const std::filesystem::path scene = content / kSceneFile;
        if (!engine::scene::load_scene_file(scene, world, registry, library)) {
            ENGINE_LOG_ERROR("The sandbox could not read {}.", scene.string());
            return false;
        }

        ENGINE_LOG_INFO("The sandbox loaded {} entities from {}.", world.size(), scene.string());
        return true;
    }

    std::size_t update(engine::scene::World& world, float seconds) {
        std::size_t moved = 0;

        for (const auto [entity, spin] : world.registry().view<const Spin>().each()) {
            if (spin.seconds_per_turn <= 0.0F) {
                continue;
            }

            const float length = glm::length(spin.axis);
            if (length < kShortestAxis) {
                // Normalizing this would divide by nearly zero and fill the
                // rotation with NaN, which then spreads through every child.
                continue;
            }

            engine::Transform local = world.local(entity);
            local.rotation = glm::angleAxis(kTwoPi * seconds / spin.seconds_per_turn,
                                            spin.axis / length);
            world.set_local(entity, local);
            ++moved;
        }

        return moved;
    }

} // namespace sandbox
