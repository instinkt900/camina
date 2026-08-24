// M4.2 tests for the cooker and the manifest.
//
// The property that carries the milestone is the incremental check. A cook
// that redoes everything wastes a minute today and an hour at M4.4, and a cook
// that skips too much ships a stale asset. Both failures are quiet, so the
// tests here drive the second run and check the counts rather than the output.
//
// The cooker now links libshaderc, so shader tests no longer need a separate
// glslc executable.

#include "assets/content.h"
#include "assets/hot_reload.h"
#include "assets/irradiance.h"
#include "assets/manifest.h"
#include "assets/meta.h"
#include "assets/reference.h"
#include "assets/shader.h"
#include "assets/sound.h"
#include "assets/texture.h"
#include "check.h"
#include "import/cook.h"
#include "import/document.h"
#include "import/source_assets.h"
#include "import/writer.h"
#include "core/guid.h"
#include "platform/paths.h"
#include "reflect/attributes.h"
#include "reflect/reflect.h"
#include "scene/component_registry.h"
#include "scene/references.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <span>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace test_game {

    /**
     * A component the engine does not define, holding an asset reference.
     *
     * The sandbox stands in for a game everywhere else in this repository, and
     * neither of its components names an asset. So the case that matters here
     * has nothing to drive it: a cooker that knew only the engine's components
     * would pass every test and still fail the first game that added one.
     */
    struct Billboard {
        engine::Guid mesh; ///< The mesh it draws, named by identity.
    };

} // namespace test_game

/// Field descriptors for the test game component.
template <>
struct engine::reflect::Describe<test_game::Billboard> {
    static constexpr const char* name = "Billboard"; ///< The name a document stores.
    /// @brief The one field, and it names an asset.
    /// @return A tuple of field descriptors.
    static constexpr auto fields() {
        return std::make_tuple(
            ENGINE_FIELD(test_game::Billboard, mesh, engine::reflect::AssetRef{}));
    }
};

namespace {

    using test::check;

    /**
     * The cooked name of one variant of a shader source.
     *
     * The shader rule numbers its parts the way the glTF rule does, so
     * `look.frag` gives `look.frag.0.shader` for the base form. Part 0 is the
     * form compiled with no defines.
     */
    [[nodiscard]] std::string shader_part(std::string_view source, int part = 0) {
        return std::string(source) + "." + std::to_string(part) +
               engine::assets::kShaderExtension;
    }
    namespace as = engine::assets;
    namespace sc = engine::scene;

    /// Names this binary's scratch tree. See test::scratch.
    constexpr std::string_view kSuite = "cooker";

    std::filesystem::path scratch(std::string_view name) {
        return test::scratch(kSuite, name);
    }

    void write_file(const std::filesystem::path& path, std::string_view text) {
        std::filesystem::create_directories(path.parent_path());
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file << text;
    }

    std::string read_file(const std::filesystem::path& path) {
        std::ifstream file(path, std::ios::binary);
        return std::string{ std::istreambuf_iterator<char>{ file },
                            std::istreambuf_iterator<char>{} };
    }

    void test_hash_is_content_not_time() {
        const std::filesystem::path dir = scratch("hash");
        const std::filesystem::path file = dir / "a.txt";
        write_file(file, "hello");

        std::uint64_t first = 0;
        check(as::hash_file(file, first), "a file hashes");

        // Rewriting the same bytes must give the same answer, even though the
        // modification time moved. That is the whole reason this is a content
        // hash and not a timestamp.
        write_file(file, "hello");
        std::uint64_t again = 0;
        check(as::hash_file(file, again) && again == first,
              "the same bytes hash the same after a rewrite");

        write_file(file, "hellp");
        std::uint64_t changed = 0;
        check(as::hash_file(file, changed) && changed != first,
              "one different byte hashes differently");

        std::uint64_t missing = 0;
        check(!as::hash_file(dir / "not_there.txt", missing), "a missing file reports");

        test::remove_tree(dir);
    }

    void test_input_order_matters() {
        const std::filesystem::path dir = scratch("inputs");
        write_file(dir / "a.txt", "aaa");
        write_file(dir / "b.txt", "bbb");

        std::uint64_t forward = 0;
        std::uint64_t backward = 0;
        check(as::hash_inputs(dir, { "a.txt", "b.txt" }, forward), "two inputs hash");
        check(as::hash_inputs(dir, { "b.txt", "a.txt" }, backward), "and so does the other order");
        check(forward != backward, "the input order changes the hash");

        // The name is part of the hash, so renaming a file counts as a change
        // even when the bytes stay the same.
        write_file(dir / "c.txt", "aaa");
        std::uint64_t renamed = 0;
        check(as::hash_inputs(dir, { "c.txt", "b.txt" }, renamed), "the renamed pair hashes");
        check(renamed != forward, "the same bytes under another name is a change");

        // Swap the contents of two inputs and leave the names alone. The
        // checks above cannot tell this apart, because they would still pass
        // if only the names decided the order. Folding each content hash in
        // with an exclusive or would pass them too, and would then miss this,
        // because an exclusive or gives the same answer either way round.
        write_file(dir / "a.txt", "aaa");
        write_file(dir / "b.txt", "bbb");
        std::uint64_t before = 0;
        check(as::hash_inputs(dir, { "a.txt", "b.txt" }, before), "the pair hashes");

        write_file(dir / "a.txt", "bbb");
        write_file(dir / "b.txt", "aaa");
        std::uint64_t after = 0;
        check(as::hash_inputs(dir, { "a.txt", "b.txt" }, after), "the swapped pair hashes");
        check(before != after, "swapping the contents of two inputs is a change");

        std::uint64_t broken = 0;
        check(!as::hash_inputs(dir, { "a.txt", "gone.txt" }, broken),
              "a missing input reports rather than hashing what is left");

        test::remove_tree(dir);
    }

    void test_cook_and_skip() {
        const std::filesystem::path source = scratch("skip/src");
        const std::filesystem::path out = scratch("skip/out");
        // A rule that copies the bytes, so this stays a test of the manifest
        // rather than of any one format. Every file the cooker touches has a
        // rule now, and a script is the simplest of them. See issue #178.
        write_file(source / "one.lua", "{}");
        write_file(source / "nested" / "two.lua", "{}");

        engine::import::Options options{ .content = source, .out = out };
        engine::import::Result result;
        check(engine::import::cook_all(options, result), "the first cook works");
        check(result.cooked == 2 && result.skipped == 0, "and it cooks both assets");
        check(std::filesystem::exists(out / "one.lua"), "the asset landed");
        check(std::filesystem::exists(out / "nested" / "two.lua"),
              "and so did the one in a subdirectory");

        // The sidecars are what make an identity survive. A first cook writes
        // them into the source tree, next to the asset.
        check(std::filesystem::exists(as::meta_path(source / "one.lua")),
              "the first cook wrote a sidecar");

        engine::import::Result second;
        check(engine::import::cook_all(options, second), "the second cook works");
        check(second.cooked == 0 && second.skipped == 2, "and it cooks nothing");

        // Touching a file moves its time but not its bytes.
        std::filesystem::last_write_time(source / "one.lua",
                                         std::filesystem::file_time_type::clock::now());
        engine::import::Result touched;
        check(engine::import::cook_all(options, touched), "a touched tree cooks");
        check(touched.cooked == 0 && touched.skipped == 2, "and a new time alone cooks nothing");

        // A real change cooks that asset, and only that asset.
        write_file(source / "one.lua", "{\"changed\":true}");
        engine::import::Result changed;
        check(engine::import::cook_all(options, changed), "a changed tree cooks");
        check(changed.cooked == 1 && changed.skipped == 1, "and it cooks only what changed");
        check(read_file(out / "one.lua") == "{\"changed\":true}",
              "the new bytes reached the output");

        // --force is the escape hatch when somebody distrusts the manifest.
        engine::import::Options forced = options;
        forced.force = true;
        engine::import::Result all;
        check(engine::import::cook_all(forced, all), "a forced cook works");
        check(all.cooked == 2 && all.skipped == 0, "and it cooks everything again");

        test::remove_tree(source.parent_path());
    }

    void test_a_script_cooks_as_source_text() {
        const std::filesystem::path source = scratch("script/src");
        const std::filesystem::path out = scratch("script/out");

        constexpr std::string_view kScript = "function on_update(dt)\n  return dt\nend\n";
        write_file(source / "scripts" / "spin.lua", kScript);

        const engine::import::Options options{ .content = source, .out = out };
        engine::import::Result first;
        check(engine::import::cook_all(options, first), "a tree with a script cooks");
        check(first.cooked == 1, "and the script is the one asset in it");

        // The cooked form is the source text. See src/assets/script.h for why
        // it is not bytecode, and issue #258 for when that could change.
        const std::filesystem::path cooked = out / "scripts" / "spin.lua";
        check(std::filesystem::exists(cooked), "the script landed under its own name");
        check(read_file(cooked) == kScript, "and the bytes went through unchanged");

        // A script is an asset, so it carries an identity like any other. This
        // is what ScriptComponent names, and losing it would leave a component
        // pointing at nothing. See issue #178.
        check(std::filesystem::exists(as::meta_path(source / "scripts" / "spin.lua")),
              "the cook wrote it a sidecar");

        as::AssetMeta meta;
        check(as::load_meta(source / "scripts" / "spin.lua", meta) && meta.guid.valid(),
              "and the sidecar carries a real identity");

        // The sidecar identity is the one a ScriptComponent stores, so the
        // manifest has to agree with it. A valid but different GUID would read
        // as correct here and resolve to nothing at load time.
        as::Manifest manifest;
        check(as::load_manifest(out, manifest), "and the manifest reads back");
        const auto entry = std::ranges::find_if(manifest.entries, [](const as::ManifestEntry& e) {
            return e.source == "scripts/spin.lua";
        });
        check(entry != manifest.entries.end(), "the manifest holds the script");
        if (entry != manifest.entries.end()) {
            check(entry->outputs.size() == 1, "and it wrote exactly one output");
            check(entry->guid == meta.guid, "and it kept the identity the sidecar gave");
        }
        if (entry != manifest.entries.end() && entry->outputs.size() == 1) {
            // The rule adds no suffix and no part number, which is what keeps a
            // cooked script readable under the name a person wrote.
            check(entry->outputs[0].cooked == "scripts/spin.lua",
                  "and the cooked path is the source path");
            check(entry->outputs[0].guid == meta.guid, "and the output carries that identity");
        }

        // The rule changes no output, so the freshness check has to behave the
        // same way it does for every other asset.
        engine::import::Result second;
        check(engine::import::cook_all(options, second), "the second cook works");
        check(second.cooked == 0 && second.skipped == 1, "an unchanged script cooks nothing");

        write_file(source / "scripts" / "spin.lua", "function on_update() end\n");
        engine::import::Result changed;
        check(engine::import::cook_all(options, changed), "an edited script cooks");
        check(changed.cooked == 1, "and the edit is what cooked it");
        check(read_file(cooked) == "function on_update() end\n",
              "and the new text reached the cooked tree");

        test::remove_tree(source.parent_path());
    }

    void test_missing_output_recooks() {
        const std::filesystem::path source = scratch("gone/src");
        const std::filesystem::path out = scratch("gone/out");
        write_file(source / "one.scene", "{}");

        const engine::import::Options options{ .content = source, .out = out };
        engine::import::Result first;
        check(engine::import::cook_all(options, first), "the first cook works");

        // Somebody deleted the cooked file but left the manifest. The entry is
        // stale even though every input still hashes the same.
        std::filesystem::remove(out / "one.scene");
        engine::import::Result second;
        check(engine::import::cook_all(options, second), "the second cook works");
        check(second.cooked == 1, "a missing output cooks again");
        check(std::filesystem::exists(out / "one.scene"), "and the file came back");

        test::remove_tree(source.parent_path());
    }

    void test_new_identity_recooks() {
        const std::filesystem::path source = scratch("ident/src");
        const std::filesystem::path out = scratch("ident/out");
        write_file(source / "one.scene", "{}");

        const engine::import::Options options{ .content = source, .out = out };
        engine::import::Result first;
        check(engine::import::cook_all(options, first), "the first cook works");

        as::Manifest before;
        check(as::load_manifest(out, before), "the manifest reads back");
        const as::ManifestEntry* entry = as::find_by_source(before, "one.scene");
        check(entry != nullptr && entry->guid.valid(), "the entry carries an identity");

        // Deleting the sidecar gives the asset a new identity. Every reference
        // to it has to see the new one, so the entry cannot be reused.
        std::filesystem::remove(as::meta_path(source / "one.scene"));
        engine::import::Result second;
        check(engine::import::cook_all(options, second), "the second cook works");
        check(second.cooked == 1, "a new identity cooks again");

        as::Manifest after;
        check(as::load_manifest(out, after), "the manifest reads back");
        const as::ManifestEntry* fresh = as::find_by_source(after, "one.scene");
        // check() records a failure and carries on, so both pointers need a
        // guard here. Without it a failed lookup above crashes this line, and
        // the crash hides every test after it.
        check(entry != nullptr && fresh != nullptr && fresh->guid != entry->guid,
              "and the identity did change");

        test::remove_tree(source.parent_path());
    }

    void test_duplicate_identity_is_refused() {
        // Copying an asset together with its .meta sidecar gives both files
        // the same GUID. The cooker must refuse the tree before it writes a
        // manifest that loads only one of the two.
        const std::filesystem::path source = scratch("dup/src");
        const std::filesystem::path out = scratch("dup/out");
        write_file(source / "one.scene", "{}");

        engine::import::Options options{ .content = source, .out = out };
        engine::import::Result first;
        check(engine::import::cook_all(options, first), "the first cook works");
        check(first.cooked == 1, "it cooked one asset");

        std::filesystem::copy_file(source / "one.scene", source / "copy.scene");
        const auto sidecar = as::meta_path(source / "one.scene");
        const auto copied_sidecar = as::meta_path(source / "copy.scene");
        std::filesystem::copy_file(sidecar, copied_sidecar);

        engine::import::Result second;
        check(!engine::import::cook_all(options, second), "a duplicate identity fails the cook");

        test::remove_tree(source.parent_path());
    }

    void test_a_shell_metacharacter_name_still_cooks() {
        // A shader whose name a shell would read as a command. glslc now runs
        // through run_process, so no shell ever sees the name, and the cooker
        // no longer refuses it. Without shell_safe, a content tree holding a
        // file named `a$(id).vert` cooks the same way any other .vert does.
        const std::filesystem::path source = scratch("inject/src");
        const std::filesystem::path out = scratch("inject/out");
#if defined(_WIN32)
        const char* name = "a%PATH%.vert";
#else
        const char* name = "a$(id).vert";
#endif
        write_file(source / name, "#version 450\nvoid main() {}\n");

        engine::import::Options options{ .content = source, .out = out };
        engine::import::Result result;
        check(engine::import::cook_all(options, result), "a name a shell would expand now cooks");
        check(result.cooked == 1, "it counted the cook");
        check(std::filesystem::exists(out / shader_part(name)),
              "the cooked shader was written");

        test::remove_tree(source.parent_path());
    }

    /**
     * The reflection that M5.1 added, over a shader that declares real bindings.
     *
     * This is the test that would catch a layout drifting from its module. The
     * shader below declares a sampler, a uniform block, and a push block, and
     * every one of them has to come back out of the cooked file.
     */
    /**
     * A sidecar that lists variants cooks one module for each.
     *
     * The point of a permutation is that the define changes the module, so this
     * checks the two forms really differ rather than only that two files
     * appeared. A cook that ignored the defines would write two identical
     * modules and pass a weaker test.
     */

    // Issue #104. A rule that writes several outputs used to write each one to
    // its final path as it went, so a failure part way left the ones before it
    // behind. The manifest carried the previous entry forward, hash and all, so
    // the tree held part 0 from the new source and part 1 from the old one and
    // said both came from the old cook. A hot reload is what reached it: the
    // cooker runs on a save and the runtime reads the tree straight after.
    //
    // Every rule stages through the writer now, so these two cases check the
    // one mechanism through two rules rather than checking two mechanisms.

    /// The bytes of a triangle: 3 vertices of position, normal and texcoord.
    /// @p size scales it, so two calls give two different cooked meshes.
    void write_triangle_buffer(const std::filesystem::path& path, float size) {
        const std::array<float, 24> vertices{
            0.0F,
            0.0F,
            0.0F,
            0.0F,
            0.0F,
            1.0F,
            0.0F,
            0.0F, //
            size,
            0.0F,
            0.0F,
            0.0F,
            0.0F,
            1.0F,
            1.0F,
            0.0F, //
            0.0F,
            size,
            0.0F,
            0.0F,
            0.0F,
            1.0F,
            0.0F,
            1.0F,
        };
        const std::array<std::uint16_t, 4> indices{ 0, 1, 2, 0 };

        std::filesystem::create_directories(path.parent_path());
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file.write(reinterpret_cast<const char*>(vertices.data()),
                   static_cast<std::streamsize>(vertices.size() * sizeof(float)));
        file.write(reinterpret_cast<const char*>(indices.data()),
                   static_cast<std::streamsize>(indices.size() * sizeof(std::uint16_t)));
    }

    /// A glTF of two meshes. The second holds no primitives when @p broken,
    /// which is the failure `cook_mesh` reports after the first is written.
    [[nodiscard]] std::string two_mesh_gltf(bool broken) {
        const std::string second = broken ? R"({"primitives": []})"
                                          : R"({"primitives": [{"attributes":
            {"POSITION": 0, "NORMAL": 1, "TEXCOORD_0": 2}, "indices": 3}]})";
        return R"({
  "asset": {"version": "2.0"},
  "scene": 0,
  "scenes": [{"nodes": [0, 1]}],
  "nodes": [{"mesh": 0}, {"mesh": 1}],
  "meshes": [
    {"primitives": [{"attributes": {"POSITION": 0, "NORMAL": 1, "TEXCOORD_0": 2}, "indices": 3}]},
    )" + second +
               R"(
  ],
  "accessors": [
    {"bufferView": 0, "byteOffset": 0, "componentType": 5126, "count": 3, "type": "VEC3",
     "min": [0, 0, 0], "max": [1, 1, 0]},
    {"bufferView": 0, "byteOffset": 12, "componentType": 5126, "count": 3, "type": "VEC3"},
    {"bufferView": 0, "byteOffset": 24, "componentType": 5126, "count": 3, "type": "VEC2"},
    {"bufferView": 1, "componentType": 5123, "count": 3, "type": "SCALAR"}
  ],
  "bufferViews": [
    {"buffer": 0, "byteOffset": 0, "byteLength": 96, "byteStride": 32},
    {"buffer": 0, "byteOffset": 96, "byteLength": 6}
  ],
  "buffers": [{"byteLength": 104, "uri": "two.bin"}]
})";
    }

    void test_a_failed_gltf_leaves_the_earlier_mesh_alone() {
        const std::filesystem::path source = scratch("partial_gltf/src");
        const std::filesystem::path out = scratch("partial_gltf/out");
        write_triangle_buffer(source / "two.bin", 1.0F);
        write_file(source / "two.gltf", two_mesh_gltf(false));

        engine::import::Options options{ .content = source, .out = out };
        engine::import::Result first;
        check(engine::import::cook_all(options, first), "a glTF of two meshes cooks");

        const std::filesystem::path part0 = out / "two.gltf.0.mesh";
        const std::string before = read_file(part0);
        check(!before.empty(), "the first mesh was written");

        // The second mesh broken, and the geometry resized so that a cook which
        // published part 0 would write different bytes for it.
        //
        // **The buffer has to change, not the node.** A first version of this
        // test moved the second node instead, and a node translation goes into
        // the prefab rather than into a mesh. Part 0 came out identical either
        // way and the check below passed with the bug still in place. The
        // mutation is what found that, not review.
        write_triangle_buffer(source / "two.bin", 4.0F);
        write_file(source / "two.gltf", two_mesh_gltf(true));

        engine::import::Result second;
        check(!engine::import::cook_all(options, second), "a broken second mesh fails the cook");
        check(read_file(part0) == before,
              "and the first mesh on disk still holds the bytes from before");
    }

    void test_a_failed_shader_variant_leaves_the_base_form_alone() {
        const std::filesystem::path source = scratch("partial_shader/src");
        const std::filesystem::path out = scratch("partial_shader/out");

        const auto shader = [](std::string_view body, bool broken) {
            return std::string{ R"(#version 450
layout(location = 0) out vec4 out_color;
void main() {
    out_color = )" } +
                   std::string{ body } + R"(;
#ifdef SECOND
    )" + (broken ? "this is not glsl" : "out_color *= 0.5") +
                   R"(;
#endif
}
)";
        };

        write_file(source / "pair.frag", shader("vec4(1.0)", false));
        write_file(source / "pair.frag.meta", R"({
  "__version": 3,
  "guid": "5c2d4e77-1f3a-4b90-8d61-0e7a2c9b4d10",
  "shader": {
    "__version": 1,
    "variants": [
      { "name": "base", "defines": [] },
      { "name": "second", "defines": ["SECOND"] }
    ]
  }
})");

        engine::import::Options options{ .content = source, .out = out };
        engine::import::Result first;
        check(engine::import::cook_all(options, first), "a shader with two variants cooks");

        const std::filesystem::path base = out / shader_part("pair.frag", 0);
        const std::string before = read_file(base);
        check(!before.empty(), "the base form was written");

        // The base form still compiles and now says something different, so a
        // cook that published it would change these bytes. The second variant
        // does not compile at all.
        write_file(source / "pair.frag", shader("vec4(0.25)", true));

        engine::import::Result second;
        check(!engine::import::cook_all(options, second),
              "a variant that will not compile fails the cook");
        check(read_file(base) == before,
              "and the base form on disk still holds the bytes from before");
    }

    void test_a_discarded_write_leaves_the_file_it_would_have_replaced() {
        const std::filesystem::path out = scratch("staging");
        write_file(out / "kept.bin", "the bytes that were already there");

        const auto bytes = [](std::string_view text) {
            return std::as_bytes(std::span{ text.data(), text.size() });
        };

        engine::import::FileWriter writer(out);
        check(writer.write("kept.bin", bytes("replaced")), "a write is taken");
        check(writer.write("fresh.bin", bytes("new")), "and so is a second one");
        check(read_file(out / "kept.bin") == "the bytes that were already there",
              "neither one is visible before the commit");
        check(!std::filesystem::exists(out / "fresh.bin"), "and a new path does not appear");

        writer.discard();
        check(read_file(out / "kept.bin") == "the bytes that were already there",
              "a discard leaves the file that was there");
        check(!std::filesystem::exists(out / "fresh.bin"), "and writes no new one");

        check(writer.write("kept.bin", bytes("replaced")), "the same pair is written again");
        check(writer.write("fresh.bin", bytes("new")), "both of them");
        check(writer.commit(), "and the commit reports success");
        check(read_file(out / "kept.bin") == "replaced", "the commit replaced the old file");
        check(read_file(out / "fresh.bin") == "new", "and wrote the new one");

        // Nothing staged is left behind under a name a reader could open.
        bool leftover = false;
        for (const auto& entry : std::filesystem::directory_iterator(out)) {
            if (entry.path().filename().string().find(".cooking") != std::string::npos) {
                leftover = true;
            }
        }
        check(!leftover, "and left no staging file behind");
    }

    void test_a_shader_cooks_once_for_each_variant() {
        const std::filesystem::path source = scratch("variants/src");
        const std::filesystem::path out = scratch("variants/out");
        write_file(source / "many.frag", R"(#version 450
layout(set = 0, binding = 0) uniform sampler2D base_color;
#ifdef HAS_NORMAL_MAP
layout(set = 0, binding = 1) uniform sampler2D normal_map;
#endif
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 out_color;
void main() {
    out_color = texture(base_color, uv);
#ifdef HAS_NORMAL_MAP
    out_color += texture(normal_map, uv);
#endif
}
)");

        // The cooker writes the sidecar on the first cook, so this one is
        // written by hand to carry the variant list before that happens.
        write_file(source / "many.frag.meta", R"({
  "__version": 3,
  "guid": "3f1b1f42-9a1e-4c8e-9b2b-7c5a0d6e1f01",
  "shader": {
    "__version": 1,
    "variants": [
      { "name": "base", "defines": [] },
      { "name": "normal", "defines": ["HAS_NORMAL_MAP"] }
    ]
  }
})");

        engine::import::Options options{ .content = source, .out = out };
        engine::import::Result result;
        check(engine::import::cook_all(options, result), "a shader with two variants cooks");

        const std::string base_bytes = read_file(out / shader_part("many.frag", 0));
        const std::string variant_bytes = read_file(out / shader_part("many.frag", 1));
        check(!base_bytes.empty(), "the base form was written");
        check(!variant_bytes.empty(), "the second variant was written");

        engine::assets::Shader base;
        engine::assets::Shader variant;
        check(engine::assets::read_shader(
                  std::as_bytes(std::span(base_bytes.data(), base_bytes.size())), base, "base"),
              "the base form reads back");
        check(engine::assets::read_shader(
                  std::as_bytes(std::span(variant_bytes.data(), variant_bytes.size())), variant,
                  "variant"),
              "the second variant reads back");

        // Each module says what it was built with, so a consumer picks by what
        // a variant declares rather than by its place in the manifest.
        check(base.defines.empty(), "the base form declares no defines");
        check(variant.defines.size() == 1 && variant.defines[0] == "HAS_NORMAL_MAP",
              "the second variant carries the define it was built with");

        // The define really reached glslc. Without it both modules would
        // declare one binding and the two files would be the same.
        check(base.bindings.size() == 1, "the base form reads one texture");
        check(variant.bindings.size() == 2, "the variant reads two");
        check(base.spirv != variant.spirv, "and the two modules are not the same");

        // Part 0 keeps the asset's own identity, so a reference to the shader
        // itself still resolves. The rest derive one.
        //
        // The identity comes from the sidecar written above and not from the
        // manifest entry. Comparing the entry against itself would pass even if
        // the cook gave the whole asset a new identity, which is the one thing
        // that breaks every reference to it.
        engine::Guid sidecar_guid;
        check(engine::Guid::parse("3f1b1f42-9a1e-4c8e-9b2b-7c5a0d6e1f01", sidecar_guid),
              "the identity written into the sidecar above parses");

        as::Manifest manifest;
        check(as::load_manifest(out, manifest), "the manifest reads back");
        const as::ManifestEntry* entry = as::find_by_source(manifest, "many.frag");
        check(entry != nullptr && entry->outputs.size() == 2, "one source, two outputs");
        if (entry != nullptr && entry->outputs.size() == 2) {
            check(entry->guid == sidecar_guid, "the cook kept the identity the sidecar gave");
            check(entry->outputs[0].cooked == shader_part("many.frag", 0),
                  "the base form is part 0");
            check(entry->outputs[0].guid == sidecar_guid,
                  "the base form keeps the identity of the source");
            check(entry->outputs[1].cooked == shader_part("many.frag", 1),
                  "the second variant is part 1");
            check(entry->outputs[1].guid ==
                      engine::Guid::derive(sidecar_guid, as::kShaderPartKind, 1),
                  "the second variant derives its own");
            check(entry->outputs[0].guid != entry->outputs[1].guid,
                  "and the two are not the same identity");
        }

        test::remove_tree(source.parent_path());
    }

    /// The base form has to be first, because it keeps the source's identity.
    void test_a_variant_list_that_starts_with_defines_is_refused() {
        const std::filesystem::path source = scratch("badvariants/src");
        const std::filesystem::path out = scratch("badvariants/out");
        write_file(source / "first.frag", R"(#version 450
layout(location = 0) out vec4 out_color;
void main() { out_color = vec4(1.0); }
)");
        write_file(source / "first.frag.meta", R"({
  "__version": 3,
  "guid": "3f1b1f42-9a1e-4c8e-9b2b-7c5a0d6e1f01",
  "shader": {
    "__version": 1,
    "variants": [
      { "name": "normal", "defines": ["HAS_NORMAL_MAP"] },
      { "name": "base", "defines": [] }
    ]
  }
})");

        engine::import::Options options{ .content = source, .out = out };
        engine::import::Result result;
        check(!engine::import::cook_all(options, result),
              "a list whose first variant defines something fails the cook");
        check(result.failed == 1, "and it counts one failure");
        check(!std::filesystem::exists(out / shader_part("first.frag", 0)),
              "and it wrote no module");

        test::remove_tree(source.parent_path());
    }

    void test_the_cooker_reflects_what_a_shader_reads() {
        const std::filesystem::path source = scratch("reflect/src");
        const std::filesystem::path out = scratch("reflect/out");
        write_file(source / "look.frag", R"(#version 450
layout(set = 0, binding = 0) uniform sampler2D base_color;
layout(set = 0, binding = 1) uniform Material {
    vec4 base_color_factor;
    float roughness_factor;
} material;
layout(push_constant) uniform Push { mat4 model; } push;
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 out_color;
void main() {
    out_color = texture(base_color, uv) * material.base_color_factor *
                push.model[0][0] * material.roughness_factor;
}
)");

        engine::import::Options options{ .content = source, .out = out };
        engine::import::Result result;
        check(engine::import::cook_all(options, result), "a shader with real bindings cooks");

        const std::string cooked =
            read_file(out / shader_part("look.frag"));
        check(!cooked.empty(), "the cooked shader is there");

        engine::assets::Shader shader;
        check(engine::assets::read_shader(
                  std::as_bytes(std::span(cooked.data(), cooked.size())), shader, "look.frag"),
              "and it reads back");
        check(shader.stage == engine::assets::ShaderStage::Fragment,
              "the extension decided the stage");
        check(!shader.spirv.empty(), "it carries the module");

        // A mat4 is 64 bytes, and the push block holds one.
        check(shader.push_constant_size == 64, "the push block size came from the module");

        check(shader.bindings.size() == 2, "both bindings were found");
        if (shader.bindings.size() == 2) {
            // Sorted by set and then by binding, which is what the pipeline
            // layout needs and what the cooker promises.
            check(shader.bindings[0].binding == 0 &&
                      shader.bindings[0].kind ==
                          engine::assets::DescriptorKind::CombinedImageSampler,
                  "the sampler is binding 0");
            check(shader.bindings[0].name == "base_color", "and it kept its name");
            check(shader.bindings[1].binding == 1 &&
                      shader.bindings[1].kind == engine::assets::DescriptorKind::UniformBuffer,
                  "the uniform block is binding 1");
            check(shader.bindings[1].stages == engine::assets::kStageBitFragment,
                  "the stage bit says the fragment stage reads it");
            check(shader.bindings[1].block_size >= 20, "the block reports its size");
        }

        check(shader.params.size() == 2, "both members of the block were found");
        if (shader.params.size() == 2) {
            check(shader.params[0].name == "base_color_factor" &&
                      shader.params[0].type == engine::assets::ParamType::Vec4 &&
                      shader.params[0].offset == 0,
                  "the vec4 member reflected");
            check(shader.params[1].name == "roughness_factor" &&
                      shader.params[1].type == engine::assets::ParamType::Float &&
                      shader.params[1].offset == 16,
                  "the float member reflected, after the vec4");
        }

        test::remove_tree(source.parent_path());
    }

    /**
     * A push constant block that does not start at zero.
     *
     * SPIRV-Reflect reports the lowest member offset in `offset`, and `size`
     * already counts from the start of the range. Adding the two counts a late
     * member twice, and the size that comes out can pass the 128 bytes every
     * Vulkan device promises. The pipeline then fails to build, on a shader that
     * is correct.
     */
    void test_a_push_block_that_starts_late_reports_its_real_size() {
        const std::filesystem::path source = scratch("push_offset/src");
        const std::filesystem::path out = scratch("push_offset/out");
        // One mat4 at offset 64, so the range runs to 128 and not to 192.
        write_file(source / "late.frag", R"(#version 450
layout(push_constant) uniform Push {
    layout(offset = 64) mat4 model;
} push;
layout(location = 0) out vec4 out_color;
void main() { out_color = push.model[0]; }
)");

        engine::import::Options options{ .content = source, .out = out };
        engine::import::Result result;
        check(engine::import::cook_all(options, result), "a shader with a late push block cooks");

        const std::string cooked =
            read_file(out / shader_part("late.frag"));
        engine::assets::Shader shader;
        check(engine::assets::read_shader(
                  std::as_bytes(std::span(cooked.data(), cooked.size())), shader, "late.frag"),
              "the cooked shader reads back");
        check(shader.push_constant_size == 128,
              "the push range ends at 128, not at the offset plus the size");

        test::remove_tree(source.parent_path());
    }

    /**
     * A shader glslc will not compile must fail the cook, not write a file.
     *
     * The reflection runs after glslc, so a broken shader has to stop before it
     * and leave nothing behind. A cooked file with no module in it would fail
     * later, at pipeline build, with a message that names no source line.
     */
    void test_a_shader_that_does_not_compile_writes_nothing() {
        const std::filesystem::path source = scratch("broken_shader/src");
        const std::filesystem::path out = scratch("broken_shader/out");
        write_file(source / "bad.frag", "#version 450\nthis is not GLSL\n");

        engine::import::Options options{ .content = source, .out = out };
        engine::import::Result result;
        check(!engine::import::cook_all(options, result), "a shader that will not compile fails the cook");
        check(result.failed == 1, "and it counts one failure");
        check(!std::filesystem::exists(out / shader_part("bad.frag")),
              "no cooked shader was written");
        // glslc writes its module beside the cooked file, and the rule removes
        // it on every path. One left behind would grow the cooked tree forever.
        check(!std::filesystem::exists(out / (shader_part("bad.frag") + ".spv")),
              "and the compiler output did not stay behind");

        test::remove_tree(source.parent_path());
    }

    /**
     * Writes a 32-bit uncompressed TGA, which stb_image reads.
     *
     * A TGA and not a PNG, because a PNG needs a deflate stream and the test
     * would then need a compressor to write one. The cooker reads both through
     * the same stb_image call, so the format proves the same thing.
     *
     * @param texels The image, row by row, as RGBA.
     */
    void write_tga(const std::filesystem::path& path, std::uint32_t width, std::uint32_t height,
                   const std::vector<std::uint8_t>& texels) {
        std::filesystem::create_directories(path.parent_path());
        std::ofstream file(path, std::ios::binary | std::ios::trunc);

        // 18 bytes. Type 2 is uncompressed true color. The last byte holds 8
        // alpha bits and the top-left origin flag, so no row flip is needed.
        const std::array<std::uint8_t, 18> header{
            0,
            0,
            2,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            static_cast<std::uint8_t>(width & 0xFFU),
            static_cast<std::uint8_t>(width >> 8U),
            static_cast<std::uint8_t>(height & 0xFFU),
            static_cast<std::uint8_t>(height >> 8U),
            32,
            0x28,
        };
        file.write(reinterpret_cast<const char*>(header.data()), header.size());

        // TGA stores blue first.
        for (std::size_t at = 0; at + 3 < texels.size(); at += 4) {
            const std::array<char, 4> bgra{
                static_cast<char>(texels[at + 2]),
                static_cast<char>(texels[at + 1]),
                static_cast<char>(texels[at + 0]),
                static_cast<char>(texels[at + 3]),
            };
            file.write(bgra.data(), bgra.size());
        }
    }

    /// A 2 by 2 image, two texels black and two white, fully opaque.
    std::vector<std::uint8_t> half_black_half_white() {
        return { 0, 0, 0, 255, 255, 255, 255, 255,
                 255, 255, 255, 255, 0, 0, 0, 255 };
    }

    /// Reads a cooked texture file back, header and all.
    bool read_cooked(const std::filesystem::path& path, std::vector<std::byte>& bytes,
                     as::TextureView& view) {
        const std::string text = read_file(path);
        bytes.assign(reinterpret_cast<const std::byte*>(text.data()),
                     reinterpret_cast<const std::byte*>(text.data()) + text.size());
        return as::read_texture(bytes, view, path.string());
    }

    /**
     * The failure this milestone exists to prevent.
     *
     * A mip chain has to average light, not the numbers that encode it. Two
     * black texels and two white ones are half the light, and half the light
     * writes back as 188 in sRGB, not as 128. Building the chain the naive way
     * gives 128, every level gets darker than the one above, and distant
     * surfaces go muddy. The cause is very hard to find later.
     *
     * So this cooks the same four texels twice, once tagged sRGB and once
     * tagged linear, and checks that the smallest level differs.
     */
    void test_color_space_decides_the_mip_chain() {
        const std::filesystem::path source = scratch("space/src");
        const std::filesystem::path out = scratch("space/out");

        // The names drive the guess, which is also under test here.
        write_tga(source / "crate_basecolor.tga", 2, 2, half_black_half_white());
        write_tga(source / "crate_normal.tga", 2, 2, half_black_half_white());

        const engine::import::Options options{ .content = source, .out = out };
        engine::import::Result result;
        check(engine::import::cook_all(options, result), "both textures cook");
        check(result.cooked == 2, "and both are new");

        as::AssetMeta color;
        as::AssetMeta normal;
        check(as::load_meta(source / "crate_basecolor.tga", color), "the sidecars read back");
        check(as::load_meta(source / "crate_normal.tga", normal), "both of them");
        check(color.texture.color_space == as::ColorSpace::Srgb,
              "a base color map is guessed as sRGB");
        check(normal.texture.color_space == as::ColorSpace::Linear,
              "and a normal map is guessed as linear");

        // Two texels wide is below one BC7 block, so both stay uncompressed and
        // the bytes below are the texels themselves.
        std::vector<std::byte> color_bytes;
        std::vector<std::byte> normal_bytes;
        as::TextureView color_view;
        as::TextureView normal_view;
        check(read_cooked(out / "crate_basecolor.tga.tex", color_bytes, color_view),
              "the cooked base color reads");
        check(read_cooked(out / "crate_normal.tga.tex", normal_bytes, normal_view),
              "and so does the cooked normal map");
        check(color_view.format == as::TextureFormat::RGBA8,
              "an image below one block stays uncompressed");
        check(color_view.mip_count == 2, "2 by 2 gives two levels");

        // Level 1 is the last four bytes: one texel, RGBA.
        //
        // check() records a failure and carries on, so a read that failed above
        // leaves the payload empty and this would read past the end. A crash
        // here would take every test after it, and the log would name none of
        // them. The sentinel matches no expected value, so the checks below
        // still fail and still say what they wanted.
        const std::size_t level_one = as::level_bytes(as::TextureFormat::RGBA8, 2, 2);
        constexpr int kNoTexel = -1;
        const auto red = [&](const as::TextureView& view) {
            return view.payload.size() > level_one ? static_cast<int>(view.payload[level_one])
                                                   : kNoTexel;
        };

        // Half of full light is 0.5 linear, and 0.5 linear encodes as 188.
        check(red(color_view) >= 187 && red(color_view) <= 189,
              "an sRGB chain averages light, so half black and half white is 188");
        // The same four texels read as numbers average to 128.
        check(red(normal_view) >= 127 && red(normal_view) <= 129,
              "a linear chain averages the numbers, so the same texels give 128");
        check(red(color_view) != red(normal_view),
              "so the color space really does change the cooked bytes");

        test::remove_tree(source.parent_path());
    }

    /**
     * A minimal 16-bit WAV, built as bytes.
     *
     * The cooker test needs one to prove a sound cooks end to end, and a file
     * committed to the repository would prove less: a generated one says
     * exactly what is in it, so a wrong sample is a wrong number here rather
     * than a file nobody can read.
     */
    std::string wav_16_bit(std::uint16_t channels, std::uint32_t rate,
                           const std::vector<std::int16_t>& samples) {
        std::string out;
        const auto put_u16 = [&out](std::uint16_t value) {
            out.push_back(static_cast<char>(value & 0xFFU));
            out.push_back(static_cast<char>((value >> 8U) & 0xFFU));
        };
        const auto put_u32 = [&out](std::uint32_t value) {
            for (unsigned i = 0; i < 4; ++i) {
                out.push_back(static_cast<char>((value >> (8U * i)) & 0xFFU));
            }
        };

        const auto data_bytes = static_cast<std::uint32_t>(samples.size() * 2);
        out += "RIFF";
        put_u32(36 + data_bytes);
        out += "WAVEfmt ";
        put_u32(16);
        put_u16(1); // integer PCM
        put_u16(channels);
        put_u32(rate);
        put_u32(rate * channels * 2);
        put_u16(static_cast<std::uint16_t>(channels * 2));
        put_u16(16);
        out += "data";
        put_u32(data_bytes);
        for (const std::int16_t sample : samples) {
            put_u16(static_cast<std::uint16_t>(sample));
        }
        return out;
    }

    /**
     * A sound cooks, and the sidecar decides which of the two forms it takes.
     *
     * The two forms are the whole of the decision in DESIGN.md section 10 M11,
     * and each one is a different cooked file from the same source. So this
     * cooks one source both ways rather than trusting the rule twice.
     */
    void test_a_sound_cooks_both_ways() {
        const std::filesystem::path source = scratch("sound/src");
        const std::filesystem::path out = scratch("sound/out");
        const std::filesystem::path wav = source / "click.wav";
        write_file(wav, wav_16_bit(1, 44100, { 0, 16384, -16384, 0 }));
        // Nothing decodes a streamed sound at cook time, so these bytes need
        // not be real music. That is the property being checked.
        write_file(source / "theme.ogg", std::string(64, 'Z'));

        const engine::import::Options options{ .content = source, .out = out };
        engine::import::Result first;
        check(engine::import::cook_all(options, first), "the tree cooks");
        check(first.cooked == 2, "and it cooks both sounds");

        engine::import::Result second;
        check(engine::import::cook_all(options, second), "a second cook works");
        check(second.cooked == 0 && second.skipped == 2, "and it cooks nothing");

        // The guess reaches the sidecar the first cook wrote, and it is what
        // decides the form. A WAV decodes and everything else streams.
        as::AssetMeta wav_meta;
        as::AssetMeta ogg_meta;
        check(as::load_meta(wav, wav_meta) && !wav_meta.sound.stream,
              "the WAV is guessed as decoded");
        check(as::load_meta(source / "theme.ogg", ogg_meta) && ogg_meta.sound.stream,
              "and anything else as streamed");

        as::SoundView view;
        const std::string cooked_wav = read_file(out / "click.wav.snd");
        const std::span<const std::byte> wav_bytes{
            reinterpret_cast<const std::byte*>(cooked_wav.data()), cooked_wav.size()
        };
        check(as::read_sound(wav_bytes, view, "click.wav.snd"), "the cooked WAV reads back");
        check(view.storage == as::SoundStorage::Pcm, "it is PCM");
        check(view.sample_rate == 44100 && view.channels == 1 && view.frame_count == 4,
              "and it kept the rate, the channels and the frames the source had");

        const std::string cooked_ogg = read_file(out / "theme.ogg.snd");
        const std::span<const std::byte> ogg_bytes{
            reinterpret_cast<const std::byte*>(cooked_ogg.data()), cooked_ogg.size()
        };
        check(as::read_sound(ogg_bytes, view, "theme.ogg.snd"), "the cooked stream reads back");
        check(view.storage == as::SoundStorage::Encoded, "it is encoded");
        check(view.payload.size() == 64, "and it carries the source bytes untouched");

        // The sidecar is an input. Turning the WAV into a streamed sound has to
        // cook it again, and it has to change the cooked file rather than only
        // the sidecar.
        wav_meta.sound.stream = true;
        check(as::save_meta(wav, wav_meta), "the sidecar takes an edit");
        engine::import::Result third;
        check(engine::import::cook_all(options, third), "the third cook works");
        check(third.cooked == 1 && third.skipped == 1, "and it cooks only the sound that changed");

        const std::string restreamed = read_file(out / "click.wav.snd");
        const std::span<const std::byte> restreamed_bytes{
            reinterpret_cast<const std::byte*>(restreamed.data()), restreamed.size()
        };
        check(as::read_sound(restreamed_bytes, view, "click.wav.snd"), "it reads back");
        check(view.storage == as::SoundStorage::Encoded,
              "and the same source is stored the other way now");

        test::remove_tree(source.parent_path());
    }

    /**
     * A sound that cannot decode at cook time is refused, not cooked wrongly.
     *
     * This is the one shape the guess cannot fix: somebody edits a sidecar to
     * decode a file the cook-time reader cannot read. It must fail the cook and
     * name the file, because the alternative is a cooked sound holding bytes
     * that are not samples.
     */
    void test_a_sound_that_cannot_decode_is_refused() {
        const std::filesystem::path source = scratch("sound-bad/src");
        const std::filesystem::path out = scratch("sound-bad/out");
        write_file(source / "theme.ogg", std::string(64, 'Z'));

        const engine::import::Options options{ .content = source, .out = out };
        engine::import::Result first;
        check(engine::import::cook_all(options, first), "it cooks as a streamed sound");

        as::AssetMeta meta;
        check(as::load_meta(source / "theme.ogg", meta), "the sidecar reads");
        meta.sound.stream = false;
        check(as::save_meta(source / "theme.ogg", meta), "and it takes the edit");

        engine::import::Result second;
        check(!engine::import::cook_all(options, second), "the cook now fails");
        check(second.failed == 1, "and it names one asset as the reason");

        test::remove_tree(source.parent_path());
    }

    void test_editing_the_sidecar_cooks_again() {
        const std::filesystem::path source = scratch("edit/src");
        const std::filesystem::path out = scratch("edit/out");
        const std::filesystem::path image = source / "plate.tga";
        write_tga(image, 2, 2, half_black_half_white());

        const engine::import::Options options{ .content = source, .out = out };
        engine::import::Result first;
        check(engine::import::cook_all(options, first), "the first cook works");
        check(first.cooked == 1, "and it cooks the texture");

        engine::import::Result second;
        check(engine::import::cook_all(options, second), "the second cook works");
        check(second.skipped == 1, "and it cooks nothing");

        // The sidecar is an input, not only a place to keep the identity. An
        // edit that changes how a texture is read has to cook it again, or the
        // edit looks like it did nothing at all.
        as::AssetMeta meta;
        check(as::load_meta(image, meta), "the sidecar reads");
        meta.texture.color_space = as::ColorSpace::Linear;
        check(as::save_meta(image, meta), "and the edit writes");

        engine::import::Result third;
        check(engine::import::cook_all(options, third), "the third cook works");
        check(third.cooked == 1, "a changed sidecar cooks the texture again");

        std::vector<std::byte> bytes;
        as::TextureView view;
        check(read_cooked(out / "plate.tga.tex", bytes, view), "the cooked file reads");
        check(view.color_space == as::ColorSpace::Linear, "and it carries the new color space");

        test::remove_tree(source.parent_path());
    }

    void test_an_older_manifest_cooks_again() {
        const std::filesystem::path source = scratch("upgrade/src");
        const std::filesystem::path out = scratch("upgrade/out");
        write_file(source / "one.scene", "{}");

        const engine::import::Options options{ .content = source, .out = out };
        engine::import::Result first;
        check(engine::import::cook_all(options, first), "the first cook works");

        // A manifest an older cooker wrote, which named fewer inputs. is_fresh()
        // hashes the inputs the old entry names, so an entry like this would
        // stay fresh forever against a list this build no longer uses. That is
        // how the sidecar quietly stopped counting as an input when it was
        // added, and every asset kept skipping.
        as::Manifest older;
        check(as::load_manifest(out, older), "the manifest reads back");
        check(older.entries.size() == 1 && older.entries.front().inputs.size() == 2,
              "and the entry names the asset and its sidecar");
        older.entries.front().inputs.resize(1);
        check(as::hash_inputs(source, older.entries.front().inputs,
                              older.entries.front().hash),
              "the shorter list hashes");
        check(as::save_manifest(out, older), "and the older manifest writes");

        engine::import::Result second;
        check(engine::import::cook_all(options, second), "the second cook works");
        check(second.cooked == 1, "an entry with an older input list cooks again");

        as::Manifest now;
        check(as::load_manifest(out, now), "the new manifest reads back");
        check(now.entries.size() == 1 && now.entries.front().inputs.size() == 2,
              "and the entry names both inputs again");

        test::remove_tree(source.parent_path());
    }

    /**
     * A cooker that learned to write a new output has to cook the tree again.
     *
     * This is the one change no per-entry check can see. The freshness check
     * compares identities, input names, and input bytes, and a rule that
     * started writing a second kind of file changes none of them. The old
     * manifest therefore stays fresh forever and the new output never appears.
     *
     * A person meets that as content missing after an engine update, with no
     * message and no failing build, and cooking into a clean directory fixes it
     * without ever saying what was wrong.
     */
    void test_an_older_cooker_cooks_again() {
        const std::filesystem::path source = scratch("cookerver/src");
        const std::filesystem::path out = scratch("cookerver/out");
        write_file(source / "one.scene", "{}");

        const engine::import::Options options{ .content = source, .out = out };
        engine::import::Result first;
        check(engine::import::cook_all(options, first), "the first cook works");
        check(first.cooked == 1, "and it cooks the asset");

        engine::import::Result second;
        check(engine::import::cook_all(options, second), "the second cook works");
        check(second.skipped == 1, "and it skips, because nothing changed");

        // The manifest as an older cooker left it. save_manifest() stamps the
        // current version, so this goes in as text.
        const std::filesystem::path file = out / as::kManifestFile;
        std::string text = read_file(file);
        const std::string field = "\"cooker\": " + std::to_string(as::kCookerVersion);
        const std::size_t stamped = text.find(field);
        // check() records a failure and carries on, so a miss here would reach
        // replace() with npos and throw. That ends the process, and every test
        // after this one reports nothing at all.
        check(stamped != std::string::npos, "the manifest records the cooker version");
        if (stamped == std::string::npos) {
            return;
        }
        text.replace(stamped, field.size(), "\"cooker\": 1");
        write_file(file, text);

        engine::import::Result third;
        check(engine::import::cook_all(options, third), "the third cook works");
        check(third.cooked == 1, "a manifest from an older cooker cooks again");

        // A manifest older still, from before the field existed. It has to read
        // as an unknown cooker rather than as the current one.
        text = read_file(file);
        const std::size_t at = text.find("\"cooker\"");
        check(at != std::string::npos, "the field is back");
        if (at == std::string::npos) {
            return;
        }
        text.erase(at, text.find('\n', at) + 1 - at);
        write_file(file, text);

        engine::import::Result fourth;
        check(engine::import::cook_all(options, fourth), "the fourth cook works");
        check(fourth.cooked == 1, "and a manifest with no cooker field cooks again too");

        test::remove_tree(source.parent_path());
    }

    void test_compression_and_mip_switches() {
        const std::filesystem::path source = scratch("switch/src");
        const std::filesystem::path out = scratch("switch/out");
        const std::filesystem::path image = source / "wall.tga";

        // 8 by 8, so it is two BC7 blocks across and two down.
        constexpr std::uint32_t kSize = 8;
        std::vector<std::uint8_t> texels(static_cast<std::size_t>(kSize) * kSize * 4, 255);
        for (std::size_t at = 0; at < texels.size(); at += 8) {
            texels[at + 0] = 0;
            texels[at + 1] = 0;
            texels[at + 2] = 0;
        }
        write_tga(image, kSize, kSize, texels);

        const engine::import::Options options{ .content = source, .out = out };
        engine::import::Result result;
        check(engine::import::cook_all(options, result), "the texture cooks");

        std::vector<std::byte> bytes;
        as::TextureView view;
        check(read_cooked(out / "wall.tga.tex", bytes, view), "the cooked file reads");
        check(view.format == as::TextureFormat::BC7, "the default compresses");
        check(view.mip_count == 4, "and 8 by 8 gives four levels");
        check(view.payload.size() == as::chain_bytes(as::TextureFormat::BC7, kSize, kSize, 4),
              "and the payload is exactly the chain");

        // Both switches off. This is the path for a lookup table, where an
        // exact texel matters more than the memory does.
        as::AssetMeta meta;
        check(as::load_meta(image, meta), "the sidecar reads");
        meta.texture.compress = false;
        meta.texture.mips = false;
        check(as::save_meta(image, meta), "and the edit writes");

        engine::import::Result again;
        check(engine::import::cook_all(options, again), "it cooks again");
        check(again.cooked == 1, "because the sidecar changed");

        check(read_cooked(out / "wall.tga.tex", bytes, view), "the new file reads");
        check(view.format == as::TextureFormat::RGBA8, "compression is off");
        check(view.mip_count == 1, "and there is one level");
        check(view.payload.size() ==
                  static_cast<std::size_t>(kSize) * kSize * 4,
              "so the payload is the texels and nothing else");

        // Uncompressed keeps every texel, so the first one is still black. The
        // guard is for the same reason as the one above: a failed read leaves
        // the payload empty, and indexing it would end the run.
        check(!view.payload.empty() && static_cast<int>(view.payload[0]) == 0,
              "and the texels came through unchanged");

        test::remove_tree(source.parent_path());
    }

    /**
     * Sizes that are not powers of two, and sizes below one block.
     *
     * Every step here divides: the box filter picks a source range, the chain
     * runs down to 1 by 1, and BC7 rounds up to whole blocks. An off-by-one in
     * any of those reads past the end of a level or writes a payload the header
     * does not describe. read_texture() checks the payload against the
     * dimensions, so it fails the moment the two disagree.
     */
    void test_awkward_sizes() {
        const std::filesystem::path source = scratch("odd/src");
        const std::filesystem::path out = scratch("odd/out");

        struct Size {
            std::uint32_t width;
            std::uint32_t height;
            std::uint32_t levels;
        };
        const std::array<Size, 7> sizes{ {
            { .width = 1, .height = 1, .levels = 1 },
            { .width = 3, .height = 1, .levels = 2 },
            { .width = 1, .height = 7, .levels = 3 },
            { .width = 5, .height = 3, .levels = 3 },
            { .width = 17, .height = 5, .levels = 5 },
            { .width = 4, .height = 4, .levels = 3 },
            { .width = 6, .height = 10, .levels = 4 },
        } };

        for (const Size& size : sizes) {
            // A gradient, so the box filter has something to average that a
            // wrong source range would change.
            std::vector<std::uint8_t> texels;
            texels.reserve(static_cast<std::size_t>(size.width) * size.height * 4);
            for (std::uint32_t y = 0; y < size.height; ++y) {
                for (std::uint32_t x = 0; x < size.width; ++x) {
                    const auto value = static_cast<std::uint8_t>((x * 37 + y * 11) % 256);
                    texels.insert(texels.end(), { value, value, value, 255 });
                }
            }

            const std::string name =
                std::to_string(size.width) + "x" + std::to_string(size.height) + ".tga";
            write_tga(source / name, size.width, size.height, texels);
        }

        const engine::import::Options options{ .content = source, .out = out };
        engine::import::Result result;
        check(engine::import::cook_all(options, result), "every awkward size cooks");
        check(result.cooked == sizes.size(), "and all of them are new");

        for (const Size& size : sizes) {
            const std::string name =
                std::to_string(size.width) + "x" + std::to_string(size.height) + ".tga";

            std::vector<std::byte> bytes;
            as::TextureView view;
            check(read_cooked(out / (name + ".tex"), bytes, view),
                  ("the cooked " + name + " reads back").c_str());
            check(view.mip_count == size.levels,
                  ("and " + name + " has the level count the size allows").c_str());
            check(view.payload.size() ==
                      as::chain_bytes(view.format, size.width, size.height, size.levels),
                  ("and its payload is exactly the chain for " + name).c_str());
        }

        test::remove_tree(source.parent_path());
    }

    void test_a_failed_cook_keeps_the_asset_it_had() {
        const std::filesystem::path source = scratch("keep_old/src");
        const std::filesystem::path out = scratch("keep_old/out");
        write_tga(source / "wall.tga", 2, 2, half_black_half_white());

        const engine::import::Options options{ .content = source, .out = out };
        engine::import::Result first;
        check(engine::import::cook_all(options, first), "the first cook works");

        as::Content before;
        check(before.open(out), "the cooked directory opens");
        check(before.find("wall.tga") != nullptr, "and it holds the asset");

        // Break the source and cook again. A rule that fails writes no output,
        // so the cooked file from the first run is still there and still good.
        write_file(source / "wall.tga", "this is not a TGA at all");
        engine::import::Result second;
        check(!engine::import::cook_all(options, second), "the cook after the break fails");
        check(second.failed == 1, "and it counts one failure");

        // The point of the test. Dropping the entry would hide a cooked file
        // that is sitting right there, and the next start of the program would
        // fail on it. Somebody editing a shader meets that on the first typo.
        as::Content after;
        check(after.open(out), "the cooked directory still opens");
        check(after.find("wall.tga") != nullptr,
              "and the asset that failed keeps the entry it had");

        std::vector<std::byte> bytes;
        check(after.find("wall.tga") != nullptr &&
                  after.read_bytes(after.find("wall.tga")->outputs.front(), bytes) &&
                  !bytes.empty(),
              "and the cooked file it names still reads");

        test::remove_tree(source.parent_path());
    }

    void test_a_failed_cook_drops_the_entry_when_its_output_is_gone() {
        const std::filesystem::path source = scratch("keep_none/src");
        const std::filesystem::path out = scratch("keep_none/out");
        write_tga(source / "wall.tga", 2, 2, half_black_half_white());

        const engine::import::Options options{ .content = source, .out = out };
        engine::import::Result first;
        check(engine::import::cook_all(options, first), "the first cook works");

        as::Content before;
        check(before.open(out), "the cooked directory opens");
        check(before.find("wall.tga") != nullptr, "and it holds the asset");
        const std::filesystem::path cooked = out / before.find("wall.tga")->outputs.front().cooked;
        check(std::filesystem::exists(cooked), "the cooked file is there");

        // Take the cooked file away and break the source, so there is nothing
        // left to keep. An entry naming a file that is gone is worse than no
        // entry: the manifest would say the asset is there and the read would
        // fail later, further from the cause.
        std::filesystem::remove(cooked);
        write_file(source / "wall.tga", "this is not a TGA at all");

        engine::import::Result second;
        check(!engine::import::cook_all(options, second), "the cook after the break fails");

        as::Content after;
        check(after.open(out), "the cooked directory still opens");
        check(after.find("wall.tga") == nullptr,
              "and the entry goes, because the file it named is gone");

        test::remove_tree(source.parent_path());
    }

    void test_a_broken_image_fails_the_cook() {
        const std::filesystem::path source = scratch("broken/src");
        const std::filesystem::path out = scratch("broken/out");
        write_file(source / "not_an_image.tga", "this is not a TGA at all");

        const engine::import::Options options{ .content = source, .out = out };
        engine::import::Result result;
        check(!engine::import::cook_all(options, result), "a file stb cannot read fails the cook");
        check(result.failed == 1, "and it counts as a failure");
        check(!std::filesystem::exists(out / "not_an_image.tga.tex"),
              "and no cooked file was left behind");

        test::remove_tree(source.parent_path());
    }

    void test_documentation_is_not_an_asset() {
        const std::filesystem::path source = scratch("docs/src");
        const std::filesystem::path out = scratch("docs/out");
        write_file(source / "one.scene", "{}");
        write_file(source / "README.md", "Where this model came from.");
        write_file(source / "LICENSE", "CC0 1.0 Universal");
        write_file(source / "notes.txt", "Anything.");

        // A content directory holds a note saying where a model came from and
        // what its license is, next to the files it describes. Cooking one
        // would give it a GUID, write it a sidecar, and copy it into the cooked
        // tree where nothing reads it.
        const engine::import::Options options{ .content = source, .out = out };
        engine::import::Result result;
        check(engine::import::cook_all(options, result), "the cook works");
        check(result.cooked == 1, "and it cooks the asset and not the documentation");
        check(!std::filesystem::exists(out / "README.md"), "the README was not copied through");
        check(!std::filesystem::exists(out / "LICENSE"), "nor a file with no extension");
        check(!std::filesystem::exists(out / "notes.txt"), "nor a text file");
        check(!std::filesystem::exists(as::meta_path(source / "README.md")),
              "and no sidecar was written beside it");

        test::remove_tree(source.parent_path());
    }

    /**
     * A rule is what makes a file content, and a build tool has none.
     *
     * A generator script sits beside the model it writes. It used to fall past
     * every rule onto the copy path, which gave it an identity, wrote a sidecar
     * next to it in the **source** tree, and copied it where nothing reads it.
     * The sidecar is the part that bit: a clean checkout went dirty the first
     * time anybody ran the runtime. See issue #178.
     */
    void test_a_file_with_no_rule_is_not_content() {
        const std::filesystem::path source = scratch("norule/src");
        const std::filesystem::path out = scratch("norule/out");
        write_file(source / "one.scene", "{}");
        write_file(source / "generate.py", "print('writes room.gltf')");

        const engine::import::Options options{ .content = source, .out = out };
        engine::import::Result result;
        check(engine::import::cook_all(options, result), "the cook works");
        check(result.cooked == 1, "and it cooks the scene alone");

        check(!std::filesystem::exists(as::meta_path(source / "generate.py")),
              "no sidecar appeared next to the generator in the source tree");
        check(!std::filesystem::exists(out / "generate.py"),
              "and the generator did not reach the cooked tree");

        as::Content content;
        check(content.open(out), "the cooked directory opens");
        check(content.find("generate.py") == nullptr,
              "and the manifest does not name the generator");

        // The source tree holds what it held. A cook must not add to it.
        check(std::filesystem::exists(source / "generate.py"),
              "the generator itself is untouched");

        test::remove_tree(source.parent_path());
    }

    /**
     * A font and a layout have a consumer, so both earn a rule.
     *
     * The font rule copies the bytes, which is enough to make it content by
     * declaration rather than by falling past every other rule. That is what
     * issue #178 asks for. src/ui/font_factory.h opens the face by the name the
     * source had, so the cooked name has to match.
     *
     * The layout rule does more, and test_a_layout_names_its_image_by_identity
     * covers that. Here it only has to stay content and keep its name.
     */
    void test_a_font_and_a_layout_are_content() {
        const std::filesystem::path source = scratch("uiassets/src");
        const std::filesystem::path out = scratch("uiassets/out");
        write_file(source / "ui" / "fonts" / "body.ttf", "not a real face");
        write_file(source / "ui" / "main.mothui", "{\"children\":[]}");

        const engine::import::Options options{ .content = source, .out = out };
        engine::import::Result result;
        check(engine::import::cook_all(options, result), "the cook works");
        check(result.cooked == 2, "and it cooks both");

        check(std::filesystem::exists(out / "ui" / "fonts" / "body.ttf"),
              "the face reached the cooked tree under its own name");
        check(read_file(out / "ui" / "fonts" / "body.ttf") == "not a real face",
              "and the face went through unchanged");
        check(std::filesystem::exists(out / "ui" / "main.mothui"),
              "and the layout reached it under its own name too");

        test::remove_tree(source.parent_path());
    }

    /**
     * M10.2. A cooked layout names its image by identity, not by path.
     *
     * A layout stores an image relative to its own directory, which is moth_ui's
     * convention: moth_editor writes one that way and moth_packer reads one that
     * way. The engine follows the format rather than imposing a second
     * convention, and the rule translates. A path is the authored form and a
     * GUID is the cooked one, the same split cook_document makes for a scene.
     *
     * Without this a rename of the image silently breaks the layout, which is
     * what M4 gave every asset an identity to stop.
     */
    void test_a_layout_names_its_image_by_identity() {
        const std::filesystem::path source = scratch("uilayout/src");
        const std::filesystem::path out = scratch("uilayout/out");
        write_tga(source / "ui" / "panel.tga", 2, 2, half_black_half_white());

        // "panel.tga", not "ui/panel.tga". The layout sits in ui/ and names the
        // image beside it.
        write_file(source / "ui" / "main.mothui",
                   R"({"children":[{"type":"Image","imagePath":"panel.tga"}]})");

        const engine::import::Options options{ .content = source, .out = out };
        engine::import::Result result;
        check(engine::import::cook_all(options, result), "the cook works");

        as::AssetMeta image;
        check(as::load_meta(source / "ui" / "panel.tga", image) && image.guid.valid(),
              "the image has an identity");

        const nlohmann::json cooked =
            nlohmann::json::parse(read_file(out / "ui" / "main.mothui"));
        const std::string stored = cooked.at("children").at(0).at("imagePath");
        check(stored == image.guid.to_text(),
              "and the cooked layout holds that identity rather than the path");

        // The sidecar of the image is an input of the layout. Without it, giving
        // the image a new identity leaves the layout holding the old one and
        // nothing re-cooks it.
        as::Manifest manifest;
        check(as::load_manifest(out, manifest), "the manifest reads");
        const as::ManifestEntry* entry = as::find_by_source(manifest, "ui/main.mothui");
        check(entry != nullptr, "the layout is in the manifest");
        if (entry != nullptr) {
            const bool named =
                std::find(entry->inputs.begin(), entry->inputs.end(), "ui/panel.tga.meta") !=
                entry->inputs.end();
            check(named, "and the image sidecar is one of its inputs");
        }

        test::remove_tree(source.parent_path());
    }

    /**
     * A layout naming an image that is not there fails the cook.
     *
     * The alternative is a layout carrying a GUID that resolves to nothing,
     * which draws no image and reports one line at run time. That reads exactly
     * like a texture that failed to upload, and it is the failure naming an
     * asset by path exists to remove.
     */
    void test_a_layout_naming_a_missing_image_fails() {
        const std::filesystem::path source = scratch("uimissing/src");
        const std::filesystem::path out = scratch("uimissing/out");
        write_file(source / "ui" / "main.mothui",
                   R"({"children":[{"type":"Image","imagePath":"gone.tga"}]})");

        const engine::import::Options options{ .content = source, .out = out };
        engine::import::Result result;
        check(!engine::import::cook_all(options, result), "the cook fails");
        check(result.failed == 1, "and it counts the layout");

        test::remove_tree(source.parent_path());
    }

    /**
     * An image entity nobody has assigned yet is not a failure.
     *
     * moth_editor makes an image entity before a person picks a file, and
     * moth_ui draws nothing for one. A layout part way through being authored
     * must still cook, or the editor cannot save its own work in progress.
     */
    void test_a_layout_with_no_image_still_cooks() {
        const std::filesystem::path source = scratch("uiempty/src");
        const std::filesystem::path out = scratch("uiempty/out");
        write_file(source / "ui" / "main.mothui",
                   R"({"children":[{"type":"Image","imagePath":""}]})");

        const engine::import::Options options{ .content = source, .out = out };
        engine::import::Result result;
        check(engine::import::cook_all(options, result), "the cook works");

        const nlohmann::json cooked =
            nlohmann::json::parse(read_file(out / "ui" / "main.mothui"));
        check(cooked.at("children").at(0).at("imagePath").get<std::string>().empty(),
              "and the empty identity is left alone");

        test::remove_tree(source.parent_path());
    }

    /**
     * A layout that climbs out of the content tree fails the cook.
     *
     * The manifest is keyed on a path relative to the content root, so a path
     * reaching above it names no asset the cooker will ever write.
     */
    void test_a_layout_climbing_out_of_the_tree_fails() {
        const std::filesystem::path source = scratch("uiescape/src");
        const std::filesystem::path out = scratch("uiescape/out");
        write_file(source / "ui" / "main.mothui",
                   R"({"children":[{"type":"Image","imagePath":"../../secret.tga"}]})");

        const engine::import::Options options{ .content = source, .out = out };
        engine::import::Result result;
        check(!engine::import::cook_all(options, result), "the cook fails");

        test::remove_tree(source.parent_path());
    }

    /**
     * A rule matches whatever case the extension is written in.
     *
     * Half the rule predicates lowered the extension themselves and half never
     * did, so `SKY.HDR` cooked and `MAIN.SCENE` did not. That used to end in the
     * copy path, which put the file in the cooked tree unchanged and mostly went
     * unnoticed. With the copy path gone it would end in no rule at all, and the
     * asset would leave the cooked tree with nothing said. See issue #178.
     */
    void test_a_rule_ignores_the_case_of_the_extension() {
        const std::filesystem::path source = scratch("shouty/src");
        const std::filesystem::path out = scratch("shouty/out");
        write_file(source / "ONE.SCENE", "{}");
        write_file(source / "BODY.TTF", "not a real face");

        const engine::import::Options options{ .content = source, .out = out };
        engine::import::Result result;
        check(engine::import::cook_all(options, result), "the cook works");
        check(result.cooked == 2, "and an upper case extension still finds its rule");
        check(std::filesystem::exists(out / "ONE.SCENE"), "the scene reached the cooked tree");
        check(std::filesystem::exists(out / "BODY.TTF"), "and so did the face");

        test::remove_tree(source.parent_path());
    }

    void test_bad_input() {
        const std::filesystem::path out = scratch("bad/out");
        engine::import::Result result;

        const engine::import::Options missing{ .content = out / "not_there", .out = out };
        check(!engine::import::cook_all(missing, result), "a content directory that is not there fails");

        test::remove_tree(out.parent_path());
    }

    void test_content_reads_what_the_cooker_wrote() {
        const std::filesystem::path source = scratch("read/src");
        const std::filesystem::path out = scratch("read/out");
        write_file(source / "one.lua", "{}");

        const engine::import::Options options{ .content = source, .out = out };
        engine::import::Result result;
        check(engine::import::cook_all(options, result), "the cook works");

        as::Content content;
        check(content.open(out), "the cooked directory opens");
        check(content.manifest().entries.size() == 1, "and it holds one entry");

        const as::ManifestEntry* entry = content.find("one.lua");
        check(entry != nullptr, "an asset is findable by its source path");
        check(entry != nullptr && entry->outputs.size() == 1, "and a copy writes one file");

        std::vector<std::byte> bytes;
        check(entry != nullptr && content.read_bytes(entry->outputs.front(), bytes),
              "its bytes read");
        check(bytes.size() == 2, "and they are the bytes the source held");

        // The GUID is the path everything past the first lookup uses. A copy
        // rule gives its one output the source asset's own identity.
        std::vector<std::byte> by_guid;
        check(entry != nullptr && content.read_bytes(entry->guid, by_guid),
              "the same bytes read by identity");
        check(by_guid == bytes, "and they are the same bytes");
        check(!content.read_bytes(engine::Guid::generate(), by_guid),
              "an identity nothing cooked is refused");

        // The same bytes read by source path, which is the first lookup a caller
        // makes before it has a GUID.
        std::vector<std::byte> by_source;
        check(content.read_bytes("one.lua", by_source),
              "the same bytes read by source path");
        check(by_source == bytes, "and they are the same bytes");

        as::Content empty;
        check(!empty.open(out / "not_there"), "a directory with no manifest is refused");

        test::remove_tree(source.parent_path());
    }

    // M4.5. The reload half of the pipeline.
    //
    // These tests run the real cooker executable, which sits beside the test
    // program because both go to the same output directory. That makes them
    // end to end: a file changes, the cooker runs in its own process, and the
    // manifest says what moved.

    /// The cooker that the build put beside this program.
    [[nodiscard]] std::filesystem::path cooker_program() {
#if defined(_WIN32)
        constexpr const char* kName = "cooker.exe";
#else
        constexpr const char* kName = "cooker";
#endif
        return engine::platform::executable_directory() / kName;
    }

    /// The identity of the one output of a source path.
    [[nodiscard]] engine::Guid identity_of(const as::Content& content, const char* source) {
        const as::ManifestEntry* entry = content.find(source);
        if (entry == nullptr || entry->outputs.empty()) {
            return {};
        }
        return entry->outputs.front().guid;
    }

    void test_reload_names_only_what_changed() {
        const std::filesystem::path source = scratch("reload/src");
        const std::filesystem::path out = scratch("reload/out");
        write_file(source / "a.scene", "{}");
        write_file(source / "b.prefab", "{}");

        const engine::import::Options options{ .content = source, .out = out };
        engine::import::Result first;
        check(engine::import::cook_all(options, first), "the first cook works");

        as::Content content;
        check(content.open(out), "the content opens");
        const engine::Guid scene = identity_of(content, "a.scene");
        const engine::Guid prefab = identity_of(content, "b.prefab");
        check(scene.valid() && prefab.valid(), "both identities are there");

        std::vector<as::AssetChange> changed;
        check(content.reload(changed) && changed.empty(),
              "a reload over an unchanged tree names nothing");

        // The property the whole feature rests on. A reload that named every
        // asset would re-upload the entire tree every time one file is saved.
        write_file(source / "a.scene", "{\"changed\":true}");
        engine::import::Result second;
        check(engine::import::cook_all(options, second), "the second cook works");
        check(content.reload(changed), "the reload reads the new manifest");
        check(changed.size() == 1, "and it names one asset");
        if (changed.size() != 1) {
            return;
        }
        check(changed.front().guid == scene, "which is the one that changed");

        check(content.reload(changed) && changed.empty(),
              "reloading again names nothing, because nothing moved");

        test::remove_tree(source.parent_path());
    }

    void test_reload_names_an_asset_that_went_away() {
        const std::filesystem::path source = scratch("gone_reload/src");
        const std::filesystem::path out = scratch("gone_reload/out");
        write_file(source / "a.scene", "{}");
        write_file(source / "b.prefab", "{}");

        const engine::import::Options options{ .content = source, .out = out };
        engine::import::Result first;
        check(engine::import::cook_all(options, first), "the first cook works");

        as::Content content;
        check(content.open(out), "the content opens");
        const engine::Guid prefab = identity_of(content, "b.prefab");

        // A cache holding an asset the content no longer has would keep
        // drawing it, so the reload has to report it.
        std::filesystem::remove(source / "b.prefab");
        std::filesystem::remove(as::meta_path(source / "b.prefab"));
        engine::import::Result second;
        check(engine::import::cook_all(options, second), "the cook after the delete works");

        std::vector<as::AssetChange> changed;
        check(content.reload(changed), "the reload reads the new manifest");
        check(changed.size() == 1, "and it names one asset");
        if (changed.size() != 1) {
            return;
        }
        check(changed.front().guid == prefab, "which is the one that went away");

        test::remove_tree(source.parent_path());
    }

    void test_a_reload_says_what_an_asset_that_went_away_was() {
        const std::filesystem::path source = scratch("gone_kind/src");
        const std::filesystem::path out = scratch("gone_kind/out");
        write_file(source / "a.scene", "{}");
        write_file(source / "b.prefab", "{}");
        write_tga(source / "wall.tga", 2, 2, half_black_half_white());

        const engine::import::Options options{ .content = source, .out = out };
        engine::import::Result first;
        check(engine::import::cook_all(options, first), "the first cook works");

        as::Content content;
        check(content.open(out), "the content opens");

        // Delete the prefab. It is not in the new manifest, so nothing can be
        // looked up about it afterwards. Before this the runtime could not tell
        // a deleted prefab from a deleted texture, and the world it built from
        // that prefab stood until a restart.
        std::filesystem::remove(source / "b.prefab");
        std::filesystem::remove(as::meta_path(source / "b.prefab"));
        engine::import::Result second;
        check(engine::import::cook_all(options, second), "the cook after the delete works");

        std::vector<as::AssetChange> changed;
        check(content.reload(changed), "the reload reads the new manifest");
        check(changed.size() == 1, "and it names one asset");
        if (changed.size() != 1) {
            return;
        }
        check(changed.front().gone, "which is reported as gone");
        check(std::filesystem::path{ changed.front().cooked }.extension() == ".prefab",
              "and it says the asset was a prefab");

        test::remove_tree(source.parent_path());
    }

    void test_a_reload_says_what_a_changed_asset_is() {
        const std::filesystem::path source = scratch("changed_kind/src");
        const std::filesystem::path out = scratch("changed_kind/out");
        write_file(source / "a.scene", "{}");

        const engine::import::Options options{ .content = source, .out = out };
        engine::import::Result first;
        check(engine::import::cook_all(options, first), "the first cook works");

        as::Content content;
        check(content.open(out), "the content opens");

        write_file(source / "a.scene", R"({"changed":true})");
        engine::import::Result second;
        check(engine::import::cook_all(options, second), "the second cook works");

        std::vector<as::AssetChange> changed;
        check(content.reload(changed) && changed.size() == 1, "the reload names one asset");
        if (changed.size() != 1) {
            return;
        }
        check(!changed.front().gone, "which is still there");
        check(std::filesystem::path{ changed.front().cooked }.extension() == ".scene",
              "and it says the asset is a scene");

        test::remove_tree(source.parent_path());
    }

    void test_reload_keeps_what_it_has_when_the_manifest_will_not_read() {
        const std::filesystem::path source = scratch("bad_manifest/src");
        const std::filesystem::path out = scratch("bad_manifest/out");
        write_file(source / "a.scene", "{}");

        const engine::import::Options options{ .content = source, .out = out };
        engine::import::Result first;
        check(engine::import::cook_all(options, first), "the first cook works");

        as::Content content;
        check(content.open(out), "the content opens");
        const engine::Guid scene = identity_of(content, "a.scene");

        write_file(out / as::kManifestFile, "not json");

        std::vector<as::AssetChange> changed;
        check(!content.reload(changed), "a manifest that will not read fails the reload");
        check(changed.empty(), "and it names nothing");
        // The point of the test. Dropping the manifest here would leave the
        // program unable to find any asset at all, over one bad write.
        check(identity_of(content, "a.scene") == scene,
              "and the content keeps the manifest it already had");

        test::remove_tree(source.parent_path());
    }

    void test_hot_reload_cooks_what_changed() {
        const std::filesystem::path source = scratch("hot/src");
        const std::filesystem::path out = scratch("hot/out");
        write_file(source / "a.scene", "{}");

        const engine::import::Options options{ .content = source, .out = out };
        engine::import::Result first;
        check(engine::import::cook_all(options, first), "the first cook works");

        as::Content content;
        check(content.open(out), "the content opens");
        const engine::Guid scene = identity_of(content, "a.scene");

        as::HotReload reload;
        check(reload.start({ .source = source, .cooker = cooker_program() }),
              "hot reload starts when the source tree and the cooker are both there");
        check(reload.active(), "and it reports itself active");

        // The timers off, so the test drives it by polling rather than by
        // sleeping. A change still needs the walk after the one that saw it.
        reload.watcher().set_interval(std::chrono::milliseconds{ 0 });
        reload.watcher().set_settle(std::chrono::milliseconds{ 0 });

        std::vector<as::AssetChange> changed;
        check(!reload.poll(content, changed), "an unchanged tree reloads nothing");
        check(reload.cooks() == 0, "and it does not run the cooker at all");

        write_file(source / "a.scene", "{\"changed\":true}");
        check(!reload.poll(content, changed), "the walk that first sees the change waits");

        check(reload.poll(content, changed), "the next poll cooks and reloads");
        check(reload.cooks() == 1, "and it ran the cooker once");
        check(changed.size() == 1 && changed.front().guid == scene, "it names the changed asset");
        // A document is parsed and written back out, so this compares what
        // it says rather than the bytes it is made of.
        check(read_file(out / "a.scene").find("\"changed\"") != std::string::npos,
              "and the new value reached the cooked tree");

        test::remove_tree(source.parent_path());
    }

    void test_hot_reload_lives_through_a_cook_that_fails() {
        const std::filesystem::path source = scratch("hot_bad/src");
        const std::filesystem::path out = scratch("hot_bad/out");
        write_file(source / "a.scene", "{}");

        const engine::import::Options options{ .content = source, .out = out };
        engine::import::Result first;
        check(engine::import::cook_all(options, first), "the first cook works");

        as::Content content;
        check(content.open(out), "the content opens");
        const engine::Guid scene = identity_of(content, "a.scene");

        as::HotReload reload;
        check(reload.start({ .source = source, .cooker = cooker_program() }),
              "hot reload starts");
        reload.watcher().set_interval(std::chrono::milliseconds{ 0 });
        reload.watcher().set_settle(std::chrono::milliseconds{ 0 });

        // One good change and one bad file in the same batch. The cooker cooks
        // the good one, fails the bad one, and returns non-zero with the
        // cooked tree part way through. A file with a texture name and bytes
        // that are not an image is what fails it.
        //
        // The good change matters. A broken file on its own produces no output
        // at all, so a reload that ignored the exit code would find nothing to
        // do and would look correct while checking nothing.
        write_file(source / "a.scene", "{\"changed\":true}");
        write_file(source / "broken.png", "this is not a PNG");

        std::vector<as::AssetChange> changed;
        check(!reload.poll(content, changed), "the walk that first sees them waits");
        check(!reload.poll(content, changed), "a cook that fails reloads nothing");
        check(reload.cooks() == 1, "and it did run the cooker");
        check(changed.empty(), "and it names no asset to load again");

        // The point of the test. A failed cook must leave the program with
        // what it already had, and it must never end the process.
        check(identity_of(content, "a.scene") == scene, "the content keeps what it had");

        // The cooker writes its manifest even when one asset failed, so the
        // half-cooked tree really is on disk. Reading it here proves that the
        // check above refused something rather than finding nothing.
        std::vector<as::AssetChange> refused;
        check(content.reload(refused) && refused.size() == 1,
              "the failed cook did change the tree, so refusing it was a decision");

        test::remove_tree(source.parent_path());
    }

    void test_hot_reload_is_off_when_it_cannot_cook() {
        const std::filesystem::path source = scratch("hot_off/src");
        write_file(source / "a.scene", "{}");

        as::Content content;
        std::vector<as::AssetChange> changed;

        as::HotReload missing_cooker;
        check(!missing_cooker.start({ .source = source, .cooker = source / "no_cooker" }),
              "hot reload will not start without a cooker");
        check(!missing_cooker.active(), "and it reports itself off");
        check(!missing_cooker.poll(content, changed), "polling it does nothing");

        as::HotReload missing_source;
        check(!missing_source.start({ .source = source / "not_here",
                                      .cooker = cooker_program() }),
              "and it will not start without a source tree");

        test::remove_tree(source.parent_path());
    }

    /**
     * One triangle, with its buffer inside the file.
     *
     * The reference tests need a real glTF and not a stand-in, because the
     * check that matters is that the identity a reference resolves to is the
     * one the mesh rule really wrote. A stand-in would prove only that the
     * resolver agrees with itself.
     */
    constexpr const char* kMinimalGltf =
        R"GLTF({"asset":{"version":"2.0"},"scenes":[{"nodes":[0]}],"scene":0,"nodes":[{"mesh":0}],"meshes":[{"primitives":[{"attributes":{"POSITION":0,"NORMAL":1,"TEXCOORD_0":2},"indices":3}]}],"accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3","min":[0,0,0],"max":[1,1,0]},{"bufferView":1,"componentType":5126,"count":3,"type":"VEC3"},{"bufferView":2,"componentType":5126,"count":3,"type":"VEC2"},{"bufferView":3,"componentType":5123,"count":3,"type":"SCALAR"}],"bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":36},{"buffer":0,"byteOffset":36,"byteLength":36},{"buffer":0,"byteOffset":72,"byteLength":24},{"buffer":0,"byteOffset":96,"byteLength":6}],"buffers":[{"byteLength":102,"uri":"data:application/octet-stream;base64,AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAAAAAAAAAAAAAAAIA/AAAAAAAAAAAAAIA/AAAAAAAAAAAAAIA/AAAAAAAAAAAAAIA/AAAAAAAAAAAAAIA/AAABAAIA"}]})GLTF";

    /// The same triangle twice, so a reference to mesh 1 has something to name.
    constexpr const char* kTwoMeshGltf =
        R"GLTF({"asset":{"version":"2.0"},"scenes":[{"nodes":[0,1]}],"scene":0,"nodes":[{"mesh":0},{"mesh":1}],"meshes":[{"primitives":[{"attributes":{"POSITION":0,"NORMAL":1,"TEXCOORD_0":2},"indices":3}]},{"primitives":[{"attributes":{"POSITION":0,"NORMAL":1,"TEXCOORD_0":2},"indices":3}]}],"accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3","min":[0,0,0],"max":[1,1,0]},{"bufferView":1,"componentType":5126,"count":3,"type":"VEC3"},{"bufferView":2,"componentType":5126,"count":3,"type":"VEC2"},{"bufferView":3,"componentType":5123,"count":3,"type":"SCALAR"}],"bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":36},{"buffer":0,"byteOffset":36,"byteLength":36},{"buffer":0,"byteOffset":72,"byteLength":24},{"buffer":0,"byteOffset":96,"byteLength":6}],"buffers":[{"byteLength":102,"uri":"data:application/octet-stream;base64,AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAAAAAAAAAAAAAAAIA/AAAAAAAAAAAAAIA/AAAAAAAAAAAAAIA/AAAAAAAAAAAAAIA/AAAAAAAAAAAAAIA/AAABAAIA"}]})GLTF";

    // M4 issue #54. A scene and a prefab name an asset by source path, and the
    // cooker turns that into the identity before it writes the file. The
    // cooked document holds only GUIDs, because that is what survives a
    // rename. These tests read the cooked JSON back rather than trusting the
    // cook to have reported a success.

    /// The value of the one "mesh" field in a cooked document.
    [[nodiscard]] std::string cooked_mesh(const std::filesystem::path& document) {
        const std::string text = read_file(document);
        const std::string key = "\"mesh\": \"";
        const std::size_t at = text.find(key);
        if (at == std::string::npos) {
            return {};
        }
        const std::size_t from = at + key.size();
        const std::size_t to = text.find('"', from);
        return to == std::string::npos ? std::string{} : text.substr(from, to - from);
    }

    void test_a_reference_reads_into_its_parts() {
        as::AssetReference reference;
        check(as::parse_reference("asset:models/crate.gltf#mesh:2", reference),
              "a part reference parses");
        check(reference.source == std::filesystem::path{ "models/crate.gltf" } &&
                  reference.kind == "mesh" && reference.index == 2,
              "and it gives the path, the kind, and the index");

        check(as::parse_reference("asset:cube.png", reference),
              "a whole-file reference parses");
        check(reference.source == std::filesystem::path{ "cube.png" } &&
                  reference.kind.empty() && reference.index == 0,
              "and it names no part");

        // A GUID is not a reference. This is what keeps a document written
        // before any of this still readable.
        check(!as::parse_reference("508dcd18-9d17-8eb2-b877-acfa91632504", reference),
              "a GUID is left alone");
        check(!as::parse_reference("crate", reference), "and so is an ordinary name");

        // Each of these means to be a reference and cannot be one, so each has
        // to be refused rather than passed through as a name.
        check(!as::parse_reference("asset:", reference), "a reference to nothing fails");
        check(!as::parse_reference("asset:a.gltf#mesh", reference),
              "a kind with no index fails");
        check(!as::parse_reference("asset:a.gltf#:0", reference),
              "an index with no kind fails");
        check(!as::parse_reference("asset:a.gltf#mesh:x", reference),
              "an index that is not a number fails");
        check(!as::parse_reference("asset:a.gltf#mesh:0x", reference),
              "and so does one with something after the number");
    }

    void test_a_document_names_a_mesh_by_path() {
        const std::filesystem::path source = scratch("reference/src");
        const std::filesystem::path out = scratch("reference/out");
        write_file(source / "models" / "crate.gltf", kMinimalGltf);
        write_file(source / "a.prefab",
                   R"({"__version":1,"entities":[{"parent":-1,"components":{)"
                   R"("MeshRenderer":{"__version":1,"mesh":"asset:models/crate.gltf#mesh:0"}}}]})");

        const engine::import::Options options{ .content = source, .out = out };
        engine::import::Result result;
        check(engine::import::cook_all(options, result), "the cook works");

        // The identity the reference resolved to has to be the one the glTF
        // rule gave the mesh, or the prefab names a mesh nothing cooked.
        as::AssetMeta meta;
        check(as::load_meta(source / "models" / "crate.gltf", meta), "the glTF has a sidecar");
        const engine::Guid expected = engine::Guid::derive(meta.guid, "mesh", 0);

        check(cooked_mesh(out / "a.prefab") == expected.to_text(),
              "the cooked prefab holds the identity the reference named");

        // And that identity is really in the manifest, so the runtime can find
        // it. A reference that resolved to a plausible GUID nothing cooked
        // would pass the check above and draw nothing.
        as::Content content;
        check(content.open(out), "the cooked directory opens");
        check(as::find_by_guid(content.manifest(), expected) != nullptr,
              "and the manifest holds that identity");

        test::remove_tree(source.parent_path());
    }

    void test_a_document_keeps_a_guid_that_is_already_written() {
        const std::filesystem::path source = scratch("plain_guid/src");
        const std::filesystem::path out = scratch("plain_guid/out");
        const engine::Guid written = engine::Guid::generate();
        write_file(source / "a.prefab",
                   R"({"entities":[{"components":{"MeshRenderer":{"mesh":")" +
                       written.to_text() + R"("}}}]})");

        const engine::import::Options options{ .content = source, .out = out };
        engine::import::Result result;
        check(engine::import::cook_all(options, result), "the cook works");
        check(cooked_mesh(out / "a.prefab") == written.to_text(),
              "a document that already holds a GUID keeps it");

        test::remove_tree(source.parent_path());
    }

    void test_a_reference_that_names_nothing_fails_the_cook() {
        const std::filesystem::path source = scratch("bad_reference/src");
        const std::filesystem::path out = scratch("bad_reference/out");
        write_file(source / "a.prefab",
                   R"({"entities":[{"components":{"MeshRenderer":)"
                   R"({"mesh":"asset:models/gone.gltf#mesh:0"}}}]})");

        const engine::import::Options options{ .content = source, .out = out };
        engine::import::Result result;
        // The whole point. Before this a wrong identity drew nothing and
        // reported one line at runtime, which looks exactly like a mesh that
        // failed to upload.
        check(!engine::import::cook_all(options, result), "a reference to a file that is not there fails");
        check(result.failed == 1, "and it is reported as a failure");

        test::remove_tree(source.parent_path());
    }

    void test_a_reference_that_will_not_read_fails_the_cook() {
        const std::filesystem::path source = scratch("bad_fragment/src");
        const std::filesystem::path out = scratch("bad_fragment/out");
        write_file(source / "models" / "crate.gltf", kMinimalGltf);
        // A kind with no index. The file it names is really there, so the only
        // thing wrong is the reference itself.
        write_file(source / "a.prefab",
                   R"({"entities":[{"components":{"MeshRenderer":)"
                   R"({"mesh":"asset:models/crate.gltf#mesh"}}}]})");

        const engine::import::Options options{ .content = source, .out = out };
        engine::import::Result result;
        // Passing it through as an ordinary string is the failure to avoid.
        // The cooked prefab would then hold "asset:models/crate.gltf#mesh"
        // where a GUID goes, and the mesh would simply never load.
        check(!engine::import::cook_all(options, result),
              "a reference that will not read fails the cook");
        check(result.failed == 1, "and it is reported as a failure");

        test::remove_tree(source.parent_path());
    }

    void test_a_reference_that_leaves_the_content_tree_is_refused() {
        as::AssetReference reference;

        // Resolving one of these would read a file the content tree does not
        // own, and writing its sidecar would put a file next to it. A cook runs
        // over content that arrives from somewhere else, so this is a refusal.
        check(!as::parse_reference("asset:/etc/passwd", reference),
              "an absolute path is refused");
        check(!as::parse_reference("asset:../outside.gltf", reference),
              "a path that steps out with .. is refused");
        check(!as::parse_reference("asset:models/../../outside.gltf", reference),
              "and so is one that steps out part way along");

        // The step has to be a whole component. A directory whose name merely
        // starts with two dots is an ordinary directory.
        check(as::parse_reference("asset:..models/a.gltf", reference),
              "a name that only begins with dots is allowed");
    }

    void test_a_part_that_is_not_there_fails_the_cook() {
        const std::filesystem::path source = scratch("missing_part/src");
        const std::filesystem::path out = scratch("missing_part/out");
        write_file(source / "models" / "crate.gltf", kMinimalGltf);
        // The glTF holds one mesh, so mesh 0 is the only part there is.
        write_file(source / "a.prefab",
                   R"({"entities":[{"components":{"MeshRenderer":)"
                   R"({"mesh":"asset:models/crate.gltf#mesh:7"}}}]})");

        const engine::import::Options options{ .content = source, .out = out };
        engine::import::Result result;
        // Guid::derive answers for any index, so this used to cook happily and
        // give the prefab an identity nothing wrote. The scene then drew
        // nothing, which is the failure naming an asset by path is meant to
        // remove.
        check(!engine::import::cook_all(options, result),
              "a reference to a part that is not there fails the cook");
        check(result.failed == 1, "and it is reported as a failure");

        test::remove_tree(source.parent_path());
    }

    void test_a_reference_stops_being_sound_when_the_model_changes() {
        const std::filesystem::path source = scratch("stale_part/src");
        const std::filesystem::path out = scratch("stale_part/out");
        write_file(source / "models" / "crate.gltf", kTwoMeshGltf);
        write_file(source / "a.prefab",
                   R"({"entities":[{"components":{"MeshRenderer":)"
                   R"({"mesh":"asset:models/crate.gltf#mesh:1"}}}]})");

        const engine::import::Options options{ .content = source, .out = out };
        engine::import::Result first;
        check(engine::import::cook_all(options, first), "the first cook works");

        // Replace the model with one that holds a single mesh. It is a valid
        // model and it cooks, so nothing fails on its own account. The prefab
        // still says mesh 1 and nobody edited it, so nothing about the prefab
        // looks stale either. Only the finished manifest can say the identity
        // it names is gone.
        write_file(source / "models" / "crate.gltf", kMinimalGltf);
        engine::import::Result second;
        check(!engine::import::cook_all(options, second),
              "a model that lost the part fails the cook of the document naming it");
        check(second.failed == 1, "and the failure is the document, not the model");

        test::remove_tree(source.parent_path());
    }

    void test_a_document_that_will_not_parse_fails_the_cook() {
        const std::filesystem::path source = scratch("bad_document/src");
        const std::filesystem::path out = scratch("bad_document/out");
        write_file(source / "a.scene", "this is not json");

        const engine::import::Options options{ .content = source, .out = out };
        engine::import::Result result;
        // A scene used to be copied through, so a broken one reached the
        // runtime and emptied the world there instead.
        check(!engine::import::cook_all(options, result), "a scene that will not parse fails the cook");
        check(result.failed == 1, "and it is reported as a failure");

        test::remove_tree(source.parent_path());
    }

    void test_a_new_sidecar_cooks_the_document_that_names_it() {
        const std::filesystem::path source = scratch("ref_input/src");
        const std::filesystem::path out = scratch("ref_input/out");
        write_file(source / "models" / "crate.gltf", kMinimalGltf);
        write_file(source / "a.prefab",
                   R"({"entities":[{"components":{"MeshRenderer":)"
                   R"({"mesh":"asset:models/crate.gltf#mesh:0"}}}]})");

        const engine::import::Options options{ .content = source, .out = out };
        engine::import::Result first;
        check(engine::import::cook_all(options, first), "the first cook works");
        const std::string before = cooked_mesh(out / "a.prefab");

        engine::import::Result second;
        check(engine::import::cook_all(options, second), "the second cook works");
        check(second.cooked == 0, "and an unchanged tree cooks nothing");

        // Replacing the sidecar gives the glTF a new identity, so every
        // identity derived from it moves. The prefab has to be cooked again or
        // it names a mesh that no longer exists.
        std::filesystem::remove(as::meta_path(source / "models" / "crate.gltf"));
        engine::import::Result third;
        check(engine::import::cook_all(options, third), "the cook after the sidecar went works");
        check(cooked_mesh(out / "a.prefab") != before,
              "a new identity for the glTF moves what the prefab names");

        as::AssetMeta meta;
        check(as::load_meta(source / "models" / "crate.gltf", meta), "the glTF has a sidecar");
        check(cooked_mesh(out / "a.prefab") ==
                  engine::Guid::derive(meta.guid, "mesh", 0).to_text(),
              "and it names the identity the new sidecar gives");

        test::remove_tree(source.parent_path());
    }

    /**
     * Eight images, and only the last sits inside the file.
     *
     * That one derives at index 7 while the entry writes four outputs: a mesh,
     * a material, the texture, and a prefab. A search bounded by the number of
     * outputs stops at four and walks straight past it.
     */
    constexpr const char* kSparseImageGltf =
        R"GLTF({"asset":{"version":"2.0"},"scenes":[{"nodes":[0]}],"scene":0,"nodes":[{"mesh":0}],"meshes":[{"primitives":[{"attributes":{"POSITION":0,"NORMAL":1,"TEXCOORD_0":2},"indices":3,"material":0}]}],"materials":[{"pbrMetallicRoughness":{"baseColorTexture":{"index":7}}}],"textures":[{"source":0},{"source":1},{"source":2},{"source":3},{"source":4},{"source":5},{"source":6},{"source":7}],"images":[{"uri":"f0.png"},{"uri":"f1.png"},{"uri":"f2.png"},{"uri":"f3.png"},{"uri":"f4.png"},{"uri":"f5.png"},{"uri":"f6.png"},{"uri":"data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8BQDwAEhQGAhKmMIQAAAABJRU5ErkJggg=="}],"accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3","min":[0,0,0],"max":[1,1,0]},{"bufferView":1,"componentType":5126,"count":3,"type":"VEC3"},{"bufferView":2,"componentType":5126,"count":3,"type":"VEC2"},{"bufferView":3,"componentType":5123,"count":3,"type":"SCALAR"}],"bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":36},{"buffer":0,"byteOffset":36,"byteLength":36},{"buffer":0,"byteOffset":72,"byteLength":24},{"buffer":0,"byteOffset":96,"byteLength":6}],"buffers":[{"byteLength":102,"uri":"data:application/octet-stream;base64,AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAAAAAAAAAAAAAAAIA/AAAAAAAAAAAAAIA/AAAAAAAAAAAAAIA/AAAAAAAAAAAAAIA/AAAAAAAAAAAAAIA/AAABAAIA"}]})GLTF";

    // Issue #73. A document a person edits holds references, and a live world
    // holds identities. Saving has to put the references back, or the save
    // replaces every name with the GUID it resolved to and undoes #54.

    void test_an_identity_reads_back_as_the_reference_that_named_it() {
        const std::filesystem::path source = scratch("back/src");
        const std::filesystem::path out = scratch("back/out");
        // Two meshes, so the part index has to survive. With one, an index
        // that was always written as zero would read back correctly by luck.
        write_file(source / "models" / "crate.gltf", kTwoMeshGltf);
        write_tga(source / "wall.tga", 2, 2, half_black_half_white());

        const engine::import::Options options{ .content = source, .out = out };
        engine::import::Result result;
        check(engine::import::cook_all(options, result), "the cook works");

        as::Content content;
        check(content.open(out), "the cooked directory opens");

        // A part of a file. This is the case that has to be worked out again
        // rather than looked up, because only the source identity is stored.
        as::AssetMeta meta;
        check(as::load_meta(source / "models" / "crate.gltf", meta), "the glTF has a sidecar");
        const engine::Guid first = engine::Guid::derive(meta.guid, as::kMeshPartKind, 0);
        check(as::reference_for(content.manifest(), first) ==
                  "asset:models/crate.gltf#mesh:0",
              "a derived identity reads back as the part that named it");

        const engine::Guid second = engine::Guid::derive(meta.guid, as::kMeshPartKind, 1);
        check(as::reference_for(content.manifest(), second) ==
                  "asset:models/crate.gltf#mesh:1",
              "and the second part reads back as the second, not the first");

        // A whole file. Its one output goes by the source's own identity.
        const as::ManifestEntry* wall = content.find("wall.tga");
        check(wall != nullptr, "the texture is in the manifest");
        if (wall == nullptr) {
            return;
        }
        check(as::reference_for(content.manifest(), wall->guid) == "asset:wall.tga",
              "a whole file reads back as its path");

        check(as::reference_for(content.manifest(), engine::Guid::generate()).empty(),
              "an identity nothing cooked names nothing");
        check(as::reference_for(content.manifest(), engine::Guid{}).empty(),
              "and neither does the null identity");

        test::remove_tree(source.parent_path());
    }

    void test_a_part_with_a_sparse_index_reads_back() {
        const std::filesystem::path source = scratch("sparse/src");
        const std::filesystem::path out = scratch("sparse/out");
        write_file(source / "m.gltf", kSparseImageGltf);
        for (int at = 0; at < 7; ++at) {
            write_tga(source / ("f" + std::to_string(at) + ".png"), 2, 2,
                      half_black_half_white());
        }

        const engine::import::Options options{ .content = source, .out = out };
        engine::import::Result result;
        check(engine::import::cook_all(options, result), "the cook works");

        as::Content content;
        check(content.open(out), "the cooked directory opens");

        as::AssetMeta meta;
        check(as::load_meta(source / "m.gltf", meta), "the glTF has a sidecar");

        // Image 7 is the only one inside the file, so it derives at index 7
        // while the entry holds four outputs.
        const engine::Guid inside = engine::Guid::derive(meta.guid, as::kTexturePartKind, 7);
        const as::ManifestEntry* entry = content.find("m.gltf");
        check(entry != nullptr, "the glTF is in the manifest");
        if (entry == nullptr) {
            return;
        }
        check(entry->outputs.size() < 7, "the entry holds fewer outputs than that index");
        check(as::find_by_guid(content.manifest(), inside) != nullptr,
              "the inline image really cooked at index 7");
        check(as::reference_for(content.manifest(), inside) == "asset:m.gltf#texture:7",
              "and it reads back at index 7, past the count of outputs");

        test::remove_tree(source.parent_path());
    }

    void test_a_saved_document_keeps_its_references() {
        const std::filesystem::path source = scratch("round/src");
        const std::filesystem::path out = scratch("round/out");
        write_file(source / "models" / "crate.gltf", kMinimalGltf);
        const std::string authored =
            R"({"entities":[{"components":{"MeshRenderer":)"
            R"({"mesh":"asset:models/crate.gltf#mesh:0"},"Name":{"value":"crate"}}}]})";
        write_file(source / "a.prefab", authored);

        sc::ComponentRegistry types;
        sc::register_builtin_components(types);

        const engine::import::Options options{ .content = source, .out = out };
        engine::import::Result result;
        check(engine::import::cook_all(options, result), "the cook works");

        as::Content content;
        check(content.open(out), "the cooked directory opens");

        // The cooked document is what a live world reads, so it holds the
        // identity. Putting it back has to give the reference again.
        nlohmann::json cooked = nlohmann::json::parse(read_file(out / "a.prefab"), nullptr, false);
        check(!cooked.is_discarded(), "the cooked prefab parses");
        if (cooked.is_discarded()) {
            return;
        }
        check(cooked["entities"][0]["components"]["MeshRenderer"]["mesh"] != "asset:models/crate.gltf#mesh:0",
              "the cooked document holds the identity, not the reference");

        const std::size_t restored = sc::restore_references(cooked, content.manifest(), types);
        check(restored == 1, "one reference goes back");
        check(cooked["entities"][0]["components"]["MeshRenderer"]["mesh"] ==
                  "asset:models/crate.gltf#mesh:0",
              "and it is the reference the source held");

        // A name is left alone, so putting references back does not eat text
        // that only looks like it might be one.
        check(cooked["entities"][0]["components"]["Name"]["value"] == "crate",
              "an ordinary string is untouched");

        test::remove_tree(source.parent_path());
    }

    /**
     * Issue #81. A name that holds a cooked identity is not a reference.
     *
     * `Name.value` is an ordinary string, and the save walk used to read the
     * text of every string in the document. So an entity a person named after
     * a GUID the manifest knows came back from a save under a name nobody
     * wrote. The walk reads the descriptors now, and `Name.value` carries no
     * `reflect::AssetRef`.
     */
    void test_a_name_that_holds_an_identity_survives_a_save() {
        const std::filesystem::path source = scratch("byfield/src");
        const std::filesystem::path out = scratch("byfield/out");
        write_file(source / "models" / "crate.gltf", kMinimalGltf);
        write_file(source / "a.prefab",
                   R"({"entities":[{"components":{"MeshRenderer":)"
                   R"({"mesh":"asset:models/crate.gltf#mesh:0"}}}]})");

        const engine::import::Options options{ .content = source, .out = out };
        engine::import::Result result;
        check(engine::import::cook_all(options, result), "the cook works");

        as::Content content;
        check(content.open(out), "the cooked directory opens");

        nlohmann::json cooked = nlohmann::json::parse(read_file(out / "a.prefab"), nullptr, false);
        check(!cooked.is_discarded(), "the cooked prefab parses");
        if (cooked.is_discarded()) {
            return;
        }

        // The very identity the mesh field holds, put in a name as well. That
        // is the case the text walk could not tell apart.
        const auto mesh = cooked["entities"][0]["components"]["MeshRenderer"]["mesh"];
        cooked["entities"][0]["components"]["Name"] = { { "value", mesh } };

        sc::ComponentRegistry types;
        sc::register_builtin_components(types);
        const std::size_t restored = sc::restore_references(cooked, content.manifest(), types);
        check(restored == 1, "the mesh field alone goes back");
        check(cooked["entities"][0]["components"]["Name"]["value"] == mesh,
              "and the name keeps the identity a person typed into it");

        test::remove_tree(source.parent_path());
    }

    /**
     * A prefab instance holds its fields in a patch, and a patch is a document
     * shape of its own. A walk that read only `components` would leave every
     * reference an instance overrode as a raw GUID, and the sandbox scene is
     * almost all instances.
     */
    void test_the_save_walk_reaches_an_instance_patch() {
        const std::filesystem::path source = scratch("patch/src");
        const std::filesystem::path out = scratch("patch/out");
        write_file(source / "models" / "crate.gltf", kMinimalGltf);
        write_file(source / "a.scene",
                   R"({"entities":[{"prefab":"p.prefab","overrides":{"0":{"MeshRenderer":)"
                   R"({"mesh":"asset:models/crate.gltf#mesh:0"}}},)"
                   R"("added":[{"parent":0,"components":{"MeshRenderer":)"
                   R"({"mesh":"asset:models/crate.gltf#mesh:0"}}}]}]})");

        const engine::import::Options options{ .content = source, .out = out };
        engine::import::Result result;
        check(engine::import::cook_all(options, result), "the cook works");

        as::Content content;
        check(content.open(out), "the cooked directory opens");

        nlohmann::json cooked = nlohmann::json::parse(read_file(out / "a.scene"), nullptr, false);
        check(!cooked.is_discarded(), "the cooked scene parses");
        if (cooked.is_discarded()) {
            return;
        }

        sc::ComponentRegistry types;
        sc::register_builtin_components(types);
        check(sc::restore_references(cooked, content.manifest(), types) == 2,
              "the patch and the added entity both come back");
        check(cooked["entities"][0]["overrides"]["0"]["MeshRenderer"]["mesh"] ==
                  "asset:models/crate.gltf#mesh:0",
              "the override reads as the path the source names");
        check(cooked["entities"][0]["added"][0]["components"]["MeshRenderer"]["mesh"] ==
                  "asset:models/crate.gltf#mesh:0",
              "and so does the entity the instance added");

        test::remove_tree(source.parent_path());
    }


    /**
     * Issue #81. The cooker resolves a game's components, not only the engine's.
     *
     * Which field names an asset comes from the descriptors now, so the cooker
     * has to hold the descriptors of every component a document can carry. A
     * game defines its own, and `tools/cooker/main.cpp` registers them for that
     * reason. This is the property that makes the link worth its cost.
     */
    void test_a_game_component_resolves_its_reference() {
        const std::filesystem::path source = scratch("gamecomp/src");
        const std::filesystem::path out = scratch("gamecomp/out");
        write_file(source / "models" / "crate.gltf", kMinimalGltf);
        write_file(source / "a.prefab",
                   R"({"entities":[{"components":{"Billboard":)"
                   R"({"mesh":"asset:models/crate.gltf#mesh:0"}}}]})");

        engine::scene::ComponentRegistry types = engine::import::engine_components();
        types.add<test_game::Billboard>();

        const engine::import::Options options{ .content = source, .out = out, .components = &types };
        engine::import::Result result;
        check(engine::import::cook_all(options, result), "the cook works with the game registered");

        as::Content content;
        check(content.open(out), "the cooked directory opens");

        nlohmann::json cooked = nlohmann::json::parse(read_file(out / "a.prefab"), nullptr, false);
        check(!cooked.is_discarded(), "the cooked prefab parses");
        if (cooked.is_discarded()) {
            return;
        }

        // check() records a failure and carries on, so get<std::string>() on a
        // shape that is not there would throw and end the process. Every test
        // after this one would then report nothing at all.
        const nlohmann::json& field = cooked["entities"][0]["components"]["Billboard"]["mesh"];
        check(field.is_string(), "the cooked prefab still holds the game component's field");

        engine::Guid resolved;
        check(field.is_string() && engine::Guid::parse(field.get<std::string>(), resolved),
              "the game component's field holds an identity now");
        check(as::find_by_guid(content.manifest(), resolved) != nullptr,
              "and that identity is one this cook produced");

        // The way back agrees, because both directions read the same list.
        check(sc::restore_references(cooked, content.manifest(), types) == 1,
              "and the save side puts the same field back");

        test::remove_tree(source.parent_path());
    }

    /**
     * The same document, cooked without the game registered.
     *
     * A cooker that did not know `Billboard` used to resolve the field anyway,
     * because it read the text of every string. Reading the descriptors means
     * it cannot, so the failure has to be loud. Passing the text through would
     * write a cooked document the runtime reads as a GUID that will not parse.
     */
    void test_an_unregistered_component_fails_the_cook() {
        const std::filesystem::path source = scratch("unknown/src");
        const std::filesystem::path out = scratch("unknown/out");
        write_file(source / "models" / "crate.gltf", kMinimalGltf);
        write_file(source / "a.prefab",
                   R"({"entities":[{"components":{"Billboard":)"
                   R"({"mesh":"asset:models/crate.gltf#mesh:0"}}}]})");

        // No registry named, so the cooker uses the engine's own components.
        const engine::import::Options options{ .content = source, .out = out };
        engine::import::Result result;
        check(!engine::import::cook_all(options, result),
              "a reference the cooker cannot place fails the cook");
        check(!std::filesystem::exists(out / "a.prefab"),
              "and it writes no cooked document to be read later");

        // The validate pass reads every document again, the skipped ones
        // included. A document that already failed is left out of it, or one
        // fault would be reported twice and counted as two.
        check(result.failed == 1, "and one fault counts once");

        test::remove_tree(source.parent_path());
    }

    /**
     * A reference typed into a field that names no asset.
     *
     * `Name.value` is an ordinary string. The resolve leaves it alone, which is
     * the whole point of reading the descriptors, so something has to say that
     * the value will never resolve. Silence here would ship the text.
     */
    void test_a_reference_in_an_untagged_field_fails_the_cook() {
        const std::filesystem::path source = scratch("untagged/src");
        const std::filesystem::path out = scratch("untagged/out");
        write_file(source / "models" / "crate.gltf", kMinimalGltf);
        write_file(source / "a.prefab",
                   R"({"entities":[{"components":{"Name":)"
                   R"({"value":"asset:models/crate.gltf"}}}]})");

        const engine::import::Options options{ .content = source, .out = out };
        engine::import::Result result;
        check(!engine::import::cook_all(options, result), "a reference in a plain string fails the cook");
        check(!std::filesystem::exists(out / "a.prefab"), "and nothing is written");
        check(result.failed == 1, "and one fault counts once");

        test::remove_tree(source.parent_path());
    }

    /**
     * A name that only looks like a reference is not one.
     *
     * The check above must not turn every string beginning with six particular
     * characters into a cook failure by accident. It fires on the prefix alone,
     * so this holds it to text that really parses as a reference.
     */
    void test_a_name_that_is_not_a_reference_cooks() {
        const std::filesystem::path source = scratch("lookalike/src");
        const std::filesystem::path out = scratch("lookalike/out");
        write_file(source / "a.prefab",
                   R"({"entities":[{"components":{"Name":{"value":"assets go here"}}}]})");

        const engine::import::Options options{ .content = source, .out = out };
        engine::import::Result result;
        check(engine::import::cook_all(options, result), "an ordinary name cooks");

        nlohmann::json cooked = nlohmann::json::parse(read_file(out / "a.prefab"), nullptr, false);
        check(!cooked.is_discarded(), "the cooked prefab parses");
        if (!cooked.is_discarded()) {
            check(cooked["entities"][0]["components"]["Name"]["value"] == "assets go here",
                  "and it comes through unchanged");
        }

        test::remove_tree(source.parent_path());
    }

    /**
     * An HDR panorama cooks to a cubemap that the runtime reader accepts.
     *
     * The reader checks the payload against what the header describes, so this
     * proves the projection wrote exactly the bytes six faces of a mip chain
     * need. A face count or a format the two sides disagreed about would show
     * up here rather than as a device upload reading past the end.
     */
    void test_an_hdr_panorama_cooks_to_a_cubemap() {
        const std::filesystem::path source = scratch("env/src");
        const std::filesystem::path out = scratch("env/out");

        // A tiny Radiance file, written by hand. Two by one is the smallest
        // thing that is still twice as wide as it is tall.
        std::string hdr = "#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y 1 +X 2\n";
        const std::array<unsigned char, 8> texels{ 255, 128, 64, 128, 64, 128, 255, 129 };
        for (const unsigned char byte : texels) {
            hdr.push_back(static_cast<char>(byte));
        }
        write_file(source / "sky.hdr", hdr);

        write_file(source / "sky.hdr.meta", R"({
  "__version": 4,
  "guid": "7c2e5a90-1b33-4f7e-9a01-2d6b8e4f0c11",
  "environment": { "__version": 1, "face_size": 8 }
})");

        // This test cooks no shader, so libshaderc is never called.
        engine::import::Options options{ .content = source, .out = out };
        engine::import::Result result;
        check(engine::import::cook_all(options, result), "an HDR panorama cooks");

        const std::string bytes = read_file(out / "sky.hdr.tex");
        check(!bytes.empty(), "the cubemap was written");

        engine::assets::TextureView view;
        check(engine::assets::read_texture(
                  std::as_bytes(std::span(bytes.data(), bytes.size())), view, "sky"),
              "and the runtime reader accepts it");
        check(view.face_count == engine::assets::kCubeFaceCount, "it holds six faces");
        check(view.format == engine::assets::TextureFormat::RGBA16F, "it is half float");
        // An HDR file is linear by definition, so the rule must not guess.
        check(view.color_space == engine::assets::ColorSpace::Linear, "and it is linear");
        check(view.width == 8 && view.height == 8, "the face is the size the sidecar asked for");

        test::remove_tree(source.parent_path());
    }


    /// A Radiance file of one repeated colour, at a size the SH sum can integrate.
    [[nodiscard]] std::string constant_panorama(int width, int height, unsigned char mantissa,
                                                unsigned char exponent) {
        std::string hdr = "#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y " + std::to_string(height) +
                          " +X " + std::to_string(width) + "\n";
        for (int i = 0; i < width * height; ++i) {
            hdr.push_back(static_cast<char>(mantissa));
            hdr.push_back(static_cast<char>(mantissa));
            hdr.push_back(static_cast<char>(mantissa));
            hdr.push_back(static_cast<char>(exponent));
        }
        return hdr;
    }

    /// Turns a stored half float back into a float, for reading a cooked payload.
    [[nodiscard]] float from_half(std::uint16_t bits) {
        const std::uint32_t sign = static_cast<std::uint32_t>(bits & 0x8000U) << 16U;
        const std::uint32_t exponent = (bits >> 10U) & 0x1FU;
        const std::uint32_t mantissa = bits & 0x3FFU;

        std::uint32_t out = 0;
        if (exponent == 0) {
            // Zero or a denormal. A denormal half is far below anything this
            // test asserts on, so treating it as zero is enough here.
            out = sign;
        } else {
            out = sign | ((exponent + 112U) << 23U) | (mantissa << 13U);
        }
        float value = 0.0F;
        std::memcpy(&value, &out, sizeof(value));
        return value;
    }

    /**
     * A constant environment gives irradiance of pi times its radiance.
     *
     * This is the one number the whole projection can be checked against. A
     * Lambertian surface under uniform radiance L receives exactly pi times L,
     * whichever way it faces. So a wrong band constant, a wrong basis constant,
     * or a missing solid angle weight all move this away from pi, and nothing
     * else in the pipeline would report any of them.
     *
     * The direction sweep matters as much as the value. Every coefficient above
     * the first must vanish for a constant source, and a sign error in one of
     * them would leave the average right and make the answer lean.
     */
    void test_a_constant_environment_integrates_to_pi() {
        const std::filesystem::path source = scratch("shconst/src");
        const std::filesystem::path out = scratch("shconst/out");

        // 128 by 64, because the projection is a Riemann sum and a coarse one
        // does not converge. Mantissa 128 with exponent 128 is 0.5 in RGBE.
        constexpr float kRadiance = 0.5F;
        write_file(source / "flat.hdr", constant_panorama(128, 64, 128, 128));
        write_file(source / "flat.hdr.meta", R"({
  "__version": 4,
  "guid": "3f1c9d20-77aa-4b18-91cc-5e0d2a6b7f31",
  "environment": { "__version": 1, "face_size": 8, "specular_samples": 16 }
})");

        engine::import::Options options{ .content = source, .out = out };
        engine::import::Result result;
        check(engine::import::cook_all(options, result), "a constant environment cooks");

        const std::string bytes = read_file(out / "flat.hdr.irr");
        check(!bytes.empty(), "the irradiance was written");

        engine::assets::IrradianceSH sh;
        check(engine::assets::read_irradiance(
                  std::as_bytes(std::span(bytes.data(), bytes.size())), sh, "flat"),
              "and the runtime reader accepts it");
        // An unread file leaves every coefficient at zero, which would fail the
        // first check below and quietly pass the eight that follow it.
        if (bytes.empty()) {
            test::remove_tree(source.parent_path());
            return;
        }

        constexpr float kPi = 3.14159265F;
        const float wanted = kPi * kRadiance;

        // One percent, which is the discretization of the sum above rather than
        // anything the format loses.
        const float tolerance = wanted * 0.01F;
        check(std::abs(sh.c[0][0] - wanted) < tolerance,
              "the first coefficient is pi times the radiance");

        for (std::size_t i = 1; i < engine::assets::kIrradianceCoefficients; ++i) {
            check(std::abs(sh.c[i][0]) < tolerance,
                  "every coefficient above the first vanishes for a constant sky");
        }

        // The sum a shader writes, evaluated by hand in several directions. See
        // assets/irradiance.h, which this must agree with exactly.
        const std::array<std::array<float, 3>, 6> directions{ {
            { 1.0F, 0.0F, 0.0F },
            { -1.0F, 0.0F, 0.0F },
            { 0.0F, 1.0F, 0.0F },
            { 0.0F, -1.0F, 0.0F },
            { 0.0F, 0.0F, 1.0F },
            { 0.0F, 0.0F, -1.0F },
        } };
        for (const auto& n : directions) {
            const float evaluated =
                sh.c[0][0] + (sh.c[1][0] * n[1]) + (sh.c[2][0] * n[2]) + (sh.c[3][0] * n[0]) +
                (sh.c[4][0] * n[0] * n[1]) + (sh.c[5][0] * n[1] * n[2]) +
                (sh.c[6][0] * ((3.0F * n[2] * n[2]) - 1.0F)) + (sh.c[7][0] * n[0] * n[2]) +
                (sh.c[8][0] * ((n[0] * n[0]) - (n[1] * n[1])));
            check(std::abs(evaluated - wanted) < tolerance,
                  "and the basis gives pi times the radiance whichever way a normal points");
        }

        test::remove_tree(source.parent_path());
    }

    /// Encodes one linear colour as a Radiance RGBE quadruple.
    void push_rgbe(std::string& out, float r, float g, float b) {
        const float largest = std::max({ r, g, b });
        if (largest < 1e-32F) {
            out.append(4, '\0');
            return;
        }
        int exponent = 0;
        const float fraction = std::frexp(largest, &exponent);
        const float scale = fraction * 256.0F / largest;
        const auto byte = [scale](float value) {
            return static_cast<char>(
                static_cast<unsigned char>(std::clamp(value * scale, 0.0F, 255.0F)));
        };
        out.push_back(byte(r));
        out.push_back(byte(g));
        out.push_back(byte(b));
        out.push_back(static_cast<char>(static_cast<unsigned char>(exponent + 128)));
    }

    /**
     * A radiance field built only from the nine polynomials the format carries.
     *
     * Band limiting it to the second order is what makes the test exact. The
     * projection throws nothing away, so a brute force integral and the nine
     * coefficients have to agree, and any disagreement is an error rather than
     * the truncation every real environment suffers.
     *
     * The constant term is large enough to keep the whole field positive.
     */
    [[nodiscard]] float band_limited_radiance(float x, float y, float z) {
        return 0.9F + (0.25F * y) + (0.15F * x) + (0.10F * ((3.0F * z * z) - 1.0F)) +
               (0.08F * x * y);
    }

    /// The direction one panorama texel points, matching the cooker's mapping.
    void panorama_direction(std::size_t px, std::size_t py, int width, int height, float& x,
                            float& y, float& z) {
        constexpr float kPi = 3.14159265358979F;
        const float theta =
            (static_cast<float>(py) + 0.5F) / static_cast<float>(height) * kPi;
        const float angle =
            (((static_cast<float>(px) + 0.5F) / static_cast<float>(width)) - 0.5F) * 2.0F * kPi;
        x = std::sin(angle) * std::sin(theta);
        y = std::cos(theta);
        z = -std::cos(angle) * std::sin(theta);
    }

    /**
     * Every band of the irradiance projection, against an independent integral.
     *
     * The constant sky above pins the first coefficient and nothing else, because
     * a uniform source has no first or second band to get wrong. Deliberately
     * corrupting the band one constant leaves that test passing, so on its own it
     * would let two thirds of the maths through untested.
     *
     * This drives a source that carries all three bands, and checks the nine
     * coefficients against a direct cosine weighted sum over the same texels. The
     * source is band limited, so the two are the same number rather than an
     * approximation of one another.
     */
    void test_the_irradiance_matches_a_direct_integral() {
        const std::filesystem::path source = scratch("shbands/src");
        const std::filesystem::path out = scratch("shbands/out");

        constexpr int kWidth = 128;
        constexpr int kHeight = 64;

        std::string hdr = "#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y " +
                          std::to_string(kHeight) + " +X " + std::to_string(kWidth) + "\n";
        for (std::size_t py = 0; py < static_cast<std::size_t>(kHeight); ++py) {
            for (std::size_t px = 0; px < static_cast<std::size_t>(kWidth); ++px) {
                float dx = 0.0F;
                float dy = 0.0F;
                float dz = 0.0F;
                panorama_direction(px, py, kWidth, kHeight, dx, dy, dz);
                const float value = band_limited_radiance(dx, dy, dz);
                push_rgbe(hdr, value, value, value);
            }
        }
        write_file(source / "bands.hdr", hdr);
        write_file(source / "bands.hdr.meta", R"({
  "__version": 4,
  "guid": "e2b7c451-38df-4a90-b6e2-1c5a9d3f7048",
  "environment": { "__version": 1, "face_size": 4, "specular_samples": 8 }
})");

        engine::import::Options options{ .content = source, .out = out };
        engine::import::Result result;
        check(engine::import::cook_all(options, result), "a band limited environment cooks");

        const std::string bytes = read_file(out / "bands.hdr.irr");
        engine::assets::IrradianceSH sh;
        check(engine::assets::read_irradiance(
                  std::as_bytes(std::span(bytes.data(), bytes.size())), sh, "bands"),
              "the irradiance reads back");
        if (bytes.empty()) {
            test::remove_tree(source.parent_path());
            return;
        }

        constexpr float kPi = 3.14159265358979F;
        const float d_theta = kPi / static_cast<float>(kHeight);
        const float d_phi = 2.0F * kPi / static_cast<float>(kWidth);

        // Directions chosen so that every polynomial contributes to at least
        // one of them. An axis alone would leave the cross terms at zero.
        const std::array<std::array<float, 3>, 5> normals{ {
            { 0.0F, 1.0F, 0.0F },
            { 1.0F, 0.0F, 0.0F },
            { 0.0F, 0.0F, 1.0F },
            { 0.57735F, 0.57735F, 0.57735F },
            { -0.57735F, 0.57735F, -0.57735F },
        } };

        for (const auto& n : normals) {
            // The integral the coefficients stand in for, summed straight.
            float direct = 0.0F;
            for (std::size_t py = 0; py < static_cast<std::size_t>(kHeight); ++py) {
                for (std::size_t px = 0; px < static_cast<std::size_t>(kWidth); ++px) {
                    float dx = 0.0F;
                    float dy = 0.0F;
                    float dz = 0.0F;
                    panorama_direction(px, py, kWidth, kHeight, dx, dy, dz);

                    const float cosine = (n[0] * dx) + (n[1] * dy) + (n[2] * dz);
                    if (cosine <= 0.0F) {
                        continue;
                    }
                    const float solid_angle = std::sin(
                        (static_cast<float>(py) + 0.5F) / static_cast<float>(kHeight) * kPi);
                    direct += band_limited_radiance(dx, dy, dz) * cosine * solid_angle *
                              d_theta * d_phi;
                }
            }

            // The sum a shader writes. See assets/irradiance.h.
            const float evaluated =
                sh.c[0][0] + (sh.c[1][0] * n[1]) + (sh.c[2][0] * n[2]) + (sh.c[3][0] * n[0]) +
                (sh.c[4][0] * n[0] * n[1]) + (sh.c[5][0] * n[1] * n[2]) +
                (sh.c[6][0] * ((3.0F * n[2] * n[2]) - 1.0F)) + (sh.c[7][0] * n[0] * n[2]) +
                (sh.c[8][0] * ((n[0] * n[0]) - (n[1] * n[1])));

            // Three percent covers the eight bit mantissa a Radiance file
            // stores and the coarseness of both sums. A wrong band constant
            // moves this by tens of percent.
            check(std::abs(evaluated - direct) < direct * 0.03F,
                  "the coefficients rebuild the same irradiance the integral gives");
        }

        test::remove_tree(source.parent_path());
    }

    /**
     * The prefiltered chain leaves a constant environment constant.
     *
     * Blurring something uniform must change nothing, at any roughness. That
     * catches the errors importance sampling is prone to: a lobe that does not
     * integrate to one, a weight left out of the divisor, or rays drawn from
     * the wrong hemisphere. All three would darken or brighten the lower levels
     * while a picture still looked plausible.
     */
    void test_a_constant_environment_survives_the_prefilter() {
        const std::filesystem::path source = scratch("prefilter/src");
        const std::filesystem::path out = scratch("prefilter/out");

        constexpr float kRadiance = 0.5F;
        write_file(source / "flat.hdr", constant_panorama(64, 32, 128, 128));
        write_file(source / "flat.hdr.meta", R"({
  "__version": 4,
  "guid": "9a4b1e77-2c60-4d8f-b3a5-6f1e0c9d8a22",
  "environment": { "__version": 1, "face_size": 8, "specular_samples": 64 }
})");

        engine::import::Options options{ .content = source, .out = out };
        engine::import::Result result;
        check(engine::import::cook_all(options, result), "the environment cooks");

        const std::string bytes = read_file(out / "flat.hdr.tex");
        engine::assets::TextureView view;
        check(engine::assets::read_texture(
                  std::as_bytes(std::span(bytes.data(), bytes.size())), view, "flat"),
              "the cubemap reads back");

        // Every texel of every face and every level, because a roughness that
        // went wrong shows up on one level and not on the others.
        //
        // The bounds start past either end rather than at the value under test.
        // Seeding them with it would let a payload of nothing pass both checks
        // below, which is the failure a cook that wrote no cubemap gives.
        float lowest = std::numeric_limits<float>::max();
        float highest = std::numeric_limits<float>::lowest();
        const auto* texels = reinterpret_cast<const std::uint16_t*>(view.payload.data());
        const std::size_t count = view.payload.size() / sizeof(std::uint16_t);
        check(count > 0, "the cubemap holds texels to look at");
        if (count == 0) {
            test::remove_tree(source.parent_path());
            return;
        }
        for (std::size_t i = 0; i < count; ++i) {
            // Channel 3 is alpha, which the source carries as 1 and the filter
            // averages the same way. Only the colour channels are asserted.
            if (i % 4 == 3) {
                continue;
            }
            const float value = from_half(texels[i]);
            lowest = std::min(lowest, value);
            highest = std::max(highest, value);
        }

        check(std::abs(lowest - kRadiance) < 0.02F,
              "no texel of the prefiltered chain fell below the constant");
        check(std::abs(highest - kRadiance) < 0.02F,
              "and none rose above it");

        test::remove_tree(source.parent_path());
    }

    /**
     * The irradiance is a sub-asset, so it derives its identity.
     *
     * A scene names an environment by one GUID. The renderer has to reach the
     * second part from that alone, so the number the cooker writes and the
     * number a consumer derives must be the same one.
     */
    void test_the_irradiance_derives_its_identity() {
        const std::filesystem::path source = scratch("shguid/src");
        const std::filesystem::path out = scratch("shguid/out");

        write_file(source / "sky.hdr", constant_panorama(16, 8, 128, 128));
        write_file(source / "sky.hdr.meta", R"({
  "__version": 4,
  "guid": "5d8e3a11-90bb-4c27-8e14-7a2f6b0d4e55",
  "environment": { "__version": 1, "face_size": 4, "specular_samples": 8 }
})");

        engine::import::Options options{ .content = source, .out = out };
        engine::import::Result result;
        check(engine::import::cook_all(options, result), "the environment cooks");

        engine::assets::Manifest manifest;
        check(engine::assets::load_manifest(out, manifest), "the manifest reads");

        const engine::assets::ManifestEntry* entry =
            engine::assets::find_by_source(manifest, "sky.hdr");
        check(entry != nullptr, "the panorama has an entry");
        if (entry == nullptr) {
            test::remove_tree(source.parent_path());
            return;
        }
        check(entry->outputs.size() == 2, "and it wrote two parts");
        if (entry->outputs.size() != 2) {
            test::remove_tree(source.parent_path());
            return;
        }

        engine::Guid parent;
        check(engine::Guid::parse("5d8e3a11-90bb-4c27-8e14-7a2f6b0d4e55", parent),
              "the sidecar GUID parses");
        check(entry->outputs[0].guid == parent, "the cubemap keeps the source identity");

        const engine::Guid derived =
            engine::Guid::derive(parent, engine::assets::kIrradiancePartKind, 0);
        check(entry->outputs[1].guid == derived,
              "and the irradiance carries the one a consumer derives");
        check(entry->outputs[1].cooked == "sky.hdr.irr", "under the irradiance extension");

        test::remove_tree(source.parent_path());
    }

    /// A ray budget of nothing would divide by zero on every prefiltered texel.
    void test_an_empty_ray_budget_is_refused() {
        const std::filesystem::path source = scratch("norays/src");
        const std::filesystem::path out = scratch("norays/out");

        write_file(source / "sky.hdr", constant_panorama(16, 8, 128, 128));
        write_file(source / "sky.hdr.meta", R"({
  "__version": 4,
  "guid": "c4a90f38-6b21-4e55-9d07-3e8f1a2b6c77",
  "environment": { "__version": 1, "face_size": 4, "specular_samples": 0 }
})");

        engine::import::Options options{ .content = source, .out = out };
        engine::import::Result result;
        check(!engine::import::cook_all(options, result), "a ray budget of zero fails the cook");
        check(result.failed == 1, "and it counts one failure");

        test::remove_tree(source.parent_path());
    }

    /// Writes a BRDF source and its sidecar, and cooks the tree.
    [[nodiscard]] bool cook_brdf_table(const std::filesystem::path& source,
                                       const std::filesystem::path& out, const char* meta) {
        write_file(source / "ibl.brdf", "# no data, see the sidecar\n");
        write_file(source / "ibl.brdf.meta", meta);
        engine::import::Options options{ .content = source, .out = out };
        engine::import::Result result;
        return engine::import::cook_all(options, result);
    }

    /**
     * The BRDF table obeys the two things that are true of it whatever else is.
     *
     * A surface cannot reflect more light than reaches it, so the scale and the
     * bias together never pass one. And at no roughness at all the lobe is a
     * mirror, every ray survives, and the two must add to exactly one at every
     * angle.
     *
     * The second one pins the shape of the integral and the first one catches
     * the errors that make it too bright. Both are worth having, because a
     * table that is merely wrong still looks like a plausible gradient and
     * nothing downstream reports it.
     *
     * This is not idle. The Smith remapping squared alpha a second time when it
     * was first written, which stopped the term shadowing anything at a grazing
     * angle. The table then reached eight at its worst entry, and the mirror
     * check above still passed, because at no roughness there is nothing to
     * shadow.
     */
    void test_the_brdf_table_conserves_energy() {
        const std::filesystem::path source = scratch("brdf/src");
        const std::filesystem::path out = scratch("brdf/out");

        constexpr std::uint32_t kSize = 64;
        check(cook_brdf_table(source, out, R"({
  "__version": 5,
  "guid": "0b3c8e51-46a2-4d77-9f18-2c7a5e9b1d04",
  "brdf": { "__version": 1, "size": 64, "samples": 256 }
})"),
              "the BRDF table cooks");

        const std::string bytes = read_file(out / "ibl.brdf.tex");
        engine::assets::TextureView view;
        check(engine::assets::read_texture(
                  std::as_bytes(std::span(bytes.data(), bytes.size())), view, "brdf"),
              "and the runtime reader accepts it");
        check(view.width == kSize && view.height == kSize, "it is the size the sidecar asked for");
        check(view.mip_count == 1, "and it carries one level, because roughness is an axis");
        check(view.format == engine::assets::TextureFormat::RGBA16F, "stored as half float");

        const std::size_t wanted = static_cast<std::size_t>(kSize) * kSize * 4;
        const std::size_t count = view.payload.size() / sizeof(std::uint16_t);
        check(count == wanted, "the payload holds every entry");
        if (count != wanted) {
            test::remove_tree(source.parent_path());
            return;
        }

        const auto* texels = reinterpret_cast<const std::uint16_t*>(view.payload.data());
        std::size_t over_unity = 0;
        std::size_t negative = 0;
        float worst_sum = 0.0F;

        for (std::uint32_t y = 0; y < kSize; ++y) {
            for (std::uint32_t x = 0; x < kSize; ++x) {
                const std::size_t at = ((static_cast<std::size_t>(y) * kSize) + x) * 4;
                const float scale = from_half(texels[at]);
                const float bias = from_half(texels[at + 1]);

                if (scale < 0.0F || bias < 0.0F) {
                    ++negative;
                }
                // A little over one is the ray budget rather than the maths.
                if (scale + bias > 1.01F) {
                    ++over_unity;
                }
                worst_sum = std::max(worst_sum, scale + bias);
            }
        }

        check(negative == 0, "no entry of the table is negative");
        check(over_unity == 0, "and none reflects more light than reaches it");
        check(worst_sum <= 1.01F, "so the whole table stays inside unity");

        // The first row is the smoothest roughness the table carries. A mirror
        // loses nothing, so the split has to add back up to one.
        for (std::uint32_t x = 0; x < kSize; ++x) {
            const float scale = from_half(texels[x * 4]);
            const float bias = from_half(texels[(x * 4) + 1]);
            check(std::abs((scale + bias) - 1.0F) < 0.02F,
                  "a mirror keeps all of what it reflects, whichever way it is seen");
        }

        test::remove_tree(source.parent_path());
    }

    /// A table of no size, or built from no rays, is refused at the cook.
    void test_a_bad_brdf_sidecar_is_refused() {
        const std::filesystem::path source = scratch("brdfbad/src");
        const std::filesystem::path out = scratch("brdfbad/out");

        check(!cook_brdf_table(source, out, R"({
  "__version": 5,
  "guid": "7e4a1c93-58b0-4f26-a3d5-9c1e6b8047af",
  "brdf": { "__version": 1, "size": 0, "samples": 64 }
})"),
              "a table of no size fails the cook");
        test::remove_tree(source.parent_path());

        const std::filesystem::path second = scratch("brdfrays/src");
        const std::filesystem::path second_out = scratch("brdfrays/out");
        check(!cook_brdf_table(second, second_out, R"({
  "__version": 5,
  "guid": "1a9d5f27-3e64-4b81-8072-5f3c2a6e9d18",
  "brdf": { "__version": 1, "size": 16, "samples": 0 }
})"),
              "and a ray budget of zero fails it too");
        test::remove_tree(second.parent_path());
    }

    /// A face size the payload size field cannot hold is refused at the cook.
    void test_an_oversized_environment_face_is_refused() {
        const std::filesystem::path source = scratch("bigenv/src");
        const std::filesystem::path out = scratch("bigenv/out");

        std::string hdr = "#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y 1 +X 2\n";
        const std::array<unsigned char, 8> texels{ 255, 128, 64, 128, 64, 128, 255, 129 };
        for (const unsigned char byte : texels) {
            hdr.push_back(static_cast<char>(byte));
        }
        write_file(source / "huge.hdr", hdr);

        // One past the bound. Six faces of a mip chain at half float run past
        // what the cooked header records in 32 bits well before this.
        write_file(source / "huge.hdr.meta", R"({
  "__version": 4,
  "guid": "b1d7e6a2-4c55-4a1e-8f30-9e2c7a5d3b44",
  "environment": { "__version": 1, "face_size": 8192 }
})");

        engine::import::Options options{ .content = source, .out = out };
        engine::import::Result result;
        check(!engine::import::cook_all(options, result), "a face size past the bound fails the cook");
        check(result.failed == 1, "and it counts one failure");

        test::remove_tree(source.parent_path());
    }

} // namespace

// M13.3a. The editor's index of a source tree, checked against a cook of
// the same tree. Both name every asset, and they have to agree exactly:
// the editor draws what the index names and the runtime draws what the
// cook wrote, so a disagreement is two engines rather than one.
void test_the_index_matches_a_cook() {
    const std::filesystem::path source = scratch("index/src");
    const std::filesystem::path out = scratch("index/out");
    std::filesystem::create_directories(source / "models");

    // One of every rule that names more than one asset, plus the plain
    // ones. A glTF gives meshes and a prefab, a shader with a variant list
    // gives several forms, and an environment gives a cubemap and its
    // irradiance.
    write_file(source / "models" / "crate.gltf", kTwoMeshGltf);
    write_file(source / "many.frag", R"(#version 450
layout(location = 0) out vec4 color;
void main() { color = vec4(1.0); }
)");
    write_file(source / "many.frag.meta", R"({
  "guid": "8d3d5c1a-5e6b-4f2a-9c7d-1e2f3a4b5c6d",
  "shader": { "variants": [ { "name": "base", "defines": [] }, { "name": "normal", "defines": [ "WITH_NORMAL_MAP" ] } ] }
})");
    write_file(source / "flat.hdr", constant_panorama(64, 32, 64, 64));
    write_file(source / "flat.hdr.meta", R"({
  "guid": "1f2e3d4c-5b6a-4988-b766-554433221100",
  "environment": { "face_size": 8, "specular_samples": 4 }
})");
    write_tga(source / "wall.tga", 8, 8, std::vector<std::uint8_t>(8 * 8 * 4, 128));
    write_file(source / "start.scene",
               R"({"__version":1,"entities":[{"parent":-1,"components":{)"
               R"("MeshRenderer":{"__version":1,"mesh":"asset:models/crate.gltf#mesh:1"}}}]})");
    write_file(source / "spin.lua", "return {}\n");
    // Both storage forms of a sound. The decoded one and the streamed one take
    // different paths through the rule, so one of them alone would leave half
    // the agreement unchecked.
    write_file(source / "click.wav", wav_16_bit(2, 48000, { 0, 1, 2, 3 }));
    write_file(source / "theme.ogg", std::string(48, 'Q'));

    engine::import::Options options{ .content = source, .out = out };
    engine::import::Result result;
    check(engine::import::cook_all(options, result), "the tree cooks");

    as::Manifest manifest;
    check(as::load_manifest(out, manifest), "and the cook wrote a manifest");

    engine::import::SourceAssets index;
    check(index.open(source), "the index opens the same tree");
    check(index.failed() == 0, "and every file in it was readable");

    // Compare as sets of the whole record. Comparing counts alone would
    // pass while every name was wrong.
    std::set<std::string> cooked;
    for (const as::ManifestEntry& entry : manifest.entries) {
        for (const as::ManifestOutput& output : entry.outputs) {
            cooked.insert(output.guid.to_text() + " " + entry.source + " " + output.cooked);
        }
    }

    std::set<std::string> indexed;
    std::vector<as::AssetRecord> all;
    check(index.assets_of_kind("", all), "the index lists everything it holds");
    for (const as::AssetRecord& record : all) {
        indexed.insert(record.guid.to_text() + " " + record.source + " " + record.name);
    }

    check(!cooked.empty(), "the cook produced something to compare against");
    check(indexed == cooked, "the index names exactly what the cook wrote");

    // M13.4a. The same answer stated as a manifest, which is what the asset
    // browser, assets::reference_for and scene::restore_references all read.
    // Anything that reads a manifest then works over a source tree with no
    // change of its own.
    std::set<std::string> stated;
    for (const as::ManifestEntry& entry : index.manifest().entries) {
        for (const as::ManifestOutput& output : entry.outputs) {
            stated.insert(output.guid.to_text() + " " + entry.source + " " + output.cooked);
        }
    }
    check(stated == cooked, "and its manifest states the same thing");

    // A manifest entry also carries the identity of the source file, which is
    // not the identity of any one output when the source holds several.
    const as::ManifestEntry* gltf = as::find_by_source(index.manifest(), "models/crate.gltf");
    check(gltf != nullptr, "the glTF has a manifest entry");
    if (gltf != nullptr) {
        as::AssetMeta meta;
        check(as::meta_for(source / "models" / "crate.gltf", meta), "its sidecar reads");
        check(gltf->guid == meta.guid, "and the entry carries the identity of the file");
    }
    if (indexed != cooked) {
        for (const std::string& one : cooked) {
            if (!indexed.contains(one)) {
                ENGINE_LOG_ERROR("the cook has {} and the index does not", one);
            }
        }
        for (const std::string& one : indexed) {
            if (!cooked.contains(one)) {
                ENGINE_LOG_ERROR("the index has {} and the cook does not", one);
            }
        }
    }
}

// A source path names every part of itself, in the order the cooker wrote
// them. mesh_variant_index() indexes into that order for a shader.
void test_the_index_answers_by_source_path() {
    const std::filesystem::path source = scratch("index_path/src");
    std::filesystem::create_directories(source / "models");
    write_file(source / "models" / "crate.gltf", kTwoMeshGltf);

    engine::import::SourceAssets index;
    check(index.open(source), "the index opens");

    std::vector<as::AssetRecord> parts;
    check(index.assets_for("models/crate.gltf", parts), "the glTF is found by its path");
    check(parts.size() >= 3, "and it names its meshes and its prefab");
    check(parts[0].name == "models/crate.gltf.0.mesh", "the first mesh comes first");
    check(parts[1].name == "models/crate.gltf.1.mesh", "then the second");
    check(parts.back().name == "models/crate.gltf.0.prefab", "and the prefab comes last");

    std::vector<as::AssetRecord> missing{ parts };
    check(!index.assets_for("models/gone.gltf", missing), "a path the tree lacks is false");
    check(missing.empty(), "and the answer is cleared");
}

// A source scene names an asset by path. The editor resolves that as it
// loads, where the cooker resolves it as it copies.
void test_the_index_resolves_a_reference() {
    const std::filesystem::path source = scratch("index_ref/src");
    std::filesystem::create_directories(source / "models");
    write_file(source / "models" / "crate.gltf", kTwoMeshGltf);

    engine::import::SourceAssets index;
    check(index.open(source), "the index opens");

    std::vector<as::AssetRecord> parts;
    check(index.assets_for("models/crate.gltf", parts), "the glTF is found");

    engine::Guid resolved;
    check(index.resolve("asset:models/crate.gltf#mesh:1", resolved),
          "a reference to a part resolves");
    check(resolved == parts[1].guid, "to the identity that part goes by");

    // Against the sidecar, not against "not a part". A glTF names only
    // derived parts, so nothing in the record list is the file itself, and
    // a resolve that used the first record would pass a weaker check.
    as::AssetMeta meta;
    check(as::meta_for(source / "models" / "crate.gltf", meta), "the sidecar reads back");
    check(index.resolve("asset:models/crate.gltf", resolved),
          "a reference to the file itself resolves");
    check(resolved == meta.guid, "to the identity in its sidecar");

    check(!index.resolve("asset:models/gone.gltf#mesh:0", resolved),
          "a reference to a file the tree lacks is refused");
}

// One broken asset must not take the project with it.
void test_a_bad_file_does_not_stop_the_index() {
    const std::filesystem::path source = scratch("index_bad/src");
    std::filesystem::create_directories(source / "models");
    write_file(source / "models" / "broken.gltf", "this is not glTF");
    write_file(source / "spin.lua", "return {}\n");

    engine::import::SourceAssets index;
    check(index.open(source), "the index still opens");
    check(index.failed() == 1, "and it counts the file it could not read");

    std::vector<as::AssetRecord> scripts;
    check(index.assets_of_kind(".lua", scripts), "the rest is still there");
    check(scripts.size() == 1, "so the good asset survived the bad one");
}


// A glTF buffer is payload, not an asset. The cooker skips one and so must
// the index, or the editor would list a file the runtime never sees.
//
// The case only exists when the buffer file has an extension that carries a
// rule. A `.bin` is already skipped, because nothing gives it one, so a
// test with a `.bin` buffer would pass with the skip deleted.
void test_the_index_skips_a_gltf_buffer() {
    const std::filesystem::path source = scratch("index_buffer/src");
    std::filesystem::create_directories(source / "models");

    // The buffer is named payload.lua, which the script rule would
    // otherwise make an asset of its own.
    std::string gltf = kMinimalGltf;
    const std::size_t at = gltf.find("data:application/octet-stream;base64,");
    check(at != std::string::npos, "the fixture carries an inline buffer to replace");
    gltf = gltf.substr(0, at) + "payload.lua\"}]}";
    write_file(source / "models" / "crate.gltf", gltf);
    write_file(source / "models" / "payload.lua", "not really a script");
    write_file(source / "real.lua", "return {}\n");

    engine::import::SourceAssets index;
    check(index.open(source), "the index opens");

    std::vector<as::AssetRecord> scripts;
    check(index.assets_of_kind(".lua", scripts), "the scripts are listed");
    check(scripts.size() == 1, "and the buffer is not one of them");
    check(scripts.front().source == "real.lua", "only the real script is an asset");
}


/**
 * The editor's index writes the same sidecar the cooker would have written.
 *
 * A sidecar is written by whichever side reaches a file with none, and it
 * decides how that file is read from then on. So a guess made on one side and
 * not on the other is not a difference that shows up once. It is a wrong file
 * on disk, and nothing reports it.
 *
 * The index used to call meta_for() directly, which makes no guess at all. A
 * normal map the editor opened first was recorded as sRGB whatever its name
 * said, and every later cook honored that.
 */
void test_the_index_writes_the_same_sidecar_a_cook_would() {
    const std::filesystem::path source = scratch("index-meta/src");
    // The name is what the guess reads. This one has to come out linear, and
    // the sound has to come out streamed.
    write_tga(source / "wall_normal.tga", 2, 2, half_black_half_white());
    write_file(source / "theme.ogg", std::string(32, 'M'));

    engine::import::SourceAssets assets;
    check(assets.open(source), "the tree opens as a source project");

    as::AssetMeta image;
    check(as::load_meta(source / "wall_normal.tga", image), "the index wrote an image sidecar");
    check(image.texture.color_space == as::ColorSpace::Linear,
          "and it guessed the color space from the name, as a cook would");

    as::AssetMeta sound;
    check(as::load_meta(source / "theme.ogg", sound), "the index wrote a sound sidecar");
    check(sound.sound.stream, "and it guessed that a file it cannot decode streams");

    test::remove_tree(source.parent_path());
}

// M13.3b. The whole point of the milestone: what the editor imports and
// what the cooker writes are the same bytes. If they ever differ, the
// editor's picture and the runtime's stop being comparable and an
// offscreen capture stops being worth taking.
void test_an_import_gives_the_cooked_bytes() {
    const std::filesystem::path source = scratch("import/src");
    const std::filesystem::path out = scratch("import/out");
    std::filesystem::create_directories(source / "models");

    write_file(source / "models" / "crate.gltf", kTwoMeshGltf);
    write_file(source / "many.frag", R"(#version 450
layout(location = 0) out vec4 color;
void main() { color = vec4(1.0); }
)");
    write_file(source / "many.frag.meta", R"({
  "guid": "8d3d5c1a-5e6b-4f2a-9c7d-1e2f3a4b5c6d",
  "shader": { "variants": [ { "name": "base", "defines": [] },
                            { "name": "normal", "defines": [ "WITH_NORMAL_MAP" ] } ] }
})");
    write_file(source / "flat.hdr", constant_panorama(64, 32, 64, 64));
    write_file(source / "flat.hdr.meta", R"({
  "guid": "1f2e3d4c-5b6a-4988-b766-554433221100",
  "environment": { "face_size": 8, "specular_samples": 4 }
})");
    write_tga(source / "wall.tga", 8, 8, std::vector<std::uint8_t>(8 * 8 * 4, 128));
    write_file(source / "start.scene",
               R"({"__version":1,"entities":[{"parent":-1,"components":{)"
               R"("MeshRenderer":{"__version":1,"mesh":"asset:models/crate.gltf#mesh:1"}}}]})");
    write_file(source / "spin.lua", "return {}\n");

    engine::import::Options options{ .content = source, .out = out };
    engine::import::Result result;
    check(engine::import::cook_all(options, result), "the tree cooks");

    as::Manifest manifest;
    check(as::load_manifest(out, manifest), "and the cook wrote a manifest");

    engine::import::SourceAssets assets;
    check(assets.open(source), "the same tree opens as a source project");
    check(assets.imports() == 0, "and opening it imports nothing");

    // Every asset the cook produced, read back through the import and
    // compared against the file on disk. Byte for byte, not by size.
    std::size_t compared = 0;
    bool all_same = true;
    for (const as::ManifestEntry& entry : manifest.entries) {
        for (const as::ManifestOutput& output : entry.outputs) {
            std::vector<std::byte> imported;
            if (!assets.read(output.guid, imported)) {
                ENGINE_LOG_ERROR("{} would not import", output.cooked);
                all_same = false;
                continue;
            }
            const std::string cooked = read_file(out / output.cooked);
            const std::string got{ reinterpret_cast<const char*>(imported.data()),
                                   imported.size() };
            if (cooked != got) {
                ENGINE_LOG_ERROR("{}: cooked {} bytes and imported {}", output.cooked,
                                 cooked.size(), got.size());
                all_same = false;
            }
            ++compared;
        }
    }

    check(compared >= 8, "every cooked asset was compared");
    check(all_same, "an imported asset is byte for byte the cooked one");
}

// A glTF holds several assets and one rule makes them all, so asking for
// any of them must import the file once.
void test_an_import_is_kept_for_the_session() {
    const std::filesystem::path source = scratch("import_cache/src");
    std::filesystem::create_directories(source / "models");
    write_file(source / "models" / "crate.gltf", kTwoMeshGltf);

    engine::import::SourceAssets assets;
    check(assets.open(source), "the project opens");

    std::vector<as::AssetRecord> parts;
    check(assets.assets_for("models/crate.gltf", parts), "the glTF is found");
    check(assets.imports() == 0, "and naming it imports nothing");

    std::vector<std::byte> first;
    check(assets.read(parts[0].guid, first), "the first mesh imports");
    check(assets.imports() == 1, "which ran the rule once");

    // Every other part of the same glTF is already in hand.
    for (const as::AssetRecord& part : parts) {
        std::vector<std::byte> bytes;
        check(assets.read(part.guid, bytes), "every other part reads");
    }
    check(assets.imports() == 1, "and none of them ran the rule again");

    std::vector<std::byte> again;
    check(assets.read(parts[0].guid, again), "the first mesh reads a second time");
    check(again == first, "and gives the same bytes");
}

// One asset that will not import must not take the editor with it.
void test_a_failed_import_does_not_stop_the_project() {
    const std::filesystem::path source = scratch("import_bad/src");
    std::filesystem::create_directories(source / "models");
    write_file(source / "models" / "broken.gltf", kTwoMeshGltf);
    write_file(source / "spin.lua", "return {}\n");

    engine::import::SourceAssets assets;
    check(assets.open(source), "the project opens");

    std::vector<as::AssetRecord> parts;
    check(assets.assets_for("models/broken.gltf", parts), "the glTF is indexed");

    // Break it after the index read it, so the import is what fails.
    write_file(source / "models" / "broken.gltf", "this is not glTF any more");

    std::vector<std::byte> bytes;
    check(!assets.read(parts.front().guid, bytes), "the import fails and says so");

    std::vector<as::AssetRecord> scripts;
    check(assets.assets_of_kind(".lua", scripts), "the project still answers");
    check(scripts.size() == 1, "and the good asset is still there");
    std::vector<std::byte> script;
    check(assets.read(scripts.front().guid, script), "and it still imports");
}


// M13.5. A source file that changes is imported again. There is no cook
// here, so a change is a cache entry to drop and nothing more.
void test_a_changed_file_is_imported_again() {
    const std::filesystem::path source = scratch("reimport/src");
    std::filesystem::create_directories(source);
    write_tga(source / "wall.tga", 8, 8, std::vector<std::uint8_t>(8 * 8 * 4, 40));

    engine::import::SourceAssets assets;
    check(assets.open(source), "the project opens");

    std::vector<as::AssetRecord> records;
    check(assets.assets_for("wall.tga", records), "the texture is there");
    const engine::Guid guid = records.front().guid;

    std::vector<std::byte> first;
    check(assets.read(guid, first), "it imports");
    check(assets.imports() == 1, "which ran the rule once");

    // A different image under the same name and the same sidecar, so the
    // identity does not move and only the bytes do.
    write_tga(source / "wall.tga", 8, 8, std::vector<std::uint8_t>(8 * 8 * 4, 220));

    std::vector<as::AssetChange> changed;
    check(assets.reload({ "wall.tga" }, changed), "the change is taken");
    check(changed.size() == 1, "and it names one asset to load again");
    check(changed.front().guid == guid, "the same asset, because the sidecar kept it");
    check(!changed.front().gone, "and it did not go away");

    std::vector<std::byte> second;
    check(assets.read(guid, second), "it imports again");
    check(assets.imports() == 2, "which ran the rule a second time");
    check(second != first, "and the bytes are the new ones");
}

// A change can add an asset or take one away, not only replace bytes.
void test_a_reload_reports_what_came_and_went() {
    const std::filesystem::path source = scratch("reimport_shape/src");
    std::filesystem::create_directories(source / "models");
    write_file(source / "models" / "crate.gltf", kMinimalGltf);
    write_file(source / "spin.lua", "return {}\n");

    engine::import::SourceAssets assets;
    check(assets.open(source), "the project opens");

    std::vector<as::AssetRecord> before;
    check(assets.assets_for("models/crate.gltf", before), "the glTF is there");
    const std::size_t one_mesh = before.size();

    // The same file with a second mesh in it.
    write_file(source / "models" / "crate.gltf", kTwoMeshGltf);
    std::vector<as::AssetChange> changed;
    check(assets.reload({ "models/crate.gltf" }, changed), "the change is taken");

    std::vector<as::AssetRecord> after;
    check(assets.assets_for("models/crate.gltf", after), "the glTF is still there");
    check(after.size() > one_mesh, "and it names more than it did");
    check(changed.size() >= after.size(), "the reload named every part of it");

    // And a file that goes away is reported as gone, so a cache holding it
    // knows to let it go.
    std::vector<as::AssetRecord> script;
    check(assets.assets_of_kind(".lua", script), "the script is there");
    const engine::Guid script_guid = script.front().guid;
    std::filesystem::remove(source / "spin.lua");
    std::filesystem::remove(source / "spin.lua.meta");

    check(assets.reload({ "spin.lua" }, changed), "the delete is taken");
    bool reported_gone = false;
    for (const as::AssetChange& one : changed) {
        if (one.guid == script_guid && one.gone) {
            reported_gone = true;
        }
    }
    check(reported_gone, "and the asset that went away is reported gone");
}


int main() {
    test::section("hashing");
    test_hash_is_content_not_time();
    test_input_order_matters();
    test::section("a cook that fails leaves the tree as it was");
    test_a_discarded_write_leaves_the_file_it_would_have_replaced();
    test_a_failed_gltf_leaves_the_earlier_mesh_alone();
    test_a_failed_shader_variant_leaves_the_base_form_alone();
    test::section("cooking");
    test_cook_and_skip();
    test_a_script_cooks_as_source_text();
    test_missing_output_recooks();
    test_new_identity_recooks();
    test_duplicate_identity_is_refused();
    test_a_shell_metacharacter_name_still_cooks();
    test_documentation_is_not_an_asset();
    test_a_file_with_no_rule_is_not_content();
    test_a_font_and_a_layout_are_content();
    test_a_layout_names_its_image_by_identity();
    test_a_layout_naming_a_missing_image_fails();
    test_a_layout_with_no_image_still_cooks();
    test_a_layout_climbing_out_of_the_tree_fails();
    test_a_rule_ignores_the_case_of_the_extension();
    test_bad_input();
    test::section("shader reflection");
    test_the_cooker_reflects_what_a_shader_reads();
    test_a_shader_cooks_once_for_each_variant();
    test_an_hdr_panorama_cooks_to_a_cubemap();
    test_a_constant_environment_integrates_to_pi();
    test_the_irradiance_matches_a_direct_integral();
    test_a_constant_environment_survives_the_prefilter();
    test_the_irradiance_derives_its_identity();
    test_an_empty_ray_budget_is_refused();
    test_the_brdf_table_conserves_energy();
    test_a_bad_brdf_sidecar_is_refused();
    test_an_oversized_environment_face_is_refused();
    test_a_variant_list_that_starts_with_defines_is_refused();
    test_a_push_block_that_starts_late_reports_its_real_size();
    test_a_shader_that_does_not_compile_writes_nothing();
    test::section("textures");
    test_color_space_decides_the_mip_chain();
    test_editing_the_sidecar_cooks_again();
    test::section("sounds");
    test_a_sound_cooks_both_ways();
    test_a_sound_that_cannot_decode_is_refused();
    test_an_older_manifest_cooks_again();
    test_an_older_cooker_cooks_again();
    test_compression_and_mip_switches();
    test_awkward_sizes();
    test_a_broken_image_fails_the_cook();
    test_a_failed_cook_keeps_the_asset_it_had();
    test_a_failed_cook_drops_the_entry_when_its_output_is_gone();
    test::section("reading it back");
    test_content_reads_what_the_cooker_wrote();
    test::section("hot reload");
    test_reload_names_only_what_changed();
    test_reload_names_an_asset_that_went_away();
    test_a_reload_says_what_an_asset_that_went_away_was();
    test_a_reload_says_what_a_changed_asset_is();
    test_reload_keeps_what_it_has_when_the_manifest_will_not_read();
    test_hot_reload_cooks_what_changed();
    test_hot_reload_lives_through_a_cook_that_fails();
    test_hot_reload_is_off_when_it_cannot_cook();
    test::section("the source index");
    test_the_index_matches_a_cook();
    test_the_index_answers_by_source_path();
    test_the_index_resolves_a_reference();
    test_a_bad_file_does_not_stop_the_index();
    test_the_index_skips_a_gltf_buffer();
    test::section("importing from source");
    test_the_index_writes_the_same_sidecar_a_cook_would();
    test_an_import_gives_the_cooked_bytes();
    test_an_import_is_kept_for_the_session();
    test_a_failed_import_does_not_stop_the_project();
    test::section("reimporting");
    test_a_changed_file_is_imported_again();
    test_a_reload_reports_what_came_and_went();
    test::section("asset references");
    test_a_reference_reads_into_its_parts();
    test_a_document_names_a_mesh_by_path();
    test_a_document_keeps_a_guid_that_is_already_written();
    test_a_reference_that_names_nothing_fails_the_cook();
    test_a_reference_that_will_not_read_fails_the_cook();
    test_a_reference_that_leaves_the_content_tree_is_refused();
    test_a_part_that_is_not_there_fails_the_cook();
    test_a_reference_stops_being_sound_when_the_model_changes();
    test_an_identity_reads_back_as_the_reference_that_named_it();
    test_a_part_with_a_sparse_index_reads_back();
    test_a_saved_document_keeps_its_references();
    test_a_name_that_holds_an_identity_survives_a_save();
    test_the_save_walk_reaches_an_instance_patch();
    test_a_game_component_resolves_its_reference();
    test_an_unregistered_component_fails_the_cook();
    test_a_reference_in_an_untagged_field_fails_the_cook();
    test_a_name_that_is_not_a_reference_cooks();
    test_a_document_that_will_not_parse_fails_the_cook();
    test_a_new_sidecar_cooks_the_document_that_names_it();
    return test::report();
}
