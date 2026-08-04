// M4.2 tests for the cooker and the manifest.
//
// The property that carries the milestone is the incremental check. A cook
// that redoes everything wastes a minute today and an hour at M4.4, and a cook
// that skips too much ships a stale asset. Both failures are quiet, so the
// tests here drive the second run and check the counts rather than the output.
//
// test_a_shell_metacharacter_name_still_cooks is the one test that cooks a shader.
// It needs glslc, which the CMake test target passes through a compile
// definition. The copy rule exercises the same manifest path for the rest of
// the tests.

#include "assets/content.h"
#include "assets/hot_reload.h"
#include "assets/manifest.h"
#include "assets/meta.h"
#include "assets/reference.h"
#include "assets/shader.h"
#include "assets/texture.h"
#include "check.h"
#include "cook.h"
#include "document.h"
#include "platform/paths.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

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

    std::filesystem::path scratch(std::string_view name) {
        const std::filesystem::path path =
            std::filesystem::temp_directory_path() / "camina_test_cooker" / name;
        test::remove_tree(path);
        std::filesystem::create_directories(path);
        return path;
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
        // A file with no rule of its own, so this stays a test of the copy
        // rule and of the manifest. A scene and a prefab have a rule now.
        write_file(source / "one.dat", "{}");
        write_file(source / "nested" / "two.dat", "{}");

        cooker::Options options{ .content = source, .out = out };
        cooker::Result result;
        check(cooker::cook_all(options, result), "the first cook works");
        check(result.cooked == 2 && result.skipped == 0, "and it cooks both assets");
        check(std::filesystem::exists(out / "one.dat"), "the asset landed");
        check(std::filesystem::exists(out / "nested" / "two.dat"),
              "and so did the one in a subdirectory");

        // The sidecars are what make an identity survive. A first cook writes
        // them into the source tree, next to the asset.
        check(std::filesystem::exists(as::meta_path(source / "one.dat")),
              "the first cook wrote a sidecar");

        cooker::Result second;
        check(cooker::cook_all(options, second), "the second cook works");
        check(second.cooked == 0 && second.skipped == 2, "and it cooks nothing");

        // Touching a file moves its time but not its bytes.
        std::filesystem::last_write_time(source / "one.dat",
                                         std::filesystem::file_time_type::clock::now());
        cooker::Result touched;
        check(cooker::cook_all(options, touched), "a touched tree cooks");
        check(touched.cooked == 0 && touched.skipped == 2, "and a new time alone cooks nothing");

        // A real change cooks that asset, and only that asset.
        write_file(source / "one.dat", "{\"changed\":true}");
        cooker::Result changed;
        check(cooker::cook_all(options, changed), "a changed tree cooks");
        check(changed.cooked == 1 && changed.skipped == 1, "and it cooks only what changed");
        check(read_file(out / "one.dat") == "{\"changed\":true}",
              "the new bytes reached the output");

        // --force is the escape hatch when somebody distrusts the manifest.
        cooker::Options forced = options;
        forced.force = true;
        cooker::Result all;
        check(cooker::cook_all(forced, all), "a forced cook works");
        check(all.cooked == 2 && all.skipped == 0, "and it cooks everything again");

        test::remove_tree(source.parent_path());
    }

    void test_missing_output_recooks() {
        const std::filesystem::path source = scratch("gone/src");
        const std::filesystem::path out = scratch("gone/out");
        write_file(source / "one.scene", "{}");

        const cooker::Options options{ .content = source, .out = out };
        cooker::Result first;
        check(cooker::cook_all(options, first), "the first cook works");

        // Somebody deleted the cooked file but left the manifest. The entry is
        // stale even though every input still hashes the same.
        std::filesystem::remove(out / "one.scene");
        cooker::Result second;
        check(cooker::cook_all(options, second), "the second cook works");
        check(second.cooked == 1, "a missing output cooks again");
        check(std::filesystem::exists(out / "one.scene"), "and the file came back");

        test::remove_tree(source.parent_path());
    }

    void test_new_identity_recooks() {
        const std::filesystem::path source = scratch("ident/src");
        const std::filesystem::path out = scratch("ident/out");
        write_file(source / "one.scene", "{}");

        const cooker::Options options{ .content = source, .out = out };
        cooker::Result first;
        check(cooker::cook_all(options, first), "the first cook works");

        as::Manifest before;
        check(as::load_manifest(out, before), "the manifest reads back");
        const as::ManifestEntry* entry = as::find_by_source(before, "one.scene");
        check(entry != nullptr && entry->guid.valid(), "the entry carries an identity");

        // Deleting the sidecar gives the asset a new identity. Every reference
        // to it has to see the new one, so the entry cannot be reused.
        std::filesystem::remove(as::meta_path(source / "one.scene"));
        cooker::Result second;
        check(cooker::cook_all(options, second), "the second cook works");
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

        cooker::Options options{ .content = source, .out = out };
        cooker::Result first;
        check(cooker::cook_all(options, first), "the first cook works");
        check(first.cooked == 1, "it cooked one asset");

        std::filesystem::copy_file(source / "one.scene", source / "copy.scene");
        const auto sidecar = as::meta_path(source / "one.scene");
        const auto copied_sidecar = as::meta_path(source / "copy.scene");
        std::filesystem::copy_file(sidecar, copied_sidecar);

        cooker::Result second;
        check(!cooker::cook_all(options, second), "a duplicate identity fails the cook");

        test::remove_tree(source.parent_path());
    }

    void test_a_shell_metacharacter_name_still_cooks() {
#if defined(ENGINE_GLSLC_PATH)
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

        cooker::Options options{ .content = source, .out = out };
        options.glslc = ENGINE_GLSLC_PATH;
        cooker::Result result;
        check(cooker::cook_all(options, result), "a name a shell would expand now cooks");
        check(result.cooked == 1, "it counted the cook");
        check(std::filesystem::exists(out / shader_part(name)),
              "the cooked shader was written");

        test::remove_tree(source.parent_path());
#endif
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
    void test_a_shader_cooks_once_for_each_variant() {
#if defined(ENGINE_GLSLC_PATH)
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

        cooker::Options options{ .content = source, .out = out };
        options.glslc = ENGINE_GLSLC_PATH;
        cooker::Result result;
        check(cooker::cook_all(options, result), "a shader with two variants cooks");

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
#endif
    }

    /// The base form has to be first, because it keeps the source's identity.
    void test_a_variant_list_that_starts_with_defines_is_refused() {
#if defined(ENGINE_GLSLC_PATH)
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

        cooker::Options options{ .content = source, .out = out };
        options.glslc = ENGINE_GLSLC_PATH;
        cooker::Result result;
        check(!cooker::cook_all(options, result),
              "a list whose first variant defines something fails the cook");
        check(result.failed == 1, "and it counts one failure");
        check(!std::filesystem::exists(out / shader_part("first.frag", 0)),
              "and it wrote no module");

        test::remove_tree(source.parent_path());
#endif
    }

    void test_the_cooker_reflects_what_a_shader_reads() {
#if defined(ENGINE_GLSLC_PATH)
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

        cooker::Options options{ .content = source, .out = out };
        options.glslc = ENGINE_GLSLC_PATH;
        cooker::Result result;
        check(cooker::cook_all(options, result), "a shader with real bindings cooks");

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
#endif
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
#if defined(ENGINE_GLSLC_PATH)
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

        cooker::Options options{ .content = source, .out = out };
        options.glslc = ENGINE_GLSLC_PATH;
        cooker::Result result;
        check(cooker::cook_all(options, result), "a shader with a late push block cooks");

        const std::string cooked =
            read_file(out / shader_part("late.frag"));
        engine::assets::Shader shader;
        check(engine::assets::read_shader(
                  std::as_bytes(std::span(cooked.data(), cooked.size())), shader, "late.frag"),
              "the cooked shader reads back");
        check(shader.push_constant_size == 128,
              "the push range ends at 128, not at the offset plus the size");

        test::remove_tree(source.parent_path());
#endif
    }

    /**
     * A shader glslc will not compile must fail the cook, not write a file.
     *
     * The reflection runs after glslc, so a broken shader has to stop before it
     * and leave nothing behind. A cooked file with no module in it would fail
     * later, at pipeline build, with a message that names no source line.
     */
    void test_a_shader_that_does_not_compile_writes_nothing() {
#if defined(ENGINE_GLSLC_PATH)
        const std::filesystem::path source = scratch("broken_shader/src");
        const std::filesystem::path out = scratch("broken_shader/out");
        write_file(source / "bad.frag", "#version 450\nthis is not GLSL\n");

        cooker::Options options{ .content = source, .out = out };
        options.glslc = ENGINE_GLSLC_PATH;
        cooker::Result result;
        check(!cooker::cook_all(options, result), "a shader that will not compile fails the cook");
        check(result.failed == 1, "and it counts one failure");
        check(!std::filesystem::exists(out / shader_part("bad.frag")),
              "no cooked shader was written");
        // glslc writes its module beside the cooked file, and the rule removes
        // it on every path. One left behind would grow the cooked tree forever.
        check(!std::filesystem::exists(out / (shader_part("bad.frag") + ".spv")),
              "and the compiler output did not stay behind");

        test::remove_tree(source.parent_path());
#endif
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

        const cooker::Options options{ .content = source, .out = out };
        cooker::Result result;
        check(cooker::cook_all(options, result), "both textures cook");
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

    void test_editing_the_sidecar_cooks_again() {
        const std::filesystem::path source = scratch("edit/src");
        const std::filesystem::path out = scratch("edit/out");
        const std::filesystem::path image = source / "plate.tga";
        write_tga(image, 2, 2, half_black_half_white());

        const cooker::Options options{ .content = source, .out = out };
        cooker::Result first;
        check(cooker::cook_all(options, first), "the first cook works");
        check(first.cooked == 1, "and it cooks the texture");

        cooker::Result second;
        check(cooker::cook_all(options, second), "the second cook works");
        check(second.skipped == 1, "and it cooks nothing");

        // The sidecar is an input, not only a place to keep the identity. An
        // edit that changes how a texture is read has to cook it again, or the
        // edit looks like it did nothing at all.
        as::AssetMeta meta;
        check(as::load_meta(image, meta), "the sidecar reads");
        meta.texture.color_space = as::ColorSpace::Linear;
        check(as::save_meta(image, meta), "and the edit writes");

        cooker::Result third;
        check(cooker::cook_all(options, third), "the third cook works");
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

        const cooker::Options options{ .content = source, .out = out };
        cooker::Result first;
        check(cooker::cook_all(options, first), "the first cook works");

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

        cooker::Result second;
        check(cooker::cook_all(options, second), "the second cook works");
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

        const cooker::Options options{ .content = source, .out = out };
        cooker::Result first;
        check(cooker::cook_all(options, first), "the first cook works");
        check(first.cooked == 1, "and it cooks the asset");

        cooker::Result second;
        check(cooker::cook_all(options, second), "the second cook works");
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

        cooker::Result third;
        check(cooker::cook_all(options, third), "the third cook works");
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

        cooker::Result fourth;
        check(cooker::cook_all(options, fourth), "the fourth cook works");
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

        const cooker::Options options{ .content = source, .out = out };
        cooker::Result result;
        check(cooker::cook_all(options, result), "the texture cooks");

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

        cooker::Result again;
        check(cooker::cook_all(options, again), "it cooks again");
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

        const cooker::Options options{ .content = source, .out = out };
        cooker::Result result;
        check(cooker::cook_all(options, result), "every awkward size cooks");
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

        const cooker::Options options{ .content = source, .out = out };
        cooker::Result first;
        check(cooker::cook_all(options, first), "the first cook works");

        as::Content before;
        check(before.open(out), "the cooked directory opens");
        check(before.find("wall.tga") != nullptr, "and it holds the asset");

        // Break the source and cook again. A rule that fails writes no output,
        // so the cooked file from the first run is still there and still good.
        write_file(source / "wall.tga", "this is not a TGA at all");
        cooker::Result second;
        check(!cooker::cook_all(options, second), "the cook after the break fails");
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

        const cooker::Options options{ .content = source, .out = out };
        cooker::Result first;
        check(cooker::cook_all(options, first), "the first cook works");

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

        cooker::Result second;
        check(!cooker::cook_all(options, second), "the cook after the break fails");

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

        const cooker::Options options{ .content = source, .out = out };
        cooker::Result result;
        check(!cooker::cook_all(options, result), "a file stb cannot read fails the cook");
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
        const cooker::Options options{ .content = source, .out = out };
        cooker::Result result;
        check(cooker::cook_all(options, result), "the cook works");
        check(result.cooked == 1, "and it cooks the asset and not the documentation");
        check(!std::filesystem::exists(out / "README.md"), "the README was not copied through");
        check(!std::filesystem::exists(out / "LICENSE"), "nor a file with no extension");
        check(!std::filesystem::exists(out / "notes.txt"), "nor a text file");
        check(!std::filesystem::exists(as::meta_path(source / "README.md")),
              "and no sidecar was written beside it");

        test::remove_tree(source.parent_path());
    }

    void test_bad_input() {
        const std::filesystem::path out = scratch("bad/out");
        cooker::Result result;

        const cooker::Options missing{ .content = out / "not_there", .out = out };
        check(!cooker::cook_all(missing, result), "a content directory that is not there fails");

        test::remove_tree(out.parent_path());
    }

    void test_content_reads_what_the_cooker_wrote() {
        const std::filesystem::path source = scratch("read/src");
        const std::filesystem::path out = scratch("read/out");
        write_file(source / "one.dat", "{}");
        // Four bytes, so it is a whole number of 32-bit words.
        write_file(source / "words.bin", "abcd");

        const cooker::Options options{ .content = source, .out = out };
        cooker::Result result;
        check(cooker::cook_all(options, result), "the cook works");

        as::Content content;
        check(content.open(out), "the cooked directory opens");
        check(content.manifest().entries.size() == 2, "and it holds both entries");

        const as::ManifestEntry* entry = content.find("one.dat");
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

        std::vector<std::uint32_t> words;
        check(content.read_words("words.bin", words), "a four-byte asset reads as words");
        check(words.size() == 1, "and it is one word");

        // Two bytes is not a whole number of words. Catching that here beats a
        // driver rejecting the module later with a message that names nothing.
        check(!content.read_words("one.dat", words),
              "an asset that is not a whole number of words is refused");
        check(!content.read_words("not_there", words), "an unknown source path is refused");

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

        const cooker::Options options{ .content = source, .out = out };
        cooker::Result first;
        check(cooker::cook_all(options, first), "the first cook works");

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
        cooker::Result second;
        check(cooker::cook_all(options, second), "the second cook works");
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

        const cooker::Options options{ .content = source, .out = out };
        cooker::Result first;
        check(cooker::cook_all(options, first), "the first cook works");

        as::Content content;
        check(content.open(out), "the content opens");
        const engine::Guid prefab = identity_of(content, "b.prefab");

        // A cache holding an asset the content no longer has would keep
        // drawing it, so the reload has to report it.
        std::filesystem::remove(source / "b.prefab");
        std::filesystem::remove(as::meta_path(source / "b.prefab"));
        cooker::Result second;
        check(cooker::cook_all(options, second), "the cook after the delete works");

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

        const cooker::Options options{ .content = source, .out = out };
        cooker::Result first;
        check(cooker::cook_all(options, first), "the first cook works");

        as::Content content;
        check(content.open(out), "the content opens");

        // Delete the prefab. It is not in the new manifest, so nothing can be
        // looked up about it afterwards. Before this the runtime could not tell
        // a deleted prefab from a deleted texture, and the world it built from
        // that prefab stood until a restart.
        std::filesystem::remove(source / "b.prefab");
        std::filesystem::remove(as::meta_path(source / "b.prefab"));
        cooker::Result second;
        check(cooker::cook_all(options, second), "the cook after the delete works");

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

        const cooker::Options options{ .content = source, .out = out };
        cooker::Result first;
        check(cooker::cook_all(options, first), "the first cook works");

        as::Content content;
        check(content.open(out), "the content opens");

        write_file(source / "a.scene", R"({"changed":true})");
        cooker::Result second;
        check(cooker::cook_all(options, second), "the second cook works");

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

        const cooker::Options options{ .content = source, .out = out };
        cooker::Result first;
        check(cooker::cook_all(options, first), "the first cook works");

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

        const cooker::Options options{ .content = source, .out = out };
        cooker::Result first;
        check(cooker::cook_all(options, first), "the first cook works");

        as::Content content;
        check(content.open(out), "the content opens");
        const engine::Guid scene = identity_of(content, "a.scene");

        as::HotReload reload;
        check(reload.start({ .source = source, .cooker = cooker_program(), .glslc = {} }),
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

        const cooker::Options options{ .content = source, .out = out };
        cooker::Result first;
        check(cooker::cook_all(options, first), "the first cook works");

        as::Content content;
        check(content.open(out), "the content opens");
        const engine::Guid scene = identity_of(content, "a.scene");

        as::HotReload reload;
        check(reload.start({ .source = source, .cooker = cooker_program(), .glslc = {} }),
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
        check(!missing_cooker.start({ .source = source, .cooker = source / "no_cooker", .glslc = {} }),
              "hot reload will not start without a cooker");
        check(!missing_cooker.active(), "and it reports itself off");
        check(!missing_cooker.poll(content, changed), "polling it does nothing");

        as::HotReload missing_source;
        check(!missing_source.start({ .source = source / "not_here",
                                      .cooker = cooker_program(),
                                      .glslc = {} }),
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

        const cooker::Options options{ .content = source, .out = out };
        cooker::Result result;
        check(cooker::cook_all(options, result), "the cook works");

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

        const cooker::Options options{ .content = source, .out = out };
        cooker::Result result;
        check(cooker::cook_all(options, result), "the cook works");
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

        const cooker::Options options{ .content = source, .out = out };
        cooker::Result result;
        // The whole point. Before this a wrong identity drew nothing and
        // reported one line at runtime, which looks exactly like a mesh that
        // failed to upload.
        check(!cooker::cook_all(options, result), "a reference to a file that is not there fails");
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

        const cooker::Options options{ .content = source, .out = out };
        cooker::Result result;
        // Passing it through as an ordinary string is the failure to avoid.
        // The cooked prefab would then hold "asset:models/crate.gltf#mesh"
        // where a GUID goes, and the mesh would simply never load.
        check(!cooker::cook_all(options, result),
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

        const cooker::Options options{ .content = source, .out = out };
        cooker::Result result;
        // Guid::derive answers for any index, so this used to cook happily and
        // give the prefab an identity nothing wrote. The scene then drew
        // nothing, which is the failure naming an asset by path is meant to
        // remove.
        check(!cooker::cook_all(options, result),
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

        const cooker::Options options{ .content = source, .out = out };
        cooker::Result first;
        check(cooker::cook_all(options, first), "the first cook works");

        // Replace the model with one that holds a single mesh. It is a valid
        // model and it cooks, so nothing fails on its own account. The prefab
        // still says mesh 1 and nobody edited it, so nothing about the prefab
        // looks stale either. Only the finished manifest can say the identity
        // it names is gone.
        write_file(source / "models" / "crate.gltf", kMinimalGltf);
        cooker::Result second;
        check(!cooker::cook_all(options, second),
              "a model that lost the part fails the cook of the document naming it");
        check(second.failed == 1, "and the failure is the document, not the model");

        test::remove_tree(source.parent_path());
    }

    void test_a_document_that_will_not_parse_fails_the_cook() {
        const std::filesystem::path source = scratch("bad_document/src");
        const std::filesystem::path out = scratch("bad_document/out");
        write_file(source / "a.scene", "this is not json");

        const cooker::Options options{ .content = source, .out = out };
        cooker::Result result;
        // A scene used to be copied through, so a broken one reached the
        // runtime and emptied the world there instead.
        check(!cooker::cook_all(options, result), "a scene that will not parse fails the cook");
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

        const cooker::Options options{ .content = source, .out = out };
        cooker::Result first;
        check(cooker::cook_all(options, first), "the first cook works");
        const std::string before = cooked_mesh(out / "a.prefab");

        cooker::Result second;
        check(cooker::cook_all(options, second), "the second cook works");
        check(second.cooked == 0, "and an unchanged tree cooks nothing");

        // Replacing the sidecar gives the glTF a new identity, so every
        // identity derived from it moves. The prefab has to be cooked again or
        // it names a mesh that no longer exists.
        std::filesystem::remove(as::meta_path(source / "models" / "crate.gltf"));
        cooker::Result third;
        check(cooker::cook_all(options, third), "the cook after the sidecar went works");
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

        const cooker::Options options{ .content = source, .out = out };
        cooker::Result result;
        check(cooker::cook_all(options, result), "the cook works");

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

        const cooker::Options options{ .content = source, .out = out };
        cooker::Result result;
        check(cooker::cook_all(options, result), "the cook works");

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

        const cooker::Options options{ .content = source, .out = out };
        cooker::Result result;
        check(cooker::cook_all(options, result), "the cook works");

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

        const std::size_t restored = as::restore_references(cooked, content.manifest());
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

        cooker::Options options{ .content = source, .out = out };
        options.glslc = ENGINE_GLSLC_PATH;
        cooker::Result result;
        check(cooker::cook_all(options, result), "an HDR panorama cooks");

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

} // namespace

int main() {
    test::section("hashing");
    test_hash_is_content_not_time();
    test_input_order_matters();
    test::section("cooking");
    test_cook_and_skip();
    test_missing_output_recooks();
    test_new_identity_recooks();
    test_duplicate_identity_is_refused();
    test_a_shell_metacharacter_name_still_cooks();
    test_documentation_is_not_an_asset();
    test_bad_input();
    test::section("shader reflection");
    test_the_cooker_reflects_what_a_shader_reads();
    test_a_shader_cooks_once_for_each_variant();
    test_an_hdr_panorama_cooks_to_a_cubemap();
    test_a_variant_list_that_starts_with_defines_is_refused();
    test_a_push_block_that_starts_late_reports_its_real_size();
    test_a_shader_that_does_not_compile_writes_nothing();
    test::section("textures");
    test_color_space_decides_the_mip_chain();
    test_editing_the_sidecar_cooks_again();
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
    test_a_document_that_will_not_parse_fails_the_cook();
    test_a_new_sidecar_cooks_the_document_that_names_it();
    return test::report();
}
