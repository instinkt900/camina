// M4.1 tests for the asset database and the sidecar.
//
// Two properties carry the milestone. A handle that a caller resolved before
// the asset loaded still points at the asset afterwards, because M4.5 replaces
// an asset while the program runs and must not fix up every handle. And a
// missing asset gives the placeholder rather than ending the process, because
// an artist with a half-cooked directory has to be able to keep working.

#include "assets/database.h"
#include "assets/meta.h"
#include "check.h"

#include <filesystem>
#include <fstream>
#include <string_view>

namespace {

    using test::check;
    namespace as = engine::assets;
    using engine::Guid;

    /// Stands in for a texture. M4.3 brings the real one.
    struct Image {
        int width = 0;
        int height = 0;
    };

    /// A second type, so the database has to keep two pools apart.
    struct Sound {
        float seconds = 0.0F;
    };

    /// A directory of its own, so two runs of the test cannot collide.
    std::filesystem::path scratch_directory() {
        const std::filesystem::path path =
            std::filesystem::temp_directory_path() / "camina_test_assets";
        std::filesystem::remove_all(path);
        std::filesystem::create_directories(path);
        return path;
    }

    void write_file(const std::filesystem::path& path, std::string_view text) {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file << text;
    }

    void test_resolve_is_stable() {
        as::AssetDatabase database;
        const Guid guid = Guid::generate();

        const as::AssetHandle<Image> early = database.resolve<Image>(guid);
        check(early.valid(), "a GUID gets a handle before the asset loads");
        check(database.state(early) == as::AssetState::Unloaded, "and the slot starts empty");

        check(database.resolve<Image>(guid) == early, "asking twice gives the same handle");

        // The point of the whole design. The handle a component stored before
        // the load still names the asset after it.
        const as::AssetHandle<Image> filled =
            database.store<Image>(guid, Image{ .width = 4, .height = 2 });
        check(filled == early, "loading keeps the handle the caller already held");
        check(database.state(early) == as::AssetState::Loaded, "and the slot now holds it");
        check(database.get(early).width == 4, "reading the old handle gives the new asset");

        // Hot reload, in the small. M4.5 does this from a file watcher.
        const as::AssetHandle<Image> again =
            database.store<Image>(guid, Image{ .width = 8, .height = 16 });
        check(again == early, "replacing an asset keeps the handle");
        check(database.get(early).width == 8, "and the old handle sees the new asset");
    }

    void test_reference_survives_later_loads() {
        // get() hands out a reference into a slot, and resolve() adds slots. A
        // renderer reads a mesh at the top of a frame and a load lands part way
        // through, so the two happen together in the normal case. The pool
        // therefore has to hold its slots in a container that does not move
        // them when it grows.
        as::AssetDatabase database;
        const as::AssetHandle<Image> first =
            database.store<Image>(Guid::generate(), Image{ .width = 7 });
        const Image& held = database.get(first);

        constexpr int kEnoughToRegrow = 1000;
        for (int i = 0; i < kEnoughToRegrow; ++i) {
            (void)database.resolve<Image>(Guid::generate());
        }

        check(held.width == 7, "a reference survives a thousand later slots");
        check(database.pool<Image>().size() == kEnoughToRegrow + 1, "and every slot is there");
    }

    void test_null_guid_gets_no_slot() {
        as::AssetDatabase database;
        const as::AssetHandle<Image> handle = database.resolve<Image>(Guid{});
        check(!handle.valid(), "the null GUID resolves to no handle");
        check(database.pool<Image>().size() == 0, "and it takes up no slot");
    }

    void test_placeholder_stands_in() {
        as::AssetDatabase database;
        database.pool<Image>().set_placeholder(Image{ .width = -1, .height = -1 });

        // Nothing loaded yet.
        const as::AssetHandle<Image> pending = database.resolve<Image>(Guid::generate());
        check(database.get(pending).width == -1, "an asset that is not loaded gives the placeholder");

        // A handle from nowhere. This is what a stale or hand-made handle looks
        // like, and it must not read another asset's memory.
        const as::AssetHandle<Image> nonsense = as::AssetHandle<Image>::make(9999, 1);
        check(database.get(nonsense).width == -1, "a handle outside the pool gives the placeholder");
        check(database.state(nonsense) == as::AssetState::Unloaded, "and it reads as not loaded");

        const as::AssetHandle<Image> nothing;
        check(database.get(nothing).width == -1, "the invalid handle gives the placeholder");

        // A broken asset. The database remembers, so a caller that asks every
        // frame gets the placeholder and not a retry.
        const Guid broken = Guid::generate();
        const as::AssetHandle<Image> failed =
            database.pool<Image>().fail(broken, "the file is not a PNG");
        check(database.state(failed) == as::AssetState::Failed, "a failure is remembered");
        check(database.get(failed).width == -1, "and a broken asset gives the placeholder");
    }

    void test_stale_handle_after_clear() {
        as::AssetDatabase database;
        database.pool<Image>().set_placeholder(Image{ .width = -1 });

        const Guid guid = Guid::generate();
        const as::AssetHandle<Image> old = database.store<Image>(guid, Image{ .width = 4 });
        database.pool<Image>().clear();

        // The slot index comes back the moment anything else loads. Without the
        // generation counter the old handle would read that other asset.
        const as::AssetHandle<Image> fresh =
            database.store<Image>(Guid::generate(), Image{ .width = 100 });
        check(fresh.index() == old.index(), "the new asset took the same slot");
        check(fresh != old, "but it is not the same handle");
        check(database.get(old).width == -1, "the stale handle gives the placeholder");
        check(database.get(fresh).width == 100, "and the live handle gives the asset");
    }

    void test_types_stay_apart() {
        as::AssetDatabase database;
        const Guid shared = Guid::generate();

        const as::AssetHandle<Image> image =
            database.store<Image>(shared, Image{ .width = 32, .height = 32 });
        const as::AssetHandle<Sound> sound =
            database.store<Sound>(shared, Sound{ .seconds = 1.5F });

        check(database.pool_count() == 2, "two asset types give two pools");
        check(database.get(image).width == 32, "the image is the image");
        check(database.get(sound).seconds == 1.5F, "the sound is the sound");
        check(database.pool<Image>().guid_of(image) == shared, "the handle remembers its GUID");
    }

    void test_meta_path() {
        // The full source name stays in the sidecar name. Without that,
        // crate.png and crate.gltf would fight over crate.meta.
        check(as::meta_path("content/crate.png").filename() == "crate.png.meta",
              "the sidecar keeps the whole source name");
        check(as::meta_path("content/crate.gltf").filename() == "crate.gltf.meta",
              "so two source types get two sidecars");
    }

    void test_meta_is_written_once() {
        const std::filesystem::path directory = scratch_directory();
        const std::filesystem::path source = directory / "crate.png";
        write_file(source, "not really a png");

        as::AssetMeta first;
        check(as::meta_for(source, first), "a source file with no sidecar gets one");
        check(first.guid.valid(), "and the sidecar holds a real GUID");
        check(std::filesystem::exists(as::meta_path(source)), "the sidecar is on disk");

        as::AssetMeta second;
        check(as::meta_for(source, second), "the second call reads the sidecar");
        check(second.guid == first.guid, "and the asset keeps the identity it had");

        // The whole reason the identity does not live in the path.
        const std::filesystem::path renamed = directory / "box.png";
        std::filesystem::rename(source, renamed);
        std::filesystem::rename(as::meta_path(source), as::meta_path(renamed));

        as::AssetMeta after_rename;
        check(as::meta_for(renamed, after_rename), "the renamed pair still reads");
        check(after_rename.guid == first.guid, "a rename does not change the identity");

        std::filesystem::remove_all(directory);
    }

    void test_meta_refuses_and_repairs() {
        const std::filesystem::path directory = scratch_directory();

        as::AssetMeta meta;
        check(!as::meta_for(directory / "not_there.png", meta),
              "a source file that is not there gets no identity");
        check(!as::meta_for(directory, meta), "a directory is not a source asset");

        // A sidecar somebody truncated, or a bad merge. Writing a new one beats
        // refusing to cook, because the alternative is a cook that stops on a
        // file the user can neither see nor fix.
        const std::filesystem::path source = directory / "crate.png";
        write_file(source, "not really a png");
        write_file(as::meta_path(source), "{}");

        as::AssetMeta repaired;
        check(as::meta_for(source, repaired), "a sidecar with no GUID is replaced");
        check(repaired.guid.valid(), "and the asset gets a real identity");

        as::AssetMeta reread;
        check(as::meta_for(source, reread) && reread.guid == repaired.guid,
              "the replacement sticks");

        std::filesystem::remove_all(directory);
    }

} // namespace

int main() {
    std::printf("handles\n");
    test_resolve_is_stable();
    test_reference_survives_later_loads();
    test_null_guid_gets_no_slot();
    test_stale_handle_after_clear();
    test_types_stay_apart();
    std::printf("placeholders\n");
    test_placeholder_stands_in();
    std::printf("sidecars\n");
    test_meta_path();
    test_meta_is_written_once();
    test_meta_refuses_and_repairs();
    return test::report();
}
