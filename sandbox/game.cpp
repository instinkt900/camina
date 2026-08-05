#include "sandbox/game.h"

#include "platform/paths.h"

#include "core/log.h"
#include "math/transform.h"
#include "sandbox/components.h"
#include "scene/scene_file.h"

#include <cmath>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace sandbox {

    namespace {

        constexpr float kTwoPi = 6.2831853F;

        /// Below this an axis is too short to normalize without losing meaning.
        constexpr float kShortestAxis = 1.0e-6F;

        /**
         * Reads the prefab the cooker wrote for one model, under a chosen name.
         *
         * A glTF file cooks into several outputs, and only one of them is the
         * prefab. They are told apart by extension rather than by position,
         * because the cooker is free to change the order it writes them in.
         *
         * The first scene of the file is the one that is used. glTF allows
         * several and names a default, and no model the sandbox ships has more
         * than one.
         */
        [[nodiscard]] bool add_model_prefab(const engine::assets::Content& cooked,
                                            std::string_view source, std::string name,
                                            engine::scene::PrefabLibrary& library) {
            const engine::assets::ManifestEntry* entry = cooked.find(source);
            if (entry == nullptr) {
                ENGINE_LOG_ERROR("The cooked content holds no {}. Build the cooker target.",
                                 source);
                return false;
            }

            for (const engine::assets::ManifestOutput& output : entry->outputs) {
                if (!std::string_view{ output.cooked }.ends_with(".prefab")) {
                    continue;
                }
                std::vector<std::byte> bytes;
                if (!cooked.read_bytes(output, bytes)) {
                    return false;
                }
                const nlohmann::json document = nlohmann::json::parse(
                    std::string_view{ reinterpret_cast<const char*>(bytes.data()), bytes.size() },
                    nullptr, false);
                if (document.is_discarded()) {
                    ENGINE_LOG_ERROR("{} will not parse as a prefab.", output.cooked);
                    return false;
                }
                if (!library.add(std::move(name), document)) {
                    ENGINE_LOG_ERROR("{} is not a prefab this build can use.", output.cooked);
                    return false;
                }
                return true;
            }

            ENGINE_LOG_ERROR("{} cooked no prefab. It may hold no glTF scene.", source);
            return false;
        }

    } // namespace

    std::filesystem::path default_content_directory() {
        return engine::platform::cooked_content_root() / kContentName;
    }

    void register_components(engine::scene::ComponentRegistry& registry) {
        registry.add<Spin>();
    }

    bool load(const std::filesystem::path& content, const engine::assets::Content* cooked,
              engine::scene::World& world, const engine::scene::ComponentRegistry& registry,
              engine::scene::PrefabLibrary& library) {
        // The prefabs go in first. A scene that names one the library does not
        // hold cannot build its entities.
        const std::filesystem::path prefab =
            content / (std::string(kCratePrefab) + ".prefab");
        if (!library.add_file(kCratePrefab, prefab)) {
            ENGINE_LOG_ERROR("The sandbox could not read {}.", prefab.string());
            return false;
        }

        // The model prefab comes out of the cooker rather than out of the
        // source tree, so it is found by source path and read by identity. The
        // path is what a person edits, and the identity is what the manifest
        // says that path became.
        if (cooked != nullptr && !add_model_prefab(*cooked, kHelmetSource, kHelmetPrefab,
                                                   library)) {
            return false;
        }
        if (cooked != nullptr &&
            !add_model_prefab(*cooked, kGlassSource, kGlassPrefab, library)) {
            return false;
        }
        if (cooked != nullptr &&
            !add_model_prefab(*cooked, kSpheresSource, kSpheresPrefab, library)) {
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
