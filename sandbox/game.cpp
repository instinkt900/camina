#include "sandbox/game.h"

#include "platform/paths.h"

#include "assets/reference.h"
#include "core/log.h"
#include "math/transform.h"
#include "sandbox/components.h"
#include "scene/scene_file.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace sandbox {

    namespace {

        /**
         * Reads every prefab the cooked tree holds, under the source path that
         * produced it.
         *
         * A source cooks into several outputs and only some of them are
         * prefabs. They are told apart by extension rather than by position,
         * because the cooker is free to change the order it writes them in.
         *
         * This asks the source for every prefab rather than reading a list of
         * paths the game holds. A game that named its models in C++ could load
         * only its own content tree, and the large test scene of issue #130 is
         * a tree the sandbox never sees at build time.
         */
        [[nodiscard]] bool add_prefabs(const engine::assets::AssetSource& source,
                                       engine::scene::PrefabLibrary& library) {
            std::size_t added = 0;

            std::vector<engine::assets::AssetRecord> records;
            if (!source.assets_of_kind(engine::assets::kPrefabExtension, records)) {
                ENGINE_LOG_ERROR("The prefabs could not be listed.");
                return false;
            }

            for (const engine::assets::AssetRecord& record : records) {
                std::vector<std::byte> bytes;
                if (!source.read(record.guid, bytes)) {
                    // AssetSource promises a bool and nothing else, so an
                    // implementation can fail without a message of its own.
                    ENGINE_LOG_ERROR("{} is in the project and will not read.", record.name);
                    return false;
                }
                const nlohmann::json document =
                    nlohmann::json::parse(std::string_view{
                                              reinterpret_cast<const char*>(bytes.data()),
                                              bytes.size() },
                                          nullptr, false);
                if (document.is_discarded()) {
                    ENGINE_LOG_ERROR("{} will not parse as a prefab.", record.name);
                    return false;
                }
                if (!library.add(engine::assets::prefab_name(record.source, record.name),
                                 document)) {
                    ENGINE_LOG_ERROR("{} is not a prefab this build can use.", record.name);
                    return false;
                }
                ++added;
            }

            ENGINE_LOG_INFO("The sandbox registered {} prefabs.", added);
            return true;
        }

        /**
         * Reads the opening scene through the asset source.
         *
         * The scene is an asset like any other, so it is found by the source
         * path a person edits and read by identity. A source project resolves
         * its references on the way through, because that is what the document
         * rule does.
         */
        [[nodiscard]] bool load_scene_from(const engine::assets::AssetSource& assets,
                                           engine::scene::World& world,
                                           const engine::scene::ComponentRegistry& registry,
                                           engine::scene::PrefabLibrary& library) {
            std::vector<engine::assets::AssetRecord> found;
            if (!assets.assets_for(kSceneFile, found) || found.empty()) {
                ENGINE_LOG_ERROR("The project holds no {}.", kSceneFile);
                return false;
            }

            std::vector<std::byte> bytes;
            if (!assets.read(found.front().guid, bytes)) {
                ENGINE_LOG_ERROR("{} would not read.", kSceneFile);
                return false;
            }

            const nlohmann::json document = nlohmann::json::parse(
                std::string_view{ reinterpret_cast<const char*>(bytes.data()), bytes.size() },
                nullptr, false);
            if (document.is_discarded()) {
                ENGINE_LOG_ERROR("{} will not parse as a scene.", kSceneFile);
                return false;
            }

            if (!engine::scene::load_scene(document, world, registry, library)) {
                ENGINE_LOG_ERROR("The sandbox could not build the world from {}.", kSceneFile);
                return false;
            }

            ENGINE_LOG_INFO("The sandbox loaded {} entities from {}.", world.size(), kSceneFile);
            return true;
        }

    } // namespace

    std::filesystem::path default_content_directory() {
        return engine::platform::cooked_content_root() / kContentName;
    }

    void bind_actions(engine::platform::Input& input) {
        input.bind(kThrowAction, kThrowKey);
        input.bind(kResetAction, kResetKey);
    }

    void register_components(engine::scene::ComponentRegistry& registry) {
        registry.add<Spin>();
        registry.add<Goal>();
    }

    bool load(const std::filesystem::path& content, const engine::assets::AssetSource* assets,
              engine::scene::World& world, const engine::scene::ComponentRegistry& registry,
              engine::scene::PrefabLibrary& library) {
        // The prefabs go in first. A scene that names one the library does not
        // hold cannot build its entities.
        //
        // They are found by kind and read by identity, so the game does not
        // learn whether they were cooked or imported. A hand-authored prefab is
        // in there too, because the cooker copies what it has no rule for.
        if (assets != nullptr && !add_prefabs(*assets, library)) {
            return false;
        }

        // Through the asset source rather than off the disk. A cooked scene
        // names its assets by identity already. A source scene names them by
        // path, and reading it through the seam runs the document rule, which
        // is what resolves them. So one path serves both kinds of project.
        if (assets != nullptr) {
            return load_scene_from(*assets, world, registry, library);
        }

        const std::filesystem::path scene = content / kSceneFile;
        if (!engine::scene::load_scene_file(scene, world, registry, library)) {
            ENGINE_LOG_ERROR("The sandbox could not read {}.", scene.string());
            return false;
        }

        ENGINE_LOG_INFO("The sandbox loaded {} entities from {}.", world.size(), scene.string());
        return true;
    }

} // namespace sandbox
