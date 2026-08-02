#include "scene/scene_file.h"

#include "core/log.h"

#include <algorithm>
#include <fstream>
#include <unordered_map>
#include <vector>

namespace engine::scene {

    namespace {

        constexpr const char* kVersionKey = "__version";
        constexpr const char* kEntitiesKey = "entities";
        constexpr const char* kComponentsKey = "components";
        constexpr const char* kParentKey = "parent";
        /// The value a root stores for its parent. No entity holds index -1.
        constexpr int kNoParent = -1;

        /**
         * Lists every entity, parents before children, in a stable order.
         *
         * The roots come out sorted by their raw entity value, and the children
         * come out in child-list order. Both are stable across a save and a
         * load, which is what makes the document reproducible.
         */
        std::vector<entt::entity> walk_in_order(const World& world) {
            const entt::registry& registry = world.registry();

            std::vector<entt::entity> roots;
            for (const auto [entity, node] : registry.view<const Hierarchy>().each()) {
                if (node.parent == entt::null) {
                    roots.push_back(entity);
                }
            }
            // A view does not promise an iteration order, and a scene file has
            // to be reproducible, so pin the order here.
            std::sort(roots.begin(), roots.end(), [](entt::entity left, entt::entity right) {
                return entt::to_integral(left) < entt::to_integral(right);
            });

            std::vector<entt::entity> ordered;
            ordered.reserve(registry.view<const Hierarchy>().size());

            // Depth first, and a parent always lands before its children.
            std::vector<entt::entity> stack;
            for (auto root = roots.rbegin(); root != roots.rend(); ++root) {
                stack.push_back(*root);
            }
            while (!stack.empty()) {
                const entt::entity current = stack.back();
                stack.pop_back();
                ordered.push_back(current);

                const Hierarchy& node = registry.get<Hierarchy>(current);
                std::vector<entt::entity> children;
                for (entt::entity child = node.first_child; child != entt::null;
                     child = registry.get<Hierarchy>(child).next_sibling) {
                    children.push_back(child);
                }
                for (auto child = children.rbegin(); child != children.rend(); ++child) {
                    stack.push_back(*child);
                }
            }
            return ordered;
        }

        /// Checks the document shape and finds the entity array.
        bool read_header(const nlohmann::json& document, const nlohmann::json** out_entities) {
            if (!document.is_object()) {
                ENGINE_LOG_ERROR("A scene document must be an object, not {}.",
                                 document.type_name());
                return false;
            }

            const auto version = document.find(kVersionKey);
            if (version == document.end() || !version->is_number_integer()) {
                ENGINE_LOG_ERROR("A scene document must carry an integer {}.", kVersionKey);
                return false;
            }
            if (version->get<std::uint32_t>() > kSceneVersion) {
                ENGINE_LOG_ERROR("The scene is version {}, and this build reads up to {}.",
                                 version->get<std::uint32_t>(), kSceneVersion);
                return false;
            }

            const auto list = document.find(kEntitiesKey);
            if (list == document.end() || !list->is_array()) {
                ENGINE_LOG_ERROR("A scene document must carry an array of {}.", kEntitiesKey);
                return false;
            }

            *out_entities = &(*list);
            return true;
        }

        /// Attaches one entity to its parent, if the record names one.
        bool apply_parent(const nlohmann::json& record, std::size_t self,
                          const std::vector<entt::entity>& created, World& world) {
            const int parent = record.value(kParentKey, kNoParent);
            if (parent == kNoParent) {
                return true;
            }
            if (parent < 0 || static_cast<std::size_t>(parent) >= created.size()) {
                ENGINE_LOG_ERROR("Entity {} names parent {}, which is not in the file.", self,
                                 parent);
                return false;
            }
            if (!world.set_parent(created[self], created[static_cast<std::size_t>(parent)])) {
                ENGINE_LOG_ERROR("Entity {} could not attach to parent {}.", self, parent);
                return false;
            }
            return true;
        }

        /// Builds every component the record names and the registry knows.
        bool apply_components(const nlohmann::json& record, std::size_t self,
                              entt::entity entity, World& world,
                              const ComponentRegistry& registry) {
            const auto parts = record.find(kComponentsKey);
            if (parts == record.end()) {
                return true;
            }
            if (!parts->is_object()) {
                ENGINE_LOG_ERROR("Entity {} holds {} that is not an object.", self,
                                 kComponentsKey);
                return false;
            }

            bool ok = true;
            for (const auto& [name, value] : parts->items()) {
                const ComponentOps* ops = registry.find(name);
                if (ops == nullptr) {
                    // An older build reading a newer file lands here. Keep the
                    // rest of the entity rather than refuse the whole scene.
                    ENGINE_LOG_WARN("Entity {} carries component {}, which this build does "
                                    "not know. Skipping it.",
                                    self, name);
                    continue;
                }
                if (!ops->load(world.registry(), entity, value)) {
                    ENGINE_LOG_ERROR("Entity {} could not read its {} component.", self, name);
                    ok = false;
                }
            }
            return ok;
        }

    } // namespace

    nlohmann::json save_scene(const World& world, const ComponentRegistry& registry) {
        const entt::registry& entities = world.registry();
        const std::vector<entt::entity> ordered = walk_in_order(world);

        std::unordered_map<entt::entity, int> index;
        index.reserve(ordered.size());
        for (std::size_t i = 0; i < ordered.size(); ++i) {
            index[ordered[i]] = static_cast<int>(i);
        }

        nlohmann::json out = nlohmann::json::object();
        out[kVersionKey] = kSceneVersion;

        nlohmann::json list = nlohmann::json::array();
        for (const entt::entity entity : ordered) {
            nlohmann::json record = nlohmann::json::object();

            const Hierarchy& node = entities.get<Hierarchy>(entity);
            record[kParentKey] =
                node.parent == entt::null ? kNoParent : index.at(node.parent);

            nlohmann::json parts = nlohmann::json::object();
            for (const ComponentOps& ops : registry.all()) {
                if (ops.has(entities, entity)) {
                    parts[ops.name] = ops.save(entities, entity);
                }
            }
            record[kComponentsKey] = std::move(parts);
            list.push_back(std::move(record));
        }

        out[kEntitiesKey] = std::move(list);
        return out;
    }

    bool load_scene(const nlohmann::json& document, World& world,
                    const ComponentRegistry& registry) {
        if (world.size() != 0) {
            ENGINE_LOG_ERROR("load_scene needs an empty world. It holds {} entities.",
                             world.size());
            return false;
        }

        const nlohmann::json* list = nullptr;
        if (!read_header(document, &list)) {
            return false;
        }

        // Create every entity first, so a parent index always resolves. The
        // writer puts parents first, but a hand-edited file may not.
        std::vector<entt::entity> created;
        created.reserve(list->size());
        for (std::size_t i = 0; i < list->size(); ++i) {
            created.push_back(world.create());
        }

        bool ok = true;
        for (std::size_t i = 0; i < list->size(); ++i) {
            const nlohmann::json& record = (*list)[i];
            if (!record.is_object()) {
                ENGINE_LOG_ERROR("Entity {} is not an object.", i);
                ok = false;
                continue;
            }

            ok = apply_parent(record, i, created, world) && ok;
            ok = apply_components(record, i, created[i], world, registry) && ok;
        }

        return ok;
    }

    bool save_scene_file(const std::filesystem::path& path, const World& world,
                         const ComponentRegistry& registry) {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        if (!file) {
            ENGINE_LOG_ERROR("Could not open {} for writing.", path.string());
            return false;
        }

        constexpr int kIndent = 2;
        file << save_scene(world, registry).dump(kIndent) << '\n';
        if (!file) {
            ENGINE_LOG_ERROR("Could not write {}.", path.string());
            return false;
        }
        return true;
    }

    bool load_scene_file(const std::filesystem::path& path, World& world,
                         const ComponentRegistry& registry) {
        std::ifstream file(path, std::ios::binary);
        if (!file) {
            ENGINE_LOG_ERROR("Could not open {} for reading.", path.string());
            return false;
        }

        // Exceptions off, per reflect/json.h. A bad document comes back
        // discarded instead of thrown.
        const nlohmann::json document = nlohmann::json::parse(file, nullptr, false);
        if (document.is_discarded()) {
            ENGINE_LOG_ERROR("{} is not valid JSON.", path.string());
            return false;
        }

        return load_scene(document, world, registry);
    }

} // namespace engine::scene
