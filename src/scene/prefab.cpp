#include "scene/prefab.h"

#include "core/log.h"
#include "scene/document.h"

#include <fstream>
#include <utility>

namespace engine::scene {

    namespace {

        /// The patch for one entity of an instance, or nullptr when it has none.
        const nlohmann::json* patch_for(const nlohmann::json& overrides, std::size_t index) {
            if (!overrides.is_object()) {
                return nullptr;
            }
            const auto found = overrides.find(std::to_string(index));
            return found == overrides.end() ? nullptr : &(*found);
        }

        /// Reads one entity record out of a prefab document.
        bool parse_entity(const nlohmann::json& record, std::size_t index,
                          std::string_view name, PrefabEntity& out) {
            if (!record.is_object()) {
                ENGINE_LOG_ERROR("Prefab {}: entity {} is not an object.", name, index);
                return false;
            }

            out.parent = record.value(kParentKey, kNoParent);
            if (index == 0) {
                if (out.parent != kNoParent) {
                    ENGINE_LOG_ERROR("Prefab {}: the first entity must be the root.", name);
                    return false;
                }
            } else if (out.parent < 0 || static_cast<std::size_t>(out.parent) >= index) {
                // A parent that comes later, or no parent at all, would give a
                // second root or a cycle. Both break the one forward pass that
                // instantiate() relies on.
                ENGINE_LOG_ERROR("Prefab {}: entity {} names parent {}, which must be an "
                                 "earlier entity.",
                                 name, index, out.parent);
                return false;
            }

            if (const auto parts = record.find(kComponentsKey); parts != record.end()) {
                if (!parts->is_object()) {
                    ENGINE_LOG_ERROR("Prefab {}: entity {} holds components that are not an "
                                     "object.",
                                     name, index);
                    return false;
                }
                out.components = *parts;
            }
            return true;
        }

        /// Lists the members of one instance, in hierarchy order.
        std::vector<entt::entity> members_of(const entt::registry& entities, entt::entity root) {
            std::vector<entt::entity> found;
            std::vector<entt::entity> stack{ root };
            while (!stack.empty()) {
                const entt::entity current = stack.back();
                stack.pop_back();

                const auto* member = entities.try_get<PrefabMember>(current);
                if (member != nullptr && member->root == root) {
                    found.push_back(current);
                }

                const Hierarchy& node = entities.get<Hierarchy>(current);
                for (entt::entity child = node.first_child; child != entt::null;
                     child = entities.get<Hierarchy>(child).next_sibling) {
                    stack.push_back(child);
                }
            }
            return found;
        }

    } // namespace

    bool Prefab::parse(std::string name, const nlohmann::json& document, Prefab& out) {
        if (!document.is_object()) {
            ENGINE_LOG_ERROR("Prefab {} is {}, not an object.", name, document.type_name());
            return false;
        }

        const auto version = document.find(kVersionKey);
        if (version == document.end() || !version->is_number_integer()) {
            ENGINE_LOG_ERROR("Prefab {} must carry an integer {}.", name, kVersionKey);
            return false;
        }
        if (version->get<std::uint32_t>() > kPrefabVersion) {
            ENGINE_LOG_ERROR("Prefab {} is version {}, and this build reads up to {}.", name,
                             version->get<std::uint32_t>(), kPrefabVersion);
            return false;
        }

        const auto list = document.find(kEntitiesKey);
        if (list == document.end() || !list->is_array()) {
            ENGINE_LOG_ERROR("Prefab {} must carry an array of {}.", name, kEntitiesKey);
            return false;
        }
        if (list->empty()) {
            ENGINE_LOG_ERROR("Prefab {} holds no entity. A prefab needs a root.", name);
            return false;
        }

        std::vector<PrefabEntity> parsed;
        parsed.reserve(list->size());
        for (std::size_t i = 0; i < list->size(); ++i) {
            PrefabEntity entity;
            if (!parse_entity((*list)[i], i, name, entity)) {
                return false;
            }
            parsed.push_back(std::move(entity));
        }

        out.name_ = std::move(name);
        out.entities_ = std::move(parsed);
        return true;
    }

    bool PrefabLibrary::add(std::string name, const nlohmann::json& document) {
        Prefab prefab;
        if (!Prefab::parse(std::move(name), document, prefab)) {
            return false;
        }

        for (Prefab& entry : entries_) {
            if (entry.name() == prefab.name()) {
                entry = std::move(prefab);
                return true;
            }
        }
        entries_.push_back(std::move(prefab));
        return true;
    }

    bool PrefabLibrary::add_file(std::string name, const std::filesystem::path& path) {
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

        return add(std::move(name), document);
    }

    const Prefab* PrefabLibrary::find(std::string_view name) const {
        for (const Prefab& entry : entries_) {
            if (entry.name() == name) {
                return &entry;
            }
        }
        return nullptr;
    }

    PrefabLibrary& prefabs() {
        // A function-local static builds on the first call, so this is ready
        // before main() runs and needs no start-up order rule.
        static PrefabLibrary library;
        return library;
    }

    entt::entity instantiate(World& world, const Prefab& prefab, const nlohmann::json& overrides,
                             const ComponentRegistry& registry) {
        if (prefab.size() == 0) {
            ENGINE_LOG_ERROR("Prefab {} holds no entity.", prefab.name());
            return entt::null;
        }
        if (!overrides.is_null() && !overrides.is_object()) {
            ENGINE_LOG_ERROR("Prefab {}: the overrides are {}, not an object.", prefab.name(),
                             overrides.type_name());
            return entt::null;
        }

        std::vector<entt::entity> created;
        created.reserve(prefab.size());
        for (std::size_t i = 0; i < prefab.size(); ++i) {
            created.push_back(world.create());
        }
        const entt::entity root = created.front();

        bool ok = true;
        for (std::size_t i = 0; i < prefab.size(); ++i) {
            const PrefabEntity& source = prefab.entities()[i];
            if (source.parent != kNoParent) {
                // parse() promised the parent comes first, so it already exists.
                if (!world.set_parent(created[i], created[static_cast<std::size_t>(source.parent)])) {
                    ENGINE_LOG_ERROR("Prefab {}: entity {} could not attach.", prefab.name(), i);
                    ok = false;
                }
            }
            world.registry().emplace<PrefabMember>(created[i],
                                                   PrefabMember{ .root = root, .index = i });

            // The prefab holds the defaults and the patch holds the changes, so
            // a field the instance left alone still comes from the prefab. That
            // is the whole of "editing the prefab reaches every instance".
            nlohmann::json parts = source.components;
            if (const nlohmann::json* patch = patch_for(overrides, i); patch != nullptr) {
                parts.merge_patch(*patch);
            }

            const std::string where = prefab.name() + " entity " + std::to_string(i);
            if (!load_components(parts, world.registry(), created[i], registry, where)) {
                ok = false;
            }
        }

        world.registry().emplace<PrefabInstance>(root, PrefabInstance{ .prefab = prefab.name() });

        if (!ok) {
            // Every created entity is the root or a descendant of it, so one
            // destroy leaves no orphan behind.
            world.destroy(root);
            return entt::null;
        }
        return root;
    }

    nlohmann::json override_patch(const nlohmann::json& base, const nlohmann::json& live) {
        if (!base.is_object() || !live.is_object()) {
            // An array is a value, not a container of fields. Half a position is
            // not a useful override, so the whole value goes in the patch.
            return live;
        }

        nlohmann::json patch = nlohmann::json::object();
        for (const auto& [key, value] : live.items()) {
            const auto found = base.find(key);
            if (found == base.end()) {
                patch[key] = value;
            } else if (*found != value) {
                patch[key] = override_patch(*found, value);
            }
        }
        for (const auto& entry : base.items()) {
            if (live.find(entry.key()) == live.end()) {
                // A merge patch says "remove this key" with a null.
                patch[entry.key()] = nullptr;
            }
        }
        return patch;
    }

    nlohmann::json instance_overrides(const World& world, entt::entity root,
                                      const Prefab& prefab, const ComponentRegistry& registry) {
        const entt::registry& entities = world.registry();

        nlohmann::json out = nlohmann::json::object();
        for (const entt::entity member : members_of(entities, root)) {
            const std::size_t index = entities.get<PrefabMember>(member).index;
            if (index >= prefab.size()) {
                ENGINE_LOG_WARN("An instance of {} names entity {}, and the prefab holds {}. "
                                "The prefab changed shape.",
                                prefab.name(), index, prefab.size());
                continue;
            }

            nlohmann::json patch =
                override_patch(prefab.entities()[index].components,
                               save_components(entities, member, registry));
            if (!patch.empty()) {
                out[std::to_string(index)] = std::move(patch);
            }
        }
        return out;
    }

} // namespace engine::scene
