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
         * This walks the whole manifest rather than a list of paths the game
         * holds. A game that named its models in C++ could load only its own
         * content tree, and the large test scene of issue #130 is a tree the
         * sandbox never sees at build time.
         */
        [[nodiscard]] bool add_prefabs(const engine::assets::Content& cooked,
                                       engine::scene::PrefabLibrary& library) {
            std::size_t added = 0;

            for (const engine::assets::ManifestEntry& entry : cooked.manifest().entries) {
                for (const engine::assets::ManifestOutput& output : entry.outputs) {
                    if (!std::string_view{ output.cooked }.ends_with(
                            engine::assets::kPrefabExtension)) {
                        continue;
                    }

                    std::vector<std::byte> bytes;
                    if (!cooked.read_bytes(output, bytes)) {
                        return false;
                    }
                    const nlohmann::json document =
                        nlohmann::json::parse(std::string_view{
                                                  reinterpret_cast<const char*>(bytes.data()),
                                                  bytes.size() },
                                              nullptr, false);
                    if (document.is_discarded()) {
                        ENGINE_LOG_ERROR("{} will not parse as a prefab.", output.cooked);
                        return false;
                    }
                    if (!library.add(prefab_name(entry.source, output.cooked), document)) {
                        ENGINE_LOG_ERROR("{} is not a prefab this build can use.", output.cooked);
                        return false;
                    }
                    ++added;
                }
            }

            ENGINE_LOG_INFO("The sandbox registered {} prefabs.", added);
            return true;
        }

    } // namespace

    std::string prefab_name(std::string_view source, std::string_view cooked) {
        // A prefab the cooker copied rather than wrote keeps the source path,
        // extension and all, and there is no scene index to read. This is
        // compared before the extension comes off, because the source path of a
        // copied prefab already ends in it.
        if (cooked == source) {
            return std::string{ source };
        }

        std::string_view stem = cooked;
        if (stem.ends_with(engine::assets::kPrefabExtension)) {
            stem.remove_suffix(std::string_view{ engine::assets::kPrefabExtension }.size());
        }

        // A glTF writes "<source>.<scene>.prefab". The index is read from the
        // path rather than counted, because the identity of the prefab is
        // derived from that same scene index. Counting would be a second
        // source of truth, and the two would disagree the moment the manifest
        // held the outputs of one source in another order.
        if (stem.size() > source.size() + 1 && stem.starts_with(source) &&
            stem[source.size()] == '.') {
            const std::string_view digits = stem.substr(source.size() + 1);
            if (!digits.empty() && std::ranges::all_of(digits, [](char c) {
                    return c >= '0' && c <= '9';
                })) {
                return digits == "0" ? std::string{ source }
                                     : std::string{ source } + "#" + std::string{ digits };
            }
        }

        // A shape nobody planned for. The cooked path is still unique, so this
        // names the prefab rather than colliding with the source path.
        return std::string{ stem };
    }

    std::filesystem::path default_content_directory() {
        return engine::platform::cooked_content_root() / kContentName;
    }

    void register_components(engine::scene::ComponentRegistry& registry) {
        registry.add<Spin>();
        registry.add<Goal>();
    }

    bool load(const std::filesystem::path& content, const engine::assets::Content* cooked,
              engine::scene::World& world, const engine::scene::ComponentRegistry& registry,
              engine::scene::PrefabLibrary& library) {
        // The prefabs go in first. A scene that names one the library does not
        // hold cannot build its entities.
        //
        // They come out of the cooker rather than out of the source tree, so
        // they are found through the manifest and read by identity. A
        // hand-authored prefab is in there too, because the cooker copies what
        // it has no rule for. The path is what a person edits, and the manifest
        // says what that path became.
        if (cooked != nullptr && !add_prefabs(*cooked, library)) {
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

} // namespace sandbox
