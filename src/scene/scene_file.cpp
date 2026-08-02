#include "scene/scene_file.h"

#include "core/log.h"
#include "scene/document.h"

#include <algorithm>
#include <fstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace engine::scene {

    namespace {

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
            return load_components(*parts, world.registry(), entity, registry,
                                   "Entity " + std::to_string(self));
        }

        /// What one record produced, and whether the reader had to complain.
        struct Built {
            entt::entity entity = entt::null; ///< The entity, or an instance root.
            bool ok = true;                   ///< False when the record named something wrong.
        };

        /**
         * Creates the entity one record asks for.
         *
         * A record that names a prefab builds a whole instance, and the root
         * comes back. Anything the reader cannot build leaves an empty entity
         * behind, so every later parent index still points where the file says.
         */
        Built build_entity(const nlohmann::json& record, std::size_t self, World& world,
                           const ComponentRegistry& registry, const PrefabLibrary& library) {
            if (!record.is_object()) {
                // The second pass reports the shape. Keep the index in step.
                return { .entity = world.create(), .ok = true };
            }

            const auto name = record.find(kPrefabKey);
            if (name == record.end()) {
                return { .entity = world.create(), .ok = true };
            }
            if (!name->is_string()) {
                ENGINE_LOG_ERROR("Entity {} names a prefab that is not a string.", self);
                return { .entity = world.create(), .ok = false };
            }

            const std::string wanted = name->get<std::string>();
            const Prefab* prefab = library.find(wanted);
            if (prefab == nullptr) {
                ENGINE_LOG_ERROR("Entity {} is an instance of prefab {}, and the library does "
                                 "not hold it.",
                                 self, wanted);
                return { .entity = world.create(), .ok = false };
            }

            const entt::entity root =
                instantiate(world, *prefab,
                            record.value(kOverridesKey, nlohmann::json::object()), registry);
            if (root == entt::null) {
                ENGINE_LOG_ERROR("Entity {} could not build an instance of prefab {}.", self,
                                 wanted);
                return { .entity = world.create(), .ok = false };
            }
            return { .entity = root, .ok = true };
        }

        /// The instance roots the writer can collapse to a name and a patch.
        std::unordered_set<entt::entity> collapsible(const entt::registry& entities,
                                                     const std::vector<entt::entity>& ordered,
                                                     const PrefabLibrary& library) {
            std::unordered_set<entt::entity> roots;
            for (const entt::entity entity : ordered) {
                const auto* link = entities.try_get<PrefabInstance>(entity);
                if (link == nullptr) {
                    continue;
                }
                if (library.find(link->prefab) != nullptr) {
                    roots.insert(entity);
                    continue;
                }
                // Without the prefab there is nothing to compare against, so a
                // link would throw away every change the instance holds. Write
                // the entities one by one instead, and say so.
                ENGINE_LOG_ERROR("An instance of prefab {} is in the world, and the library "
                                 "does not hold that prefab. Writing its entities one by one, "
                                 "and the link is lost.",
                                 link->prefab);
            }
            return roots;
        }

        /// Picks the entities that get a record of their own, and numbers them.
        std::vector<entt::entity> choose_records(
            const entt::registry& entities, const std::vector<entt::entity>& ordered,
            const std::unordered_set<entt::entity>& collapsed,
            std::unordered_map<entt::entity, int>& index) {
            std::vector<entt::entity> written;
            for (const entt::entity entity : ordered) {
                const auto* member = entities.try_get<PrefabMember>(entity);
                if (member != nullptr && member->root != entity &&
                    collapsed.contains(member->root)) {
                    // The prefab already holds this entity. The root carries the
                    // fields it changed.
                    continue;
                }

                const Hierarchy& node = entities.get<Hierarchy>(entity);
                if (node.parent != entt::null && !index.contains(node.parent)) {
                    // The parent went into a prefab instance, so there is no
                    // index to point at. Attaching your own entity inside an
                    // instance is not stored yet. See issue #27.
                    ENGINE_LOG_WARN("An entity sits under a prefab instance without belonging "
                                    "to it. A scene file does not store that yet, so the "
                                    "entity and its children are dropped.");
                    continue;
                }

                index[entity] = static_cast<int>(written.size());
                written.push_back(entity);
            }
            return written;
        }

    } // namespace

    nlohmann::json save_scene(const World& world, const ComponentRegistry& registry,
                              const PrefabLibrary& library) {
        const entt::registry& entities = world.registry();
        const std::vector<entt::entity> ordered = walk_in_order(world);
        const std::unordered_set<entt::entity> collapsed =
            collapsible(entities, ordered, library);

        std::unordered_map<entt::entity, int> index;
        index.reserve(ordered.size());
        const std::vector<entt::entity> written =
            choose_records(entities, ordered, collapsed, index);

        nlohmann::json out = nlohmann::json::object();
        out[kVersionKey] = kSceneVersion;

        nlohmann::json list = nlohmann::json::array();
        for (const entt::entity entity : written) {
            nlohmann::json record = nlohmann::json::object();

            const Hierarchy& node = entities.get<Hierarchy>(entity);
            record[kParentKey] =
                node.parent == entt::null ? kNoParent : index.at(node.parent);

            if (collapsed.contains(entity)) {
                const PrefabInstance& link = entities.get<PrefabInstance>(entity);
                const Prefab* prefab = library.find(link.prefab);
                record[kPrefabKey] = link.prefab;

                // collapsible() already found the prefab, so this cannot fail.
                nlohmann::json patch = instance_overrides(world, entity, *prefab, registry);
                if (!patch.empty()) {
                    record[kOverridesKey] = std::move(patch);
                }
            } else {
                record[kComponentsKey] = save_components(entities, entity, registry);
            }

            list.push_back(std::move(record));
        }

        out[kEntitiesKey] = std::move(list);
        return out;
    }

    bool load_scene(const nlohmann::json& document, World& world,
                    const ComponentRegistry& registry, const PrefabLibrary& library) {
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
        bool ok = true;
        std::vector<entt::entity> created;
        created.reserve(list->size());
        for (std::size_t i = 0; i < list->size(); ++i) {
            const Built built = build_entity((*list)[i], i, world, registry, library);
            created.push_back(built.entity);
            ok = built.ok && ok;
        }

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
                         const ComponentRegistry& registry, const PrefabLibrary& library) {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        if (!file) {
            ENGINE_LOG_ERROR("Could not open {} for writing.", path.string());
            return false;
        }

        constexpr int kIndent = 2;
        file << save_scene(world, registry, library).dump(kIndent) << '\n';
        if (!file) {
            ENGINE_LOG_ERROR("Could not write {}.", path.string());
            return false;
        }
        return true;
    }

    bool load_scene_file(const std::filesystem::path& path, World& world,
                         const ComponentRegistry& registry, const PrefabLibrary& library) {
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

        return load_scene(document, world, registry, library);
    }

} // namespace engine::scene
