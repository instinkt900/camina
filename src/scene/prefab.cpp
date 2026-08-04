#include "scene/prefab.h"

#include "core/log.h"
#include "scene/document.h"

#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <map>
#include <set>
#include <unordered_map>
#include <utility>
#include <vector>

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

        /**
         * An instance record, read into the form instantiate() walks.
         *
         * The index space runs over the prefab's own entities first, 0 to
         * N-1, and then over the entities the instance added. One space covers
         * both, so a parent index reads the same whichever kind it names.
         */
        struct InstanceRecord {
            /// The field patches, keyed by index written as a decimal string.
            nlohmann::json overrides = nlohmann::json::object();
            /// The entities the instance added, in the order the file lists them.
            std::vector<PrefabEntity> added;
            /// Every index that is not built, a removed member's children included.
            std::set<std::size_t> gone;
            /// A member that moved, from its index to its new parent index.
            std::map<std::size_t, int> moved;

            /// Where entity @p index hangs, after the instance had its say.
            [[nodiscard]] int parent_of(std::size_t index, const Prefab& prefab) const {
                if (const auto found = moved.find(index); found != moved.end()) {
                    return found->second;
                }
                return index < prefab.size() ? prefab.entities()[index].parent
                                             : added[index - prefab.size()].parent;
            }

            /// The components entity @p index starts from, before any patch.
            [[nodiscard]] const nlohmann::json& components_of(std::size_t index,
                                                              const Prefab& prefab) const {
                return index < prefab.size() ? prefab.entities()[index].components
                                             : added[index - prefab.size()].components;
            }
        };

        /// Reads the array of member indices an instance destroyed.
        [[nodiscard]] bool read_removed(const nlohmann::json& record, const Prefab& prefab,
                                        std::set<std::size_t>& out) {
            const auto found = record.find(kRemovedKey);
            if (found == record.end()) {
                return true;
            }
            if (!found->is_array()) {
                ENGINE_LOG_ERROR("Prefab {}: {} is {}, and it must be an array.", prefab.name(),
                                 kRemovedKey, found->type_name());
                return false;
            }
            for (const nlohmann::json& value : *found) {
                // Signed, because a number a person types is signed whatever
                // its sign. Asking for unsigned here refused "removed": [1]
                // out of a hand-written file, which is the case this exists
                // for.
                if (!value.is_number_integer()) {
                    ENGINE_LOG_ERROR("Prefab {}: {} holds {}, and every entry must be a "
                                     "whole number.",
                                     prefab.name(), kRemovedKey, value.type_name());
                    return false;
                }
                const auto given = value.get<std::int64_t>();
                if (given < 0) {
                    ENGINE_LOG_ERROR("Prefab {}: {} names entity {}, and an index is never "
                                     "negative.",
                                     prefab.name(), kRemovedKey, given);
                    return false;
                }
                const auto index = static_cast<std::size_t>(given);
                if (index >= prefab.size()) {
                    // Only a member can be removed. An entity the instance
                    // added is removed by deleting its record.
                    ENGINE_LOG_ERROR("Prefab {}: {} names entity {}, and the prefab holds {}.",
                                     prefab.name(), kRemovedKey, index, prefab.size());
                    return false;
                }
                out.insert(index);
            }
            return true;
        }

        /// Reads the entities an instance added under itself.
        [[nodiscard]] bool read_added(const nlohmann::json& record, const Prefab& prefab,
                                      std::vector<PrefabEntity>& out) {
            const auto found = record.find(kAddedKey);
            if (found == record.end()) {
                return true;
            }
            if (!found->is_array()) {
                ENGINE_LOG_ERROR("Prefab {}: {} is {}, and it must be an array.", prefab.name(),
                                 kAddedKey, found->type_name());
                return false;
            }

            for (std::size_t at = 0; at < found->size(); ++at) {
                const nlohmann::json& entry = (*found)[at];
                const std::size_t index = prefab.size() + at;
                const std::string where =
                    "Prefab " + prefab.name() + " added entity " + std::to_string(index);
                if (!entry.is_object()) {
                    ENGINE_LOG_ERROR("{} is not an object.", where);
                    return false;
                }

                PrefabEntity built;
                if (!read_parent(entry, where, built.parent)) {
                    return false;
                }
                // An added entity hangs off the instance, so it always has a
                // parent, and that parent has to exist by the time this one
                // does. Naming a later entity would give a cycle no walk ends.
                if (built.parent < 0 || static_cast<std::size_t>(built.parent) >= index) {
                    ENGINE_LOG_ERROR("{} names parent {}, which must be an earlier entity.",
                                     where, built.parent);
                    return false;
                }
                if (const auto parts = entry.find(kComponentsKey); parts != entry.end()) {
                    if (!parts->is_object()) {
                        ENGINE_LOG_ERROR("{} holds components that are not an object.", where);
                        return false;
                    }
                    built.components = *parts;
                }
                out.push_back(std::move(built));
            }
            return true;
        }

        /// Reads the members an instance moved, and where it moved them to.
        [[nodiscard]] bool read_reparented(const nlohmann::json& record, const Prefab& prefab,
                                           std::size_t total, std::map<std::size_t, int>& out) {
            const auto found = record.find(kReparentedKey);
            if (found == record.end()) {
                return true;
            }
            if (!found->is_object()) {
                ENGINE_LOG_ERROR("Prefab {}: {} is {}, and it must be an object.",
                                 prefab.name(), kReparentedKey, found->type_name());
                return false;
            }

            for (const auto& [key, value] : found->items()) {
                const std::size_t index = std::strtoull(key.c_str(), nullptr, 10);
                if (index == 0 || index >= total) {
                    // Entity 0 is the instance root. Its parent belongs to the
                    // scene record, not to the instance.
                    ENGINE_LOG_ERROR("Prefab {}: {} names entity {}, which is not a member "
                                     "this instance can move.",
                                     prefab.name(), kReparentedKey, key);
                    return false;
                }
                if (!value.is_number_integer()) {
                    ENGINE_LOG_ERROR("Prefab {}: {} gives entity {} a parent that is not a "
                                     "whole number.",
                                     prefab.name(), kReparentedKey, key);
                    return false;
                }
                const auto parent = value.get<int>();
                if (parent < 0 || static_cast<std::size_t>(parent) >= total ||
                    static_cast<std::size_t>(parent) == index) {
                    ENGINE_LOG_ERROR("Prefab {}: {} moves entity {} under {}, which is not "
                                     "an entity of this instance.",
                                     prefab.name(), kReparentedKey, key, parent);
                    return false;
                }
                out.emplace(index, parent);
            }
            return true;
        }

        /**
         * Reads an instance record, and works out what is really built.
         *
         * Destroying an entity in a world takes its subtree with it, so a
         * record written from a live world lists every one. A record somebody
         * edited may name a parent and leave a child behind, and building that
         * child would attach it to an entity that does not exist. So the
         * removal closes over the tree here, and says when it had to.
         */
        [[nodiscard]] bool read_instance_record(const nlohmann::json& record,
                                                const Prefab& prefab, InstanceRecord& out) {
            if (record.is_object()) {
                if (const auto found = record.find(kOverridesKey); found != record.end()) {
                    if (!found->is_object()) {
                        ENGINE_LOG_ERROR("Prefab {}: {} is {}, not an object.", prefab.name(),
                                         kOverridesKey, found->type_name());
                        return false;
                    }
                    out.overrides = *found;
                }
                if (!read_removed(record, prefab, out.gone) ||
                    !read_added(record, prefab, out.added)) {
                    return false;
                }
                if (!read_reparented(record, prefab, prefab.size() + out.added.size(),
                                     out.moved)) {
                    return false;
                }
            }

            // To a fixed point rather than in one pass. A member moved under an
            // entity the instance added can name a parent with a higher index,
            // so a single forward sweep would walk past a removal that has not
            // reached it yet. The set only grows and it is bounded, so this
            // ends.
            const std::size_t total = prefab.size() + out.added.size();
            for (bool growing = true; growing;) {
                growing = false;
                for (std::size_t i = 1; i < total; ++i) {
                    if (out.gone.contains(i)) {
                        continue;
                    }
                    const int parent = out.parent_of(i, prefab);
                    if (parent >= 0 && out.gone.contains(static_cast<std::size_t>(parent))) {
                        ENGINE_LOG_WARN("Prefab {}: entity {} sits under {}, which this "
                                        "instance removed, so it goes as well.",
                                        prefab.name(), i, parent);
                        out.gone.insert(i);
                        growing = true;
                    }
                }
            }
            return true;
        }

        /**
         * Attaches one entity of an instance and loads its components.
         *
         * Every entity already exists by the time this runs, so a parent with a
         * higher index is no problem. That is what lets a member move under an
         * entity the instance added.
         */
        [[nodiscard]] bool build_member(World& world, const Prefab& prefab,
                                        const InstanceRecord& parts,
                                        const ComponentRegistry& registry,
                                        const std::vector<entt::entity>& created,
                                        std::size_t index, entt::entity root) {
            bool ok = true;
            const int parent = parts.parent_of(index, prefab);
            if (parent != kNoParent &&
                !world.set_parent(created[index], created[static_cast<std::size_t>(parent)])) {
                ENGINE_LOG_ERROR("Prefab {}: entity {} could not attach.", prefab.name(), index);
                ok = false;
            }

            // Only a prefab member carries the link back. An entity the
            // instance added belongs to the scene, so a later save writes it
            // out as an addition rather than comparing it against a template
            // that never held it.
            const bool is_member = index < prefab.size();
            if (is_member) {
                world.registry().emplace<PrefabMember>(
                    created[index], PrefabMember{ .root = root, .index = index });
            }

            // The prefab holds the defaults and the patch holds the changes, so
            // a field the instance left alone still comes from the prefab. That
            // is the whole of "editing the prefab reaches every instance".
            nlohmann::json components = parts.components_of(index, prefab);
            const nlohmann::json* patch =
                is_member ? patch_for(parts.overrides, index) : nullptr;
            if (patch != nullptr) {
                components.merge_patch(*patch);
            }

            const std::string where = prefab.name() + " entity " + std::to_string(index);
            return load_components(components, world.registry(), created[index], registry, where) &&
                   ok;
        }

        /// Reads one entity record out of a prefab document.
        bool parse_entity(const nlohmann::json& record, std::size_t index,
                          std::string_view name, PrefabEntity& out) {
            if (!record.is_object()) {
                ENGINE_LOG_ERROR("Prefab {}: entity {} is not an object.", name, index);
                return false;
            }

            const std::string where =
                "Prefab " + std::string(name) + " entity " + std::to_string(index);
            if (!read_parent(record, where, out.parent)) {
                return false;
            }

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
        /**
         * Numbers every entity of an instance, parents before children.
         *
         * A member keeps the index the prefab gave it. Anything else under the
         * root is an addition, and those continue from the end of the prefab.
         * The walk is depth first, so an addition can always name its parent by
         * an index that is already assigned.
         */
        void number_instance(const entt::registry& entities, entt::entity root,
                             const Prefab& prefab, std::unordered_map<entt::entity, int>& index,
                             std::vector<entt::entity>& extra) {
            std::vector<entt::entity> stack{ root };
            while (!stack.empty()) {
                const entt::entity current = stack.back();
                stack.pop_back();

                const auto* member = entities.try_get<PrefabMember>(current);
                const bool mine = member != nullptr && member->root == root;
                if (mine && member->index < prefab.size()) {
                    index[current] = static_cast<int>(member->index);
                } else {
                    if (mine) {
                        // The prefab lost entities since this instance was
                        // built, so this one has no template to compare
                        // against. Writing it as an addition keeps it, where
                        // dropping it would lose what the scene put on it.
                        ENGINE_LOG_WARN("An instance of {} names entity {}, and the prefab "
                                        "holds {}. The prefab changed shape, so that entity "
                                        "is written as an addition.",
                                        prefab.name(), member->index, prefab.size());
                    }
                    index[current] = static_cast<int>(prefab.size() + extra.size());
                    extra.push_back(current);
                }

                std::vector<entt::entity> children;
                for (entt::entity child = entities.get<Hierarchy>(current).first_child;
                     child != entt::null; child = entities.get<Hierarchy>(child).next_sibling) {
                    children.push_back(child);
                }
                for (auto child = children.rbegin(); child != children.rend(); ++child) {
                    stack.push_back(*child);
                }
            }
        }

        /// The prefab indices with no live entity behind them.
        [[nodiscard]] nlohmann::json removed_members(const std::vector<entt::entity>& present) {
            nlohmann::json out = nlohmann::json::array();
            for (std::size_t i = 0; i < present.size(); ++i) {
                if (present[i] == entt::null) {
                    out.push_back(i);
                }
            }
            return out;
        }

        /// The entities the scene added under an instance, with their parents.
        [[nodiscard]] nlohmann::json added_entities(
            const entt::registry& entities, const std::vector<entt::entity>& extra,
            const std::unordered_map<entt::entity, int>& index,
            const ComponentRegistry& registry) {
            nlohmann::json out = nlohmann::json::array();
            for (const entt::entity entity : extra) {
                nlohmann::json entry = nlohmann::json::object();
                entry[kParentKey] = index.at(entities.get<Hierarchy>(entity).parent);
                entry[kComponentsKey] = save_components(entities, entity, registry);
                out.push_back(std::move(entry));
            }
            return out;
        }

        /// The members whose parent is no longer the one the prefab gave them.
        [[nodiscard]] nlohmann::json moved_members(
            const entt::registry& entities, const Prefab& prefab,
            const std::vector<entt::entity>& present,
            const std::unordered_map<entt::entity, int>& index) {
            // From 1, because the root's parent belongs to the scene record
            // rather than to the instance.
            nlohmann::json out = nlohmann::json::object();
            for (std::size_t i = 1; i < prefab.size(); ++i) {
                if (present[i] == entt::null) {
                    continue;
                }
                const entt::entity parent = entities.get<Hierarchy>(present[i]).parent;
                const int now = parent == entt::null ? kNoParent : index.at(parent);
                if (now != prefab.entities()[i].parent) {
                    out[std::to_string(i)] = now;
                }
            }
            return out;
        }

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

    entt::entity instantiate(World& world, const Prefab& prefab, const nlohmann::json& record,
                             const ComponentRegistry& registry) {
        if (prefab.size() == 0) {
            ENGINE_LOG_ERROR("Prefab {} holds no entity.", prefab.name());
            return entt::null;
        }
        if (!record.is_null() && !record.is_object()) {
            ENGINE_LOG_ERROR("Prefab {}: the instance record is {}, not an object.",
                             prefab.name(), record.type_name());
            return entt::null;
        }

        InstanceRecord parts;
        if (!read_instance_record(record, prefab, parts)) {
            return entt::null;
        }

        // Every entity is created before any of them is parented, so a member
        // moved under an entity the instance added works whatever order the two
        // sit in. The one forward pass the prefab format guarantees covers the
        // prefab's own parents and nothing else.
        const std::size_t total = prefab.size() + parts.added.size();
        // entt::null converts to both a size and an entity, so it is spelled
        // out here to say which one this is.
        std::vector<entt::entity> created(total, entt::entity{ entt::null });
        for (std::size_t i = 0; i < total; ++i) {
            if (!parts.gone.contains(i)) {
                created[i] = world.create();
            }
        }
        const entt::entity root = created.front();
        if (root == entt::null) {
            ENGINE_LOG_ERROR("Prefab {}: the instance removes its own root, so there is "
                             "nothing to place.",
                             prefab.name());
            return entt::null;
        }

        bool ok = true;
        for (std::size_t i = 0; i < total; ++i) {
            if (created[i] != entt::null &&
                !build_member(world, prefab, parts, registry, created, i, root)) {
                ok = false;
            }
        }

        world.registry().emplace<PrefabInstance>(root, PrefabInstance{ .prefab = prefab.name() });

        if (!ok) {
            // Destroy the list rather than the tree. Destroying the root reaches
            // every entity that attached, and walking the list then reaches any
            // that did not. World::destroy() ignores an entity that has already
            // gone, so the two do not fight.
            for (const entt::entity dead : created) {
                world.destroy(dead);
            }
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

    nlohmann::json instance_record(const World& world, entt::entity root, const Prefab& prefab,
                                   const ComponentRegistry& registry) {
        const entt::registry& entities = world.registry();

        std::unordered_map<entt::entity, int> index;
        std::vector<entt::entity> extra;
        number_instance(entities, root, prefab, index, extra);

        nlohmann::json out = nlohmann::json::object();

        nlohmann::json fields = instance_overrides(world, root, prefab, registry);
        if (!fields.empty()) {
            out[kOverridesKey] = std::move(fields);
        }

        // A member the walk did not reach is gone. That covers one that was
        // destroyed and one that was dragged out of the instance, and the two
        // read the same from here.
        std::vector<entt::entity> present(prefab.size(), entt::entity{ entt::null });
        for (const auto& [entity, at] : index) {
            if (static_cast<std::size_t>(at) < prefab.size()) {
                present[static_cast<std::size_t>(at)] = entity;
            }
        }

        if (nlohmann::json removed = removed_members(present); !removed.empty()) {
            out[kRemovedKey] = std::move(removed);
        }
        if (nlohmann::json added = added_entities(entities, extra, index, registry);
            !added.empty()) {
            out[kAddedKey] = std::move(added);
        }
        if (nlohmann::json moved = moved_members(entities, prefab, present, index);
            !moved.empty()) {
            out[kReparentedKey] = std::move(moved);
        }
        return out;
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
