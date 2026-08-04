// M4.2 tests for the cooker and the manifest.
//
// The property that carries the milestone is the incremental check. A cook
// that redoes everything wastes a minute today and an hour at M4.4, and a cook
// that skips too much ships a stale asset. Both failures are quiet, so the
// tests here drive the second run and check the counts rather than the output.
//
// test_shell_metacharacters_are_refused is the one test that cooks a shader.
// It needs glslc, which the CMake test target passes through a compile
// definition. The copy rule exercises the same manifest path for the rest of
// the tests.

#include "assets/content.h"
#include "assets/hot_reload.h"
#include "assets/manifest.h"
#include "assets/meta.h"
#include "assets/texture.h"
#include "check.h"
#include "cook.h"
#include "platform/paths.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

    using test::check;
    namespace as = engine::assets;

    std::filesystem::path scratch(std::string_view name) {
        const std::filesystem::path path =
            std::filesystem::temp_directory_path() / "camina_test_cooker" / name;
        std::filesystem::remove_all(path);
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

        std::filesystem::remove_all(dir);
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

        std::filesystem::remove_all(dir);
    }

    void test_cook_and_skip() {
        const std::filesystem::path source = scratch("skip/src");
        const std::filesystem::path out = scratch("skip/out");
        write_file(source / "one.scene", "{}");
        write_file(source / "nested" / "two.prefab", "{}");

        cooker::Options options{ .content = source, .out = out };
        cooker::Result result;
        check(cooker::cook_all(options, result), "the first cook works");
        check(result.cooked == 2 && result.skipped == 0, "and it cooks both assets");
        check(std::filesystem::exists(out / "one.scene"), "the asset landed");
        check(std::filesystem::exists(out / "nested" / "two.prefab"),
              "and so did the one in a subdirectory");

        // The sidecars are what make an identity survive. A first cook writes
        // them into the source tree, next to the asset.
        check(std::filesystem::exists(as::meta_path(source / "one.scene")),
              "the first cook wrote a sidecar");

        cooker::Result second;
        check(cooker::cook_all(options, second), "the second cook works");
        check(second.cooked == 0 && second.skipped == 2, "and it cooks nothing");

        // Touching a file moves its time but not its bytes.
        std::filesystem::last_write_time(source / "one.scene",
                                         std::filesystem::file_time_type::clock::now());
        cooker::Result touched;
        check(cooker::cook_all(options, touched), "a touched tree cooks");
        check(touched.cooked == 0 && touched.skipped == 2, "and a new time alone cooks nothing");

        // A real change cooks that asset, and only that asset.
        write_file(source / "one.scene", "{\"changed\":true}");
        cooker::Result changed;
        check(cooker::cook_all(options, changed), "a changed tree cooks");
        check(changed.cooked == 1 && changed.skipped == 1, "and it cooks only what changed");
        check(read_file(out / "one.scene") == "{\"changed\":true}", "the new bytes reached the output");

        // --force is the escape hatch when somebody distrusts the manifest.
        cooker::Options forced = options;
        forced.force = true;
        cooker::Result all;
        check(cooker::cook_all(forced, all), "a forced cook works");
        check(all.cooked == 2 && all.skipped == 0, "and it cooks everything again");

        std::filesystem::remove_all(source.parent_path());
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

        std::filesystem::remove_all(source.parent_path());
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

        std::filesystem::remove_all(source.parent_path());
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

        std::filesystem::remove_all(source.parent_path());
    }

    void test_shell_metacharacters_are_refused() {
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
        check(std::filesystem::exists(out / (std::string(name) + ".spv")),
              "the SPIR-V was written");

        std::filesystem::remove_all(source.parent_path());
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

        std::filesystem::remove_all(source.parent_path());
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

        std::filesystem::remove_all(source.parent_path());
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

        std::filesystem::remove_all(source.parent_path());
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

        std::filesystem::remove_all(source.parent_path());
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

        std::filesystem::remove_all(source.parent_path());
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

        std::filesystem::remove_all(source.parent_path());
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

        std::filesystem::remove_all(source.parent_path());
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

        std::filesystem::remove_all(source.parent_path());
    }

    void test_bad_input() {
        const std::filesystem::path out = scratch("bad/out");
        cooker::Result result;

        const cooker::Options missing{ .content = out / "not_there", .out = out };
        check(!cooker::cook_all(missing, result), "a content directory that is not there fails");

        std::filesystem::remove_all(out.parent_path());
    }

    void test_content_reads_what_the_cooker_wrote() {
        const std::filesystem::path source = scratch("read/src");
        const std::filesystem::path out = scratch("read/out");
        write_file(source / "one.scene", "{}");
        // Four bytes, so it is a whole number of 32-bit words.
        write_file(source / "words.bin", "abcd");

        const cooker::Options options{ .content = source, .out = out };
        cooker::Result result;
        check(cooker::cook_all(options, result), "the cook works");

        as::Content content;
        check(content.open(out), "the cooked directory opens");
        check(content.manifest().entries.size() == 2, "and it holds both entries");

        const as::ManifestEntry* entry = content.find("one.scene");
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
        check(!content.read_words("one.scene", words),
              "an asset that is not a whole number of words is refused");
        check(!content.read_words("not_there", words), "an unknown source path is refused");

        as::Content empty;
        check(!empty.open(out / "not_there"), "a directory with no manifest is refused");

        std::filesystem::remove_all(source.parent_path());
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

        std::vector<engine::Guid> changed;
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
        check(changed.front() == scene, "which is the one that changed");

        check(content.reload(changed) && changed.empty(),
              "reloading again names nothing, because nothing moved");

        std::filesystem::remove_all(source.parent_path());
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

        std::vector<engine::Guid> changed;
        check(content.reload(changed), "the reload reads the new manifest");
        check(changed.size() == 1, "and it names one asset");
        if (changed.size() != 1) {
            return;
        }
        check(changed.front() == prefab, "which is the one that went away");

        std::filesystem::remove_all(source.parent_path());
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

        std::vector<engine::Guid> changed;
        check(!content.reload(changed), "a manifest that will not read fails the reload");
        check(changed.empty(), "and it names nothing");
        // The point of the test. Dropping the manifest here would leave the
        // program unable to find any asset at all, over one bad write.
        check(identity_of(content, "a.scene") == scene,
              "and the content keeps the manifest it already had");

        std::filesystem::remove_all(source.parent_path());
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

        std::vector<engine::Guid> changed;
        check(!reload.poll(content, changed), "an unchanged tree reloads nothing");
        check(reload.cooks() == 0, "and it does not run the cooker at all");

        write_file(source / "a.scene", "{\"changed\":true}");
        check(!reload.poll(content, changed), "the walk that first sees the change waits");

        check(reload.poll(content, changed), "the next poll cooks and reloads");
        check(reload.cooks() == 1, "and it ran the cooker once");
        check(changed.size() == 1 && changed.front() == scene, "it names the changed asset");
        check(read_file(out / "a.scene") == "{\"changed\":true}",
              "and the new bytes reached the cooked tree");

        std::filesystem::remove_all(source.parent_path());
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

        std::vector<engine::Guid> changed;
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
        std::vector<engine::Guid> refused;
        check(content.reload(refused) && refused.size() == 1,
              "the failed cook did change the tree, so refusing it was a decision");

        std::filesystem::remove_all(source.parent_path());
    }

    void test_hot_reload_is_off_when_it_cannot_cook() {
        const std::filesystem::path source = scratch("hot_off/src");
        write_file(source / "a.scene", "{}");

        as::Content content;
        std::vector<engine::Guid> changed;

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

        std::filesystem::remove_all(source.parent_path());
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
    test_shell_metacharacters_are_refused();
    test_documentation_is_not_an_asset();
    test_bad_input();
    test::section("textures");
    test_color_space_decides_the_mip_chain();
    test_editing_the_sidecar_cooks_again();
    test_an_older_manifest_cooks_again();
    test_an_older_cooker_cooks_again();
    test_compression_and_mip_switches();
    test_awkward_sizes();
    test_a_broken_image_fails_the_cook();
    test::section("reading it back");
    test_content_reads_what_the_cooker_wrote();
    test::section("hot reload");
    test_reload_names_only_what_changed();
    test_reload_names_an_asset_that_went_away();
    test_reload_keeps_what_it_has_when_the_manifest_will_not_read();
    test_hot_reload_cooks_what_changed();
    test_hot_reload_lives_through_a_cook_that_fails();
    test_hot_reload_is_off_when_it_cannot_cook();
    return test::report();
}
