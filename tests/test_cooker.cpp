// M4.2 tests for the cooker and the manifest.
//
// The property that carries the milestone is the incremental check. A cook
// that redoes everything wastes a minute today and an hour at M4.4, and a cook
// that skips too much ships a stale asset. Both failures are quiet, so the
// tests here drive the second run and check the counts rather than the output.
//
// These tests cook no shader. glslc is a separate program, and a test that
// needs it would not run on a machine without the Conan environment. The copy
// rule exercises the same manifest path, and the build itself cooks the real
// shaders.

#include "assets/content.h"
#include "assets/manifest.h"
#include "assets/meta.h"
#include "check.h"
#include "cook.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

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
        check(fresh != nullptr && fresh->guid != entry->guid, "and the identity did change");

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

        std::vector<std::byte> bytes;
        check(entry != nullptr && content.read_bytes(*entry, bytes), "its bytes read");
        check(bytes.size() == 2, "and they are the bytes the source held");

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

} // namespace

int main() {
    std::printf("hashing\n");
    test_hash_is_content_not_time();
    test_input_order_matters();
    std::printf("cooking\n");
    test_cook_and_skip();
    test_missing_output_recooks();
    test_new_identity_recooks();
    test_bad_input();
    std::printf("reading it back\n");
    test_content_reads_what_the_cooker_wrote();
    return test::report();
}
