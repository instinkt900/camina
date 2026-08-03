// M4.1 tests for the asset database and the sidecar.
//
// Two properties carry the milestone.
//
// A handle that a caller resolved before the asset loaded still points at the
// asset afterwards. M4.5 replaces an asset while the program runs, and it must
// not fix up every handle to do so.
//
// A missing asset gives the placeholder rather than ending the process. An
// artist with a half-cooked directory has to be able to keep working.

#include "assets/database.h"
#include "assets/material.h"
#include "assets/meta.h"
#include "assets/texture.h"
#include "check.h"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <vector>

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
        // get() hands out a reference into a slot, and resolve() adds slots.
        // A renderer reads a mesh at the top of a frame, and a load lands part
        // way through. The two therefore happen together in the normal case.
        // So the pool has to hold its slots in a container that does not move
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
        // refusing to cook. The alternative is a cook that stops on a file the
        // user can neither see nor fix.
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

    void test_meta_reports_who_wrote_it() {
        const std::filesystem::path directory = scratch_directory();
        const std::filesystem::path source = directory / "crate.png";
        write_file(source, "not really a png");

        // A cooker rule fills in a guessed import setting only on the call that
        // wrote the sidecar. Getting this backwards would overwrite what a
        // person typed, every cook, and the edit would look like it did nothing.
        as::AssetMeta meta;
        bool created = false;
        check(as::meta_for(source, meta, &created) && created, "the first call writes it");

        created = true;
        check(as::meta_for(source, meta, &created) && !created, "the second call reads it");

        std::filesystem::remove_all(directory);
    }

    void test_meta_carries_import_settings() {
        const std::filesystem::path directory = scratch_directory();
        const std::filesystem::path source = directory / "crate.png";
        write_file(source, "not really a png");

        as::AssetMeta written;
        written.guid = Guid::generate();
        written.texture.color_space = as::ColorSpace::Linear;
        written.texture.compress = false;
        written.texture.mips = false;
        check(as::save_meta(source, written), "a sidecar with import settings writes");

        as::AssetMeta read;
        check(as::load_meta(source, read), "and it reads back");
        check(read.texture.color_space == as::ColorSpace::Linear, "the color space survived");
        check(!read.texture.compress && !read.texture.mips, "and so did the two switches");

        // The color space reaches the file as a word, not as a number. A person
        // opens this file to fix a texture that came out wrong, and "1" says
        // nothing about which one it is.
        //
        // The stream lives in a scope of its own, so it closes before the
        // remove_all below. Windows refuses to delete a file that is open, and
        // the throwing remove_all then ends the process with no message at all.
        // Linux unlinks an open file without complaint, so this kind of mistake
        // only ever shows up in CI.
        std::string text;
        {
            std::ifstream file(as::meta_path(source));
            text.assign(std::istreambuf_iterator<char>{ file },
                        std::istreambuf_iterator<char>{});
        }
        check(text.find("\"Linear\"") != std::string::npos,
              "and the file names the color space in words");

        std::filesystem::remove_all(directory);
    }

    void test_mip_arithmetic() {
        check(as::mip_count_for(256, 256) == 9, "256 by 256 holds 9 levels");
        check(as::mip_count_for(256, 1) == 9, "and so does 256 by 1, down to 1 by 1");
        check(as::mip_count_for(1, 1) == 1, "1 by 1 holds one level");
        check(as::mip_count_for(0, 4) == 0, "an empty texture holds none");

        check(as::mip_extent(256, 3) == 32, "level 3 of 256 is 32");
        check(as::mip_extent(1, 4) == 1, "a level never falls below one texel");
        check(as::mip_extent(256, 40) == 1, "and a level past the end does not shift off");

        check(as::level_bytes(as::TextureFormat::RGBA8, 4, 4) == 64, "16 texels cost 64 bytes");

        // The rounding that a caller must not do itself. A 2 by 2 level does not
        // fill a block, and it still costs a whole one.
        check(as::level_bytes(as::TextureFormat::BC7, 4, 4) == 16, "one block is 16 bytes");
        check(as::level_bytes(as::TextureFormat::BC7, 2, 2) == 16,
              "a level below one block still costs one");
        check(as::level_bytes(as::TextureFormat::BC7, 5, 5) == 64,
              "5 by 5 rounds up to 2 blocks each way");

        // 65536 + 16384 + 4096 + 1024 + 256 + 64 + 16 + 16 + 16.
        check(as::chain_bytes(as::TextureFormat::BC7, 256, 256, 9) == 87408,
              "a whole BC7 chain adds up, with the small levels rounded up");
    }

    /// Builds a cooked texture file in memory, so a test can then break it.
    std::vector<std::byte> make_texture_file(const as::TextureHeader& header,
                                             std::size_t payload_size) {
        std::vector<std::byte> bytes(sizeof(as::TextureHeader) + payload_size);
        std::memcpy(bytes.data(), &header, sizeof(header));
        return bytes;
    }

    as::TextureHeader good_header() {
        as::TextureHeader header;
        header.format = static_cast<std::uint32_t>(as::TextureFormat::RGBA8);
        header.color_space = static_cast<std::uint32_t>(as::ColorSpace::Linear);
        header.width = 4;
        header.height = 4;
        header.mip_count = 3;
        header.payload_size =
            static_cast<std::uint32_t>(as::chain_bytes(as::TextureFormat::RGBA8, 4, 4, 3));
        return header;
    }

    void test_read_texture_refuses_a_bad_file() {
        // Every branch here ends in a device upload that reads past the end of
        // the buffer, or in a driver rejecting a texture with a message that
        // names nothing. Catching it at the file is the whole point.
        as::TextureView view;

        const as::TextureHeader header = good_header();
        const std::vector<std::byte> good = make_texture_file(header, header.payload_size);
        check(as::read_texture(good, view, "good"), "a whole file reads");
        check(view.width == 4 && view.mip_count == 3, "and the view carries the header");
        check(view.color_space == as::ColorSpace::Linear, "and the color space");
        check(view.payload.size() == header.payload_size, "and it points at every level");

        check(!as::read_texture({}, view, "empty"), "an empty file is refused");
        check(!as::read_texture(std::span{ good }.first(8), view, "short"),
              "a file too short for a header is refused");

        as::TextureHeader wrong = header;
        wrong.magic = 0;
        check(!as::read_texture(make_texture_file(wrong, wrong.payload_size), view, "magic"),
              "a file that is not a cooked texture is refused");

        wrong = header;
        wrong.version = as::kTextureVersion + 1;
        check(!as::read_texture(make_texture_file(wrong, wrong.payload_size), view, "version"),
              "a file from a later format version is refused");

        wrong = header;
        wrong.format = 99;
        check(!as::read_texture(make_texture_file(wrong, wrong.payload_size), view, "format"),
              "a format this build does not have is refused");

        wrong = header;
        wrong.color_space = 99;
        check(!as::read_texture(make_texture_file(wrong, wrong.payload_size), view, "space"),
              "a color space this build does not have is refused");

        wrong = header;
        wrong.mip_count = 0;
        check(!as::read_texture(make_texture_file(wrong, 0), view, "no levels"),
              "a file with no levels is refused");

        wrong = header;
        wrong.mip_count = 9;
        check(!as::read_texture(make_texture_file(wrong, wrong.payload_size), view, "too many"),
              "more levels than the size can hold is refused");

        // The two that matter most. A short file makes the upload read past the
        // end, and a long one means the header describes something else.
        check(!as::read_texture(std::span{ good }.first(good.size() - 4), view, "truncated"),
              "a file shorter than its header claims is refused");
        check(!as::read_texture(make_texture_file(header, header.payload_size + 4), view, "long"),
              "a file longer than its header claims is refused");
    }

    void test_color_space_text() {
        as::ColorSpace space = as::ColorSpace::Srgb;
        check(as::from_text("Linear", space) && space == as::ColorSpace::Linear, "Linear reads");
        check(as::from_text("srgb", space) && space == as::ColorSpace::Srgb,
              "and letter case does not matter");
        check(as::to_text(as::ColorSpace::Linear) == "Linear", "Linear writes");
        check(as::to_text(as::ColorSpace::Srgb) == "sRGB", "and sRGB writes");

        // A word nobody recognizes leaves the value alone, so a typo in a
        // sidecar keeps the default rather than silently picking one.
        space = as::ColorSpace::Linear;
        check(!as::from_text("gamma", space), "an unknown word is refused");
        check(space == as::ColorSpace::Linear, "and it changes nothing");
    }

    /// A material with every field set to something a default would not give.
    as::Material distinctive_material() {
        as::Material material;
        material.base_color = Guid{ .high = 1, .low = 2 };
        material.metallic_roughness = Guid{ .high = 3, .low = 4 };
        material.normal = Guid{ .high = 5, .low = 6 };
        material.occlusion = Guid{ .high = 7, .low = 8 };
        material.emissive = Guid{ .high = 9, .low = 10 };
        material.base_color_factor = engine::Vec4{ 0.1F, 0.2F, 0.3F, 0.4F };
        material.emissive_factor = engine::Vec3{ 0.5F, 0.6F, 0.7F };
        material.metallic_factor = 0.25F;
        material.roughness_factor = 0.75F;
        material.normal_scale = 2.0F;
        material.occlusion_strength = 0.125F;
        material.alpha_cutoff = 0.375F;
        material.alpha_mode = as::AlphaMode::Mask;
        material.double_sided = true;
        return material;
    }

    std::vector<std::byte> make_material_file(const as::MaterialHeader& header) {
        std::vector<std::byte> bytes(sizeof(header));
        std::memcpy(bytes.data(), &header, sizeof(header));
        return bytes;
    }

    void test_material_round_trips() {
        // Every field, not a sample of them. A writer that skipped one and a
        // reader that skipped the same one would agree with each other and
        // lose the value, and no other test would notice.
        const as::Material wrote = distinctive_material();
        const std::vector<std::byte> bytes = make_material_file(as::pack_material(wrote));

        as::Material read;
        check(as::read_material(bytes, read, "round"), "a packed material reads back");
        check(read.base_color == wrote.base_color && read.metallic_roughness ==
                                                         wrote.metallic_roughness,
              "with the base color and the metallic-roughness map");
        check(read.normal == wrote.normal && read.occlusion == wrote.occlusion &&
                  read.emissive == wrote.emissive,
              "and the normal, occlusion, and emissive maps");
        check(read.base_color_factor == wrote.base_color_factor &&
                  read.emissive_factor == wrote.emissive_factor,
              "and both color factors");
        check(read.metallic_factor == wrote.metallic_factor &&
                  read.roughness_factor == wrote.roughness_factor &&
                  read.normal_scale == wrote.normal_scale &&
                  read.occlusion_strength == wrote.occlusion_strength &&
                  read.alpha_cutoff == wrote.alpha_cutoff,
              "and every scalar");
        check(read.alpha_mode == as::AlphaMode::Mask && read.double_sided,
              "and the alpha mode and the double sided flag");
    }

    void test_read_material_refuses_a_bad_file() {
        as::Material material;
        const std::vector<std::byte> good =
            make_material_file(as::pack_material(distinctive_material()));

        check(!as::read_material({}, material, "empty"), "an empty file is refused");
        check(!as::read_material(std::span{ good }.first(good.size() - 1), material, "short"),
              "a file shorter than the format is refused");
        std::vector<std::byte> longer = good;
        longer.push_back(std::byte{ 0 });
        check(!as::read_material(longer, material, "long"),
              "a file longer than the format is refused");

        as::MaterialHeader wrong = as::pack_material(distinctive_material());
        wrong.magic = 0;
        check(!as::read_material(make_material_file(wrong), material, "magic"),
              "a file that is not a cooked material is refused");

        wrong = as::pack_material(distinctive_material());
        wrong.version = as::kMaterialVersion + 1;
        check(!as::read_material(make_material_file(wrong), material, "version"),
              "a file from a later format version is refused");

        wrong = as::pack_material(distinctive_material());
        wrong.alpha_mode = as::kAlphaModeMax + 1;
        check(!as::read_material(make_material_file(wrong), material, "mode"),
              "an alpha mode this build does not have is refused");

        // A factor that is not a number multiplies a surface into nothing, and
        // it survives every size and range check above. The failure it causes
        // is a mesh that draws in the wrong color, or not at all, with no
        // message that names the material.
        wrong = as::pack_material(distinctive_material());
        wrong.base_color_factor[2] = std::numeric_limits<float>::quiet_NaN();
        check(!as::read_material(make_material_file(wrong), material, "nan"),
              "a base color factor that is not a number is refused");

        wrong = as::pack_material(distinctive_material());
        wrong.roughness_factor = std::numeric_limits<float>::infinity();
        check(!as::read_material(make_material_file(wrong), material, "inf"),
              "and so is a roughness that runs off to infinity");
    }

} // namespace

int main() {
    test::section("handles");
    test_resolve_is_stable();
    test_reference_survives_later_loads();
    test_null_guid_gets_no_slot();
    test_stale_handle_after_clear();
    test_types_stay_apart();
    test::section("placeholders");
    test_placeholder_stands_in();
    test::section("sidecars");
    test_meta_path();
    test_meta_is_written_once();
    test_meta_refuses_and_repairs();
    test_meta_reports_who_wrote_it();
    test_meta_carries_import_settings();
    test::section("the cooked texture format");
    test_mip_arithmetic();
    test_read_texture_refuses_a_bad_file();
    test_color_space_text();
    test::section("the cooked material format");
    test_material_round_trips();
    test_read_material_refuses_a_bad_file();
    return test::report();
}
