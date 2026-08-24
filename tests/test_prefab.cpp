// M3.3 tests. A prefab is a scene fragment, and an instance keeps only the
// fields it changed.
//
// The check that carries the milestone is test_prefab_edit_reaches_instances().
// It changes the prefab under two instances and shows that the one which
// overrode a field keeps that field, and takes every other change.

#include "check.h"
#include "math/transform.h"
#include "reflect/json.h"
#include "scene/component_registry.h"
#include "scene/components.h"
#include "scene/prefab.h"
#include "scene/scene_file.h"
#include "scene/world.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

    using test::check;
    namespace sc = engine::scene;

    sc::ComponentRegistry make_registry() {
        sc::ComponentRegistry registry;
        sc::register_builtin_components(registry);
        return registry;
    }

    /// A crate with a lid on it: one root, one child, two components each.
    nlohmann::json crate_document(const std::string& crate_name, float crate_scale) {
        nlohmann::json root = nlohmann::json::object();
        root["parent"] = -1;
        root["components"]["Name"] = engine::reflect::to_json(sc::Name{ crate_name });
        root["components"]["Transform"] = engine::reflect::to_json(
            engine::Transform{ .scale = { crate_scale, crate_scale, crate_scale } });

        nlohmann::json lid = nlohmann::json::object();
        lid["parent"] = 0;
        lid["components"]["Name"] = engine::reflect::to_json(sc::Name{ "lid" });
        lid["components"]["Transform"] =
            engine::reflect::to_json(engine::Transform{ .position = { 0.0F, 1.0F, 0.0F } });

        nlohmann::json document = nlohmann::json::object();
        document["__version"] = sc::kPrefabVersion;
        document["entities"] = nlohmann::json::array({ root, lid });
        return document;
    }

    /**
     * The library key stays "crate" whatever the arguments say.
     *
     * test_prefab_edit_reaches_instances() saves a scene against one crate and
     * loads it against an edited crate, and that only works while both live
     * under the same key. `crate_name` sets the Name component value, which is
     * one of the fields the edit changes.
     */
    sc::PrefabLibrary crate_library(const std::string& crate_name = "crate",
                                    float crate_scale = 1.0F) {
        sc::PrefabLibrary library;
        check(library.add("crate", crate_document(crate_name, crate_scale)),
              "the crate prefab parses");
        return library;
    }

    /**
     * The crate prefab, or a hard stop.
     *
     * check() records a failure and carries on, so a null dereference here would
     * take the rest of the run with it and CI would lose every later result.
     */
    const sc::Prefab& crate_of(const sc::PrefabLibrary& library) {
        const sc::Prefab* found = library.find("crate");
        if (found == nullptr) {
            check(false, "the library holds the crate prefab");
            std::printf("Cannot go on without the prefab.\n");
            std::exit(test::report());
        }
        return *found;
    }

    /// An instance record whose only change is where the root sits.
    nlohmann::json moved_to(float x) {
        nlohmann::json record = nlohmann::json::object();
        record["overrides"]["0"]["Transform"]["position"] =
            nlohmann::json::array({ x, 0.0F, 0.0F });
        return record;
    }

    void test_parse() {
        sc::Prefab prefab;
        check(sc::Prefab::parse("crate", crate_document("crate", 1.0F), prefab),
              "a good document parses");
        check(prefab.name() == "crate", "the prefab keeps its name");
        check(prefab.size() == 2, "the prefab holds two entities");
        check(prefab.entities().at(0).parent == -1, "entity 0 is the root");
        check(prefab.entities().at(1).parent == 0, "entity 1 hangs off the root");

        check(!sc::Prefab::parse("bad", nlohmann::json::array(), prefab),
              "an array is not a prefab");
        // The header promises the target is untouched when the read fails, and
        // every negative case below reuses this one object.
        check(prefab.name() == "crate" && prefab.size() == 2,
              "a refused parse leaves the target prefab alone");

        nlohmann::json no_version = crate_document("crate", 1.0F);
        no_version.erase("__version");
        check(!sc::Prefab::parse("bad", no_version, prefab), "a prefab must carry a version");

        nlohmann::json future = crate_document("crate", 1.0F);
        future["__version"] = sc::kPrefabVersion + 1;
        check(!sc::Prefab::parse("bad", future, prefab), "a newer schema is refused");

        nlohmann::json no_entities = nlohmann::json::object();
        no_entities["__version"] = sc::kPrefabVersion;
        check(!sc::Prefab::parse("bad", no_entities, prefab),
              "a prefab must carry an entity array");

        nlohmann::json empty = no_entities;
        empty["entities"] = nlohmann::json::array();
        check(!sc::Prefab::parse("bad", empty, prefab), "a prefab needs a root");

        nlohmann::json parented_root = crate_document("crate", 1.0F);
        parented_root["entities"][0]["parent"] = 1;
        check(!sc::Prefab::parse("bad", parented_root, prefab),
              "the first entity must be the root");

        nlohmann::json second_root = crate_document("crate", 1.0F);
        second_root["entities"][1]["parent"] = -1;
        check(!sc::Prefab::parse("bad", second_root, prefab), "a prefab holds one root only");

        nlohmann::json forward = crate_document("crate", 1.0F);
        forward["entities"][1]["parent"] = 5;
        check(!sc::Prefab::parse("bad", forward, prefab),
              "a parent must be an earlier entity");

        nlohmann::json bad_entity = crate_document("crate", 1.0F);
        bad_entity["entities"][1] = 7;
        check(!sc::Prefab::parse("bad", bad_entity, prefab), "an entity must be an object");

        nlohmann::json bad_parts = crate_document("crate", 1.0F);
        bad_parts["entities"][0]["components"] = 7;
        check(!sc::Prefab::parse("bad", bad_parts, prefab), "components must be an object");

        // nlohmann::json::value() converts to the type of the default, and it
        // throws when the stored type cannot convert. Nothing here catches a
        // JSON exception, so a parent of the wrong type used to end the process.
        nlohmann::json worded_parent = crate_document("crate", 1.0F);
        worded_parent["entities"][1]["parent"] = "root";
        check(!sc::Prefab::parse("bad", worded_parent, prefab),
              "a parent that is not a number is refused rather than thrown");

        nlohmann::json null_parent = crate_document("crate", 1.0F);
        null_parent["entities"][1]["parent"] = nullptr;
        check(!sc::Prefab::parse("bad", null_parent, prefab), "a null parent is refused");

        nlohmann::json fractional_parent = crate_document("crate", 1.0F);
        fractional_parent["entities"][1]["parent"] = 0.5;
        check(!sc::Prefab::parse("bad", fractional_parent, prefab),
              "a parent with a fraction is refused");
    }

    void test_library() {
        sc::PrefabLibrary library;
        check(library.size() == 0, "a new library is empty");
        check(library.find("crate") == nullptr, "an empty library finds nothing");

        check(library.add("crate", crate_document("crate", 1.0F)), "a prefab goes in");
        check(library.size() == 1, "the library holds one prefab");
        check(library.find("crate") != nullptr, "the prefab is findable by name");

        // A second prefab under the same name replaces the first, so a reload
        // needs no remove call.
        check(library.add("crate", crate_document("heavy crate", 1.0F)), "a reload goes in");
        check(library.size() == 1, "a reload replaces rather than adds");
        check(crate_of(library).entities().at(0).components.at("Name").at("value") ==
                  "heavy crate",
              "the reload won");

        check(!library.add("broken", nlohmann::json::array()), "a bad document is refused");
        check(library.size() == 1, "a refused document leaves the library alone");

        library.clear();
        check(library.size() == 0, "clear empties the library");
    }

    /**
     * Reads a prefab from a loose file rather than from a document.
     *
     * add_file() lost its last caller when prefab registration became
     * data-driven, so nothing exercised a path that opens a file from disk.
     * The function stays because an editor opens a prefab a person just put in
     * a directory, and no cooker has run over it yet. That is M9 work, and a
     * public function with no caller and no test breaks quietly in the
     * meantime. See issue #185.
     */
    void test_add_file() {
        const std::filesystem::path dir = test::scratch("prefab", "add_file");

        const std::filesystem::path good = dir / "crate.prefab";
        {
            std::ofstream file(good, std::ios::binary | std::ios::trunc);
            file << crate_document("loose crate", 1.0F).dump();
        }

        sc::PrefabLibrary library;
        check(library.add_file("crate", good), "a prefab reads from a file");
        check(library.size() == 1, "and it goes into the library");
        check(crate_of(library).entities().at(0).components.at("Name").at("value") ==
                  "loose crate",
              "and it holds what the file held");

        // Counting entries is not enough on its own. A refusal that dropped the
        // crate and left something else behind would keep the count at one, and
        // so would one that overwrote the crate in place. So each case below
        // names the prefab and reads a field out of it.
        const auto crate_survived = [&library](const char* what) {
            const sc::Prefab* found = library.find("crate");
            check(found != nullptr && library.size() == 1, what);
            check(found != nullptr && found->size() == 2 &&
                      found->entities().at(0).components.at("Name").at("value") == "loose crate",
                  "and the prefab it holds is untouched");
        };

        // A file that is not there is the case an editor meets most, because a
        // person can rename or move one while the editor holds the name.
        check(!library.add_file("missing", dir / "not_there.prefab"),
              "a file that is not there is refused");
        crate_survived("and the library still holds the crate alone");

        // Exceptions are off, per reflect/json.h, so a bad document comes back
        // discarded rather than thrown. Without this the parse would end the
        // process instead of reporting.
        const std::filesystem::path broken = dir / "broken.prefab";
        {
            std::ofstream file(broken, std::ios::binary | std::ios::trunc);
            file << "{ this is not json";
        }
        check(!library.add_file("broken", broken), "a file that is not JSON is refused");
        crate_survived("and that leaves the crate alone too");

        // A document that parses but is not a prefab has to fail in add(),
        // which is the half add_file() delegates to.
        const std::filesystem::path wrong = dir / "wrong.prefab";
        {
            std::ofstream file(wrong, std::ios::binary | std::ios::trunc);
            file << "[]";
        }
        check(!library.add_file("wrong", wrong), "valid JSON that is not a prefab is refused");
        crate_survived("and the crate is still the one that worked");

        // A refusal under a name the library already holds is the one that could
        // overwrite. add() replaces on a name it knows, so this says the replace
        // does not happen until the document is good.
        check(!library.add_file("crate", wrong),
              "a refused document does not replace the prefab of the same name");
        crate_survived("and the crate that was already there is the one still there");

        test::remove_tree(dir);
    }

    void test_override_patch() {
        const nlohmann::json base = { { "a", 1 }, { "b", 2 } };

        check(sc::override_patch(base, base).empty(), "two equal documents need no patch");

        const nlohmann::json changed = { { "a", 1 }, { "b", 9 } };
        const nlohmann::json patch = sc::override_patch(base, changed);
        check(patch.size() == 1 && patch["b"] == 9, "a patch names only the key that changed");

        const nlohmann::json added = { { "a", 1 }, { "b", 2 }, { "c", 3 } };
        check(sc::override_patch(base, added) == nlohmann::json{ { "c", 3 } },
              "a new key goes into the patch");

        const nlohmann::json dropped = { { "a", 1 } };
        const nlohmann::json removal = sc::override_patch(base, dropped);
        check(removal.size() == 1 && removal["b"].is_null(),
              "a dropped key becomes a null, which a merge patch reads as remove");

        // The walk goes into a nested object, so two components that differ in
        // one field give a patch that names that one field.
        const nlohmann::json outer = { { "T", { { "x", 1 }, { "y", 2 } } } };
        const nlohmann::json moved = { { "T", { { "x", 1 }, { "y", 5 } } } };
        const nlohmann::json deep = sc::override_patch(outer, moved);
        check(deep == nlohmann::json{ { "T", { { "y", 5 } } } },
              "a nested change gives a nested patch");

        // An array is a value. Half a position is not a useful override.
        const nlohmann::json list = { { "p", { 1, 2, 3 } } };
        const nlohmann::json other = { { "p", { 1, 9, 3 } } };
        check(sc::override_patch(list, other) == nlohmann::json{ { "p", { 1, 9, 3 } } },
              "an array goes into the patch whole");
    }

    /**
     * A member's identity is derived from its instance root.
     *
     * An instance is one record in a scene file, so the members cannot each
     * carry a stored identity without bloating every record. They derive one,
     * the way the cooker derives a mesh identity from the glTF that holds it.
     * Two things have to hold: two instances of one prefab must not collide,
     * and the same instance must come back with the same identities every time.
     */
    void test_member_identities_are_derived() {
        const sc::ComponentRegistry registry = make_registry();
        const sc::PrefabLibrary library = crate_library();
        const sc::Prefab& crate = crate_of(library);

        sc::World world;
        const entt::entity first = sc::instantiate(world, crate, nlohmann::json::object(),
                                                   registry);
        const entt::entity second = sc::instantiate(world, crate, nlohmann::json::object(),
                                                    registry);
        check(first != entt::null && second != entt::null, "two instances build");

        const engine::Guid first_id = world.identity(first);
        const engine::Guid second_id = world.identity(second);
        check(first_id.valid() && second_id.valid(), "each root has an identity");
        check(first_id != second_id, "and the two roots differ");

        // The lid of each instance derives from its own root, so the two lids
        // are different things even though they come from one prefab entity.
        const auto lid_of = [&world](entt::entity root) -> entt::entity {
            for (const auto [entity, member] :
                 world.registry().view<const sc::PrefabMember>().each()) {
                if (member.root == root && entity != root) {
                    return entity;
                }
            }
            return entt::null;
        };

        const entt::entity first_lid = lid_of(first);
        const entt::entity second_lid = lid_of(second);
        check(first_lid != entt::null && second_lid != entt::null, "each instance has a lid");
        check(world.identity(first_lid) != world.identity(second_lid),
              "and the two lids do not collide");
        check(world.find(world.identity(first_lid)) == first_lid,
              "a member is found by its identity");

        // Through a scene file: the same instance comes back with the same
        // identities, root and member both.
        const nlohmann::json document = sc::save_scene(world, registry, library);
        sc::World loaded;
        check(sc::load_scene(document, loaded, registry, library), "the scene loads again");

        const entt::entity first_again = loaded.find(first_id);
        check(first_again != entt::null, "the first root came back under its identity");
        check(loaded.find(second_id) != entt::null, "and so did the second");
        check(loaded.find(world.identity(first_lid)) != entt::null,
              "the derived identity of a member came back too");

        // The root is a member of its own instance, at index 0. Deriving over
        // it would leave the identity on some other entity, or on none, so the
        // check is that this identity still names the instance root.
        check(first_again != entt::null &&
                  loaded.registry().all_of<sc::PrefabInstance>(first_again),
              "and that identity still names the instance root");
    }

    void test_instantiate() {
        const sc::ComponentRegistry registry = make_registry();
        const sc::PrefabLibrary library = crate_library();
        const sc::Prefab& crate = crate_of(library);

        sc::World world;
        const entt::entity root = sc::instantiate(world, crate, nlohmann::json::object(),
                                                  registry);
        check(root != entt::null, "the instance builds");
        check(world.size() == 2, "the instance created both entities");

        const entt::registry& entities = world.registry();
        check(entities.all_of<sc::PrefabInstance>(root), "the root carries the link");
        check(entities.get<sc::PrefabInstance>(root).prefab == "crate", "the link names the prefab");
        check(entities.get<sc::Name>(root).value == "crate", "the root took its name");

        const entt::entity lid = entities.get<sc::Hierarchy>(root).first_child;
        check(lid != entt::null, "the child is attached");
        check(!entities.all_of<sc::PrefabInstance>(lid), "only the root carries the link");
        check(entities.get<sc::Name>(lid).value == "lid", "the child took its name");
        check(entities.get<sc::PrefabMember>(lid).root == root, "the child points at its root");
        check(entities.get<sc::PrefabMember>(lid).index == 1, "the child knows its index");
        check(entities.get<sc::PrefabMember>(root).index == 0, "the root knows its index");

        // The prefab drives the world matrix, so the child ends up above the root.
        world.update();
        check(world.world_matrix(lid)[3][1] == 1.0F, "the child sits where the prefab put it");
    }

    void test_overrides_are_per_field() {
        const sc::ComponentRegistry registry = make_registry();
        const sc::PrefabLibrary library = crate_library("crate", 3.0F);
        const sc::Prefab& crate = crate_of(library);

        sc::World world;
        const entt::entity root = sc::instantiate(world, crate, moved_to(7.0F), registry);
        check(root != entt::null, "an overriding instance builds");

        const engine::Transform& local = world.local(root);
        check(local.position.x == 7.0F, "the overridden field took the new value");
        check(local.scale.x == 3.0F, "the field next to it still comes from the prefab");
        check(world.registry().get<sc::Name>(root).value == "crate",
              "a component the patch did not name still comes from the prefab");

        // Reading the overrides back must name that one field and nothing else.
        const nlohmann::json found = sc::instance_record(world, root, crate, registry);
        check(found == moved_to(7.0F), "the instance reports the one field it changed");

        sc::World clean;
        const entt::entity plain =
            sc::instantiate(clean, crate, nlohmann::json::object(), registry);
        check(sc::instance_overrides(clean, plain, crate, registry).empty(),
              "an instance that changed nothing reports nothing");
    }

    void test_instantiate_refuses_bad_input() {
        const sc::ComponentRegistry registry = make_registry();
        const sc::PrefabLibrary library = crate_library();
        const sc::Prefab& crate = crate_of(library);

        sc::World world;
        check(sc::instantiate(world, crate, nlohmann::json::array(), registry) == entt::null,
              "an instance record that is not an object is refused");
        check(world.size() == 0, "a refused build leaves nothing behind");

        // A patch that puts the wrong type in a field fails the component read.
        nlohmann::json wrong = nlohmann::json::object();
        wrong["overrides"]["0"]["Transform"]["position"] = "over there";
        check(sc::instantiate(world, crate, wrong, registry) == entt::null,
              "a field of the wrong type fails the build");
        check(world.size() == 0, "a failed build destroys the whole instance");
    }

    void test_unknown_component_is_a_warning() {
        // A build that never heard of a component in the prefab must still
        // instantiate it, in the same way a scene file tolerates one.
        nlohmann::json document = crate_document("crate", 1.0F);
        document["entities"][0]["components"]["Health"] = { { "current", 5 } };

        sc::PrefabLibrary library;
        check(library.add("crate", document), "the prefab parses");

        sc::World world;
        const entt::entity root = sc::instantiate(world, crate_of(library),
                                                  nlohmann::json::object(), make_registry());
        check(root != entt::null, "an unknown component does not fail the build");
        check(world.registry().get<sc::Name>(root).value == "crate",
              "the components it does know all arrived");
    }

    /// Builds a scene of two crates. The second one is moved, the first is not.
    nlohmann::json two_crates(const sc::ComponentRegistry& registry,
                              const sc::PrefabLibrary& library) {
        sc::World world;
        const sc::Prefab& crate = crate_of(library);

        check(sc::instantiate(world, crate, nlohmann::json::object(), registry) != entt::null,
              "the plain crate builds");
        check(sc::instantiate(world, crate, moved_to(5.0F), registry) != entt::null,
              "the moved crate builds");

        return sc::save_scene(world, registry, library);
    }

    /// The instance roots of a loaded scene, in file order.
    std::vector<entt::entity> instance_roots(const sc::World& world) {
        std::vector<entt::entity> found;
        for (const auto [entity, link] : world.registry().view<const sc::PrefabInstance>().each()) {
            (void)link;
            found.push_back(entity);
        }
        std::sort(found.begin(), found.end(), [](entt::entity left, entt::entity right) {
            return entt::to_integral(left) < entt::to_integral(right);
        });
        return found;
    }

    // Issue #27. An instance can change the shape of what it built, not only
    // the fields. Each of these makes one such change in a live world, saves,
    // loads, and checks the change is still there.

    /// The entity under @p parent whose Name is @p wanted, or entt::null.
    entt::entity child_named(const sc::World& world, entt::entity parent,
                             std::string_view wanted) {
        const entt::registry& entities = world.registry();
        for (entt::entity child = entities.get<sc::Hierarchy>(parent).first_child;
             child != entt::null;
             child = entities.get<sc::Hierarchy>(child).next_sibling) {
            const auto* name = entities.try_get<sc::Name>(child);
            if (name != nullptr && name->value == wanted) {
                return child;
            }
        }
        return entt::null;
    }

    /// How many entities hang under @p parent, counting only one level.
    std::size_t child_count(const sc::World& world, entt::entity parent) {
        const entt::registry& entities = world.registry();
        std::size_t count = 0;
        for (entt::entity child = entities.get<sc::Hierarchy>(parent).first_child;
             child != entt::null;
             child = entities.get<sc::Hierarchy>(child).next_sibling) {
            ++count;
        }
        return count;
    }

    /// Saves a world and loads it back into a fresh one.
    sc::World round_trip(const sc::World& world, const sc::ComponentRegistry& registry,
                         const sc::PrefabLibrary& library, nlohmann::json& document) {
        document = sc::save_scene(world, registry, library);
        sc::World loaded;
        check(sc::load_scene(document, loaded, registry, library), "the scene loads back");
        return loaded;
    }

    void test_an_added_child_survives() {
        const sc::ComponentRegistry registry = make_registry();
        const sc::PrefabLibrary library = crate_library();

        sc::World world;
        const entt::entity root =
            sc::instantiate(world, crate_of(library), nlohmann::json::object(), registry);
        check(root != entt::null, "the crate builds");

        // Hang a lamp off the crate. This is ordinary scene authoring, and
        // before issue #27 the entity vanished on the next save with a warning
        // as the only trace.
        const entt::entity lamp = world.create();
        world.registry().emplace<sc::Name>(lamp, sc::Name{ "lamp" });
        world.set_local(lamp, engine::Transform{ .position = { 0.0F, 2.0F, 0.0F } });
        check(world.set_parent(lamp, root), "the lamp attaches to the crate");

        nlohmann::json document;
        const sc::World loaded = round_trip(world, registry, library, document);

        check(document["entities"].size() == 1,
              "the instance is still one record, so the lamp went inside it");
        check(document["entities"][0]["added"].size() == 1, "the record holds one addition");

        const std::vector<entt::entity> roots = instance_roots(loaded);
        check(roots.size() == 1, "the loaded world holds the instance");
        if (roots.size() != 1) {
            return;
        }
        const entt::entity back = child_named(loaded, roots.front(), "lamp");
        check(back != entt::null, "the lamp came back");
        if (back == entt::null) {
            return;
        }
        check(loaded.local(back).position.y == 2.0F, "and it kept where it was put");
        check(child_count(loaded, roots.front()) == 2, "the crate holds its lid and the lamp");
        // The lamp is scene data, so its record carries its identity and the
        // identity does not move on the first save. A derived one would, and
        // then an undo entry naming the lamp would reach nothing.
        check(loaded.identity(back) == world.identity(lamp),
              "and it kept the identity it was made with");
    }

    void test_an_addition_keeps_its_identity_through_two_trips() {
        const sc::ComponentRegistry registry = make_registry();
        const sc::PrefabLibrary library = crate_library();

        sc::World world;
        const entt::entity root =
            sc::instantiate(world, crate_of(library), nlohmann::json::object(), registry);
        check(root != entt::null, "the crate builds");

        const entt::entity lamp = world.create();
        world.registry().emplace<sc::Name>(lamp, sc::Name{ "lamp" });
        check(world.set_parent(lamp, root), "the lamp attaches to the crate");
        const engine::Guid wanted = world.identity(lamp);

        nlohmann::json first;
        const sc::World once = round_trip(world, registry, library, first);
        nlohmann::json second;
        const sc::World twice = round_trip(once, registry, library, second);

        check(once.find(wanted) != entt::null, "the lamp came back under its identity");
        check(twice.find(wanted) != entt::null, "and it still does after a second trip");
        check(first == second, "and the two documents match");
    }

    void test_a_destroyed_member_stays_destroyed() {
        const sc::ComponentRegistry registry = make_registry();
        const sc::PrefabLibrary library = crate_library();

        sc::World world;
        const entt::entity root =
            sc::instantiate(world, crate_of(library), nlohmann::json::object(), registry);
        check(root != entt::null, "the crate builds");

        const entt::entity lid = child_named(world, root, "lid");
        check(lid != entt::null, "the crate has a lid");
        if (lid == entt::null) {
            return;
        }
        world.destroy(lid);
        check(child_count(world, root) == 0, "the lid is gone");

        nlohmann::json document;
        const sc::World loaded = round_trip(world, registry, library, document);
        check(document["entities"][0]["removed"] == nlohmann::json::array({ 1 }),
              "the record names the member it destroyed");

        const std::vector<entt::entity> roots = instance_roots(loaded);
        check(roots.size() == 1, "the loaded world holds the instance");
        if (roots.size() != 1) {
            return;
        }
        // The whole point. Without a record of the removal the prefab builds
        // the lid again, so a destroyed member comes back every load.
        check(child_count(loaded, roots.front()) == 0, "and the lid stayed destroyed");
    }

    void test_a_moved_member_stays_where_it_was_put() {
        const sc::ComponentRegistry registry = make_registry();
        const sc::PrefabLibrary library = crate_library();

        sc::World world;
        const entt::entity root =
            sc::instantiate(world, crate_of(library), nlohmann::json::object(), registry);
        check(root != entt::null, "the crate builds");

        const entt::entity lid = child_named(world, root, "lid");
        const entt::entity lamp = world.create();
        world.registry().emplace<sc::Name>(lamp, sc::Name{ "lamp" });
        check(world.set_parent(lamp, root), "the lamp attaches to the crate");
        check(lid != entt::null && world.set_parent(lid, lamp),
              "the lid moves under the lamp");

        nlohmann::json document;
        const sc::World loaded = round_trip(world, registry, library, document);
        check(document["entities"][0].contains("reparented"),
              "the record says a member moved");

        const std::vector<entt::entity> roots = instance_roots(loaded);
        check(roots.size() == 1, "the loaded world holds the instance");
        if (roots.size() != 1) {
            return;
        }
        // A member moved under an entity the instance added is the case that
        // needs every entity built before any is attached. The added lamp has
        // a higher index than the lid it now holds.
        const entt::entity back = child_named(loaded, roots.front(), "lamp");
        check(back != entt::null, "the lamp came back");
        if (back == entt::null) {
            return;
        }
        check(child_named(loaded, back, "lid") != entt::null, "and the lid is under it");
        check(child_count(loaded, roots.front()) == 1, "the crate holds only the lamp");
    }

    void test_removing_a_parent_removes_what_hangs_under_it() {
        const sc::ComponentRegistry registry = make_registry();
        const sc::PrefabLibrary library = crate_library();

        // A record somebody edited by hand. It removes the lid and adds an
        // entity under the lid in the same breath, which a live world could
        // never produce. Building the addition would attach it to an entity
        // that does not exist.
        nlohmann::json record = nlohmann::json::object();
        record["removed"] = nlohmann::json::array({ 1 });
        nlohmann::json added = nlohmann::json::object();
        added["parent"] = 1;
        added["components"]["Name"] = engine::reflect::to_json(sc::Name{ "hanger" });
        record["added"] = nlohmann::json::array({ added });

        sc::World world;
        const entt::entity root = sc::instantiate(world, crate_of(library), record, registry);
        check(root != entt::null, "the instance still builds");
        check(world.size() == 1, "and only the root is left");
        check(child_count(world, root) == 0, "nothing hangs off a member that is gone");
    }

    void test_a_record_that_will_not_read_is_refused() {
        const sc::ComponentRegistry registry = make_registry();
        const sc::PrefabLibrary library = crate_library();
        const sc::Prefab& crate = crate_of(library);

        const auto refused = [&](const nlohmann::json& record, const char* what) {
            sc::World world;
            check(sc::instantiate(world, crate, record, registry) == entt::null, what);
            check(world.size() == 0, "and it leaves nothing behind");
        };

        nlohmann::json past_the_end = nlohmann::json::object();
        past_the_end["removed"] = nlohmann::json::array({ 9 });
        refused(past_the_end, "removing an entity the prefab does not hold is refused");

        nlohmann::json no_root = nlohmann::json::object();
        no_root["removed"] = nlohmann::json::array({ 0 });
        refused(no_root, "removing the instance root is refused");

        nlohmann::json forward = nlohmann::json::object();
        nlohmann::json entry = nlohmann::json::object();
        entry["parent"] = 5;
        forward["added"] = nlohmann::json::array({ entry });
        refused(forward, "an addition whose parent comes later is refused");

        nlohmann::json stray = nlohmann::json::object();
        stray["reparented"]["1"] = 7;
        refused(stray, "moving a member under an entity that is not there is refused");

        nlohmann::json root_moved = nlohmann::json::object();
        root_moved["reparented"]["0"] = 1;
        refused(root_moved, "moving the instance root is refused");
    }

    void test_scene_collapses_an_instance() {
        const sc::ComponentRegistry registry = make_registry();
        const sc::PrefabLibrary library = crate_library();
        const nlohmann::json document = two_crates(registry, library);

        check(document["__version"] == sc::kSceneVersion, "the scene carries the current version");
        check(document["entities"].size() == 2,
              "two instances of a two-entity prefab give two records, not four");

        const nlohmann::json& plain = document["entities"][0];
        check(plain["prefab"] == "crate", "a record names its prefab");
        check(!plain.contains("components"),
              "an instance stores no components of its own");
        check(!plain.contains("overrides"),
              "an instance that changed nothing stores no overrides");

        check(document["entities"][1]["overrides"] == moved_to(5.0F)["overrides"],
              "the moved instance stores the one field it changed");
    }

    void test_scene_round_trip() {
        const sc::ComponentRegistry registry = make_registry();
        const sc::PrefabLibrary library = crate_library();
        const nlohmann::json first = two_crates(registry, library);

        sc::World loaded;
        check(sc::load_scene(first, loaded, registry, library), "the scene loads");
        check(loaded.size() == 4, "both instances expanded again");

        const nlohmann::json second = sc::save_scene(loaded, registry, library);
        check(first == second, "saving what was loaded gives the same document");

        const std::vector<entt::entity> roots = instance_roots(loaded);
        check(roots.size() == 2, "the world holds two instances");
        check(loaded.local(roots.at(0)).position.x == 0.0F, "the plain crate is at the origin");
        check(loaded.local(roots.at(1)).position.x == 5.0F, "the moved crate kept its override");
    }

    void test_prefab_edit_reaches_instances() {
        const sc::ComponentRegistry registry = make_registry();

        // Save a scene against the first version of the prefab.
        const sc::PrefabLibrary before = crate_library("crate", 1.0F);
        const nlohmann::json document = two_crates(registry, before);

        // Now the prefab changes: a new name and a new scale. Neither instance
        // ever named those fields.
        const sc::PrefabLibrary after = crate_library("reinforced crate", 4.0F);

        sc::World loaded;
        check(sc::load_scene(document, loaded, registry, after),
              "the same scene loads against the edited prefab");

        const std::vector<entt::entity> roots = instance_roots(loaded);
        check(roots.size() == 2, "both instances came back");

        for (const entt::entity root : roots) {
            check(loaded.registry().get<sc::Name>(root).value == "reinforced crate",
                  "an instance took the new name from the prefab");
            check(loaded.local(root).scale.x == 4.0F,
                  "an instance took the new scale from the prefab");
        }

        check(loaded.local(roots.at(0)).position.x == 0.0F,
              "the instance that overrode nothing follows the prefab");
        check(loaded.local(roots.at(1)).position.x == 5.0F,
              "the instance that overrode a field keeps that field");
    }

    void test_missing_prefab() {
        const sc::ComponentRegistry registry = make_registry();
        const sc::PrefabLibrary library = crate_library();
        const nlohmann::json document = two_crates(registry, library);

        // A build with no prefabs cannot invent the entities.
        const sc::PrefabLibrary empty;
        sc::World loaded;
        check(!sc::load_scene(document, loaded, registry, empty),
              "a scene that names a prefab the library does not hold is an error");
        check(loaded.size() == 2, "the reader still left one entity for each record");

        // Saving an instance without its prefab writes the entities one by one,
        // rather than throwing away every field the instance holds.
        sc::World world;
        check(sc::instantiate(world, crate_of(library), moved_to(9.0F), registry) !=
                  entt::null,
              "the instance builds");
        const nlohmann::json expanded = sc::save_scene(world, registry, empty);
        check(expanded["entities"].size() == 2, "the instance was written entity by entity");
        check(!expanded["entities"][0].contains("prefab"), "the link is gone");
        check(expanded["entities"][0]["components"]["Transform"]["position"][0] == 9.0F,
              "the fields the instance held all survived");
    }

    void test_version_one_still_loads() {
        const sc::ComponentRegistry registry = make_registry();

        // A scene written before prefabs existed. No record names one.
        nlohmann::json old = nlohmann::json::object();
        old["__version"] = 1;
        nlohmann::json record = nlohmann::json::object();
        record["parent"] = -1;
        record["components"]["Name"] = engine::reflect::to_json(sc::Name{ "lonely" });
        old["entities"] = nlohmann::json::array({ record });

        sc::World world;
        check(sc::load_scene(old, world, registry, sc::PrefabLibrary{}),
              "a version 1 scene still reads");
        check(world.size() == 1, "the entity arrived");
    }

} // namespace

int main() {
    std::printf("parsing\n");
    test_parse();
    test_library();
    test_add_file();
    std::printf("overrides\n");
    test_override_patch();
    std::printf("instances\n");
    test_instantiate();
    test_member_identities_are_derived();
    test_overrides_are_per_field();
    test_instantiate_refuses_bad_input();
    test_unknown_component_is_a_warning();
    std::printf("scene files\n");
    test_an_added_child_survives();
    test_an_addition_keeps_its_identity_through_two_trips();
    test_a_destroyed_member_stays_destroyed();
    test_a_moved_member_stays_where_it_was_put();
    test_removing_a_parent_removes_what_hangs_under_it();
    test_a_record_that_will_not_read_is_refused();
    test_scene_collapses_an_instance();
    test_scene_round_trip();
    test_prefab_edit_reaches_instances();
    std::printf("tolerance\n");
    test_missing_prefab();
    test_version_one_still_loads();
    return test::report();
}
