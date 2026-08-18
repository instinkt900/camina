#include "scene/scene_file.h"

#include "core/log.h"
#include "scene/document.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace engine::scene {

    namespace {

        /// Whether a record carries the link back to the instance it belongs to.
        enum class MemberLinks : std::uint8_t {
            Skip,  ///< What a scene writes. No member ever gets its own record.
            Write, ///< What a fragment writes. Its root can be one member.
        };


        /**
         * Lists one subtree, parents before children, in child-list order.
         *
         * Depth first, so a parent always lands before its children and a
         * reader can attach as it goes. The order is the one the hierarchy
         * holds, which is stable across a save and a load.
         */
        std::vector<entt::entity> walk_from(const entt::registry& registry,
                                            const std::vector<entt::entity>& roots) {
            std::vector<entt::entity> ordered;
            ordered.reserve(registry.view<const Hierarchy>().size());

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

            return walk_from(registry, roots);
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
            int parent = kNoParent;
            if (!read_parent(record, "Entity " + std::to_string(self), parent)) {
                return false;
            }
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

        /**
         * Puts back the link from one entity to the instance it belongs to.
         *
         * A fragment carries this and a scene does not. See @ref kMemberKey.
         *
         * The instance root has to be in the world. A member with no instance
         * is not a member, and leaving it as a loose entity would quietly turn
         * an undo of a delete into an entity somebody has to find and remove.
         */
        bool apply_member(const nlohmann::json& record, entt::entity entity, World& world,
                          std::size_t self) {
            const auto link = record.find(kMemberKey);
            if (link == record.end()) {
                return true;
            }
            if (!link->is_object()) {
                ENGINE_LOG_ERROR("Entity {}: {} is {}, and it must be an object.", self,
                                 kMemberKey, link->type_name());
                return false;
            }

            const auto named = link->find(kRootKey);
            const auto at = link->find(kIndexKey);
            if (named == link->end() || !named->is_string() || at == link->end() ||
                !at->is_number_unsigned()) {
                ENGINE_LOG_ERROR("Entity {}: {} needs a {} of text and a whole {}.", self,
                                 kMemberKey, kRootKey, kIndexKey);
                return false;
            }

            Guid root;
            if (!Guid::parse(named->get<std::string>(), root)) {
                ENGINE_LOG_ERROR("Entity {}: {} names the identity {}, which is not one.", self,
                                 kMemberKey, named->get<std::string>());
                return false;
            }

            const entt::entity instance = world.find(root);
            if (instance == entt::null) {
                ENGINE_LOG_ERROR("Entity {} belongs to the instance {}, which is not in the "
                                 "world.",
                                 self, named->get<std::string>());
                return false;
            }

            world.registry().emplace_or_replace<PrefabMember>(
                entity, PrefabMember{ .root = instance, .index = at->get<std::size_t>() });
            return true;
        }

        /// What one record produced, and whether the reader had to complain.
        struct Built {
            entt::entity entity = entt::null; ///< The entity, or an instance root.
            bool ok = true;                   ///< False when the record named something wrong.
        };

        /**
         * Gives an entity the identity its record names.
         *
         * A record from before version 4 names none, and the entity keeps the
         * one `World::create` generated. That is what lets an older scene open,
         * and it is the only case where an identity is not the one the file
         * holds.
         *
         * @param world The world holding the entity.
         * @param entity The entity to name.
         * @param record Its record.
         * @param self Its index, for the message when the text is not a GUID.
         */
        void take_identity(World& world, entt::entity entity, const nlohmann::json& record,
                           std::size_t self) {
            if (!record.is_object()) {
                return;
            }
            const auto named = record.find(kIdKey);
            if (named == record.end() || !named->is_string()) {
                return;
            }

            Guid wanted;
            if (!Guid::parse(named->get<std::string>(), wanted)) {
                ENGINE_LOG_ERROR("Entity {} names the identity {}, which is not one. It keeps "
                                 "the one it was made with.",
                                 self, named->get<std::string>());
                return;
            }
            (void)world.set_identity(entity, wanted);
        }

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
                const entt::entity plain = world.create();
                take_identity(world, plain, record, self);
                return { .entity = plain, .ok = true };
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

            // The whole record, not just the overrides. It carries the shape
            // the instance changed as well as the fields.
            const entt::entity root = instantiate(world, *prefab, record, registry);
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

        /**
         * Picks the entities that get a record of their own, and numbers them.
         *
         * Everything inside a collapsed instance is left out, because the
         * instance record carries it: a member through the field overrides, and
         * anything else through the added list. So this walks down from each
         * collapsed root and skips the lot.
         */
        std::vector<entt::entity> choose_records(
            const entt::registry& entities, const std::vector<entt::entity>& ordered,
            const std::unordered_set<entt::entity>& collapsed,
            std::unordered_map<entt::entity, int>& index) {
            std::unordered_set<entt::entity> inside;
            for (const entt::entity root : collapsed) {
                std::vector<entt::entity> stack{ root };
                while (!stack.empty()) {
                    const entt::entity current = stack.back();
                    stack.pop_back();
                    if (current != root) {
                        inside.insert(current);
                    }
                    for (entt::entity child = entities.get<Hierarchy>(current).first_child;
                         child != entt::null;
                         child = entities.get<Hierarchy>(child).next_sibling) {
                        stack.push_back(child);
                    }
                }
            }

            std::vector<entt::entity> written;
            for (const entt::entity entity : ordered) {
                if (inside.contains(entity)) {
                    continue;
                }
                index[entity] = static_cast<int>(written.size());
                written.push_back(entity);
            }
            return written;
        }

        /**
         * Writes the link from one entity back to the instance it belongs to.
         *
         * Only a fragment needs this. A scene collapses every instance to one
         * record, so a member never gets a record of its own there and the link
         * comes back when instantiate() builds the instance again. A fragment
         * can be one member somebody deleted, and then nothing rebuilds it.
         */
        void write_member_link(const entt::registry& entities, const World& world,
                               entt::entity entity, nlohmann::json& record) {
            const auto* member = entities.try_get<PrefabMember>(entity);
            if (member == nullptr) {
                return;
            }
            record[kMemberKey] = nlohmann::json{
                { kRootKey, to_text(world.identity(member->root)) },
                { kIndexKey, member->index },
            };
        }

        /// Writes the entity array that a scene and a fragment both hold.
        nlohmann::json write_records(const World& world,
                                     const std::vector<entt::entity>& written,
                                     const std::unordered_map<entt::entity, int>& index,
                                     const std::unordered_set<entt::entity>& collapsed,
                                     const ComponentRegistry& registry,
                                     const PrefabLibrary& library, MemberLinks links) {
            const entt::registry& entities = world.registry();

            nlohmann::json list = nlohmann::json::array();
            for (const entt::entity entity : written) {
                nlohmann::json record = nlohmann::json::object();

                // A parent that is not in the index is outside what is being
                // written. That is a root of the world for a scene, and the
                // entity the fragment hung under for a fragment.
                const Hierarchy& node = entities.get<Hierarchy>(entity);
                const auto parent = index.find(node.parent);
                record[kParentKey] = parent == index.end() ? kNoParent : parent->second;

                // The identity, so an entity built again from this file answers
                // to the same one and an undo entry naming it still reaches it.
                record[kIdKey] = to_text(world.identity(entity));

                if (collapsed.contains(entity)) {
                    const PrefabInstance& link = entities.get<PrefabInstance>(entity);
                    const Prefab* prefab = library.find(link.prefab);
                    record[kPrefabKey] = link.prefab;

                    // collapsible() already found the prefab, so this cannot
                    // fail. The record carries the fields the instance changed
                    // and the shape it changed, and each key is left out when it
                    // is empty. Named, not a temporary. A range-for over a
                    // temporary's items() reads a document that is already gone.
                    nlohmann::json body = instance_record(world, entity, *prefab, registry);
                    record.update(std::move(body));
                } else {
                    if (links == MemberLinks::Write) {
                        write_member_link(entities, world, entity, record);
                    }
                    record[kComponentsKey] = save_components(entities, entity, registry);
                }

                list.push_back(std::move(record));
            }
            return list;
        }

        /// Where a fragment hung: the parent, and the sibling it sat in front of.
        struct Anchor {
            entt::entity parent = entt::null; ///< What it hung under, or null for a root.
            entt::entity before = entt::null; ///< What it sat in front of, or null for last.
        };

        /// Reads one identity key off a fragment and finds the entity it names.
        bool read_anchor_key(const nlohmann::json& document, const World& world, const char* key,
                             entt::entity& out) {
            const auto found = document.find(key);
            if (found == document.end()) {
                return true;
            }
            if (!found->is_string()) {
                ENGINE_LOG_ERROR("A fragment holds a {} of {}, and it must be text.", key,
                                 found->type_name());
                return false;
            }

            Guid id;
            if (!Guid::parse(found->get<std::string>(), id)) {
                ENGINE_LOG_ERROR("A fragment names {} {}, which is not an identity.", key,
                                 found->get<std::string>());
                return false;
            }

            out = world.find(id);
            if (out == entt::null) {
                ENGINE_LOG_ERROR("A fragment hangs {} {}, which is not in the world.", key,
                                 found->get<std::string>());
                return false;
            }
            return true;
        }

        /// Reads where a fragment hung, and checks the pair makes sense.
        bool read_anchor(const nlohmann::json& document, const World& world, Anchor& out) {
            if (!read_anchor_key(document, world, kUnderKey, out.parent) ||
                !read_anchor_key(document, world, kBeforeKey, out.before)) {
                return false;
            }
            if (out.parent == entt::null && out.before != entt::null) {
                // The roots of a world come out sorted by entity value, so a
                // root has no position among its siblings to ask for. Refused
                // rather than dropped, because dropping it says the fragment
                // went back where it was when it did not.
                ENGINE_LOG_ERROR("A fragment names a {} and no {}. A root has no place among "
                                 "its siblings.",
                                 kBeforeKey, kUnderKey);
                return false;
            }
            return true;
        }

        /// The first pass: one entity for each record, each with its identity.
        std::vector<entt::entity> build_all(const nlohmann::json& list, World& world,
                                            const ComponentRegistry& registry,
                                            const PrefabLibrary& library, bool& ok) {
            std::vector<entt::entity> created;
            created.reserve(list.size());
            for (std::size_t i = 0; i < list.size(); ++i) {
                const Built built = build_entity(list[i], i, world, registry, library);
                created.push_back(built.entity);
                ok = built.ok && ok;
            }
            return created;
        }

        /// The second pass: the parent link, the components, and the instance link.
        bool link_all(const nlohmann::json& list, const std::vector<entt::entity>& created,
                      World& world, const ComponentRegistry& registry, MemberLinks links) {
            bool ok = true;
            for (std::size_t i = 0; i < list.size(); ++i) {
                const nlohmann::json& record = list[i];
                if (!record.is_object()) {
                    ENGINE_LOG_ERROR("Entity {} is not an object.", i);
                    ok = false;
                    continue;
                }

                ok = apply_parent(record, i, created, world) && ok;
                ok = apply_components(record, i, created[i], world, registry) && ok;
                if (links == MemberLinks::Write) {
                    ok = apply_member(record, created[i], world, i) && ok;
                }
            }
            return ok;
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
        out[kEntitiesKey] = write_records(world, written, index, collapsed, registry, library,
                                          MemberLinks::Skip);
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
        const std::vector<entt::entity> created =
            build_all(*list, world, registry, library, ok);
        return link_all(*list, created, world, registry, MemberLinks::Skip) && ok;
    }

    nlohmann::json save_subtree(const World& world, entt::entity root,
                                const ComponentRegistry& registry,
                                const PrefabLibrary& library) {
        const entt::registry& entities = world.registry();
        if (root == entt::null || !entities.valid(root) || !entities.all_of<Hierarchy>(root)) {
            ENGINE_LOG_ERROR("save_subtree was given an entity that is not in the world.");
            return {};
        }

        const std::vector<entt::entity> ordered = walk_from(entities, { root });
        const std::unordered_set<entt::entity> collapsed =
            collapsible(entities, ordered, library);

        std::unordered_map<entt::entity, int> index;
        index.reserve(ordered.size());
        const std::vector<entt::entity> written =
            choose_records(entities, ordered, collapsed, index);

        nlohmann::json out = nlohmann::json::object();
        out[kVersionKey] = kSceneVersion;
        out[kEntitiesKey] = write_records(world, written, index, collapsed, registry, library,
                                          MemberLinks::Write);

        // Where it hung. Both keys are left out when there is nothing to say,
        // so a fragment taken from a root of the world carries neither.
        const Hierarchy& node = entities.get<Hierarchy>(root);
        if (node.parent != entt::null) {
            out[kUnderKey] = to_text(world.identity(node.parent));
        }
        if (node.next_sibling != entt::null) {
            out[kBeforeKey] = to_text(world.identity(node.next_sibling));
        }
        return out;
    }

    entt::entity load_subtree(const nlohmann::json& document, World& world,
                              const ComponentRegistry& registry, const PrefabLibrary& library) {
        const nlohmann::json* list = nullptr;
        if (!read_header(document, &list)) {
            return entt::null;
        }
        if (list->empty()) {
            ENGINE_LOG_ERROR("A fragment must hold at least the entity it is rooted at.");
            return entt::null;
        }

        // The anchor is read before anything is built, so a fragment whose
        // parent has gone leaves the world untouched rather than half changed.
        Anchor anchor;
        if (!read_anchor(document, world, anchor)) {
            return entt::null;
        }

        bool ok = true;
        const std::vector<entt::entity> created =
            build_all(*list, world, registry, library, ok);
        ok = link_all(*list, created, world, registry, MemberLinks::Write) && ok;

        // walk_from() puts the root of the subtree first, and choose_records()
        // keeps that order.
        const entt::entity root = created.front();
        if (ok && anchor.parent != entt::null &&
            !world.set_parent(root, anchor.parent, anchor.before)) {
            ok = false;
        }

        if (!ok) {
            // The list rather than the tree. Destroying the root reaches every
            // entity that attached, and walking the list then reaches any that
            // did not. World::destroy() ignores an entity that has already gone.
            for (const entt::entity dead : created) {
                world.destroy(dead);
            }
            return entt::null;
        }
        return root;
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
