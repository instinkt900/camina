// M10.6 tests for engine::ui::ScriptSurface, the one implementation of
// script::UiSurface.
//
// tests/test_script.cpp drives the Lua binding against a fake surface, so it
// proves the `ui` table and nothing under it. This drives the real surface
// against a real moth_ui node tree, so between them the whole path from a Lua
// call to a moth_ui node is covered.
//
// It opens no device, for the reason tests/test_ui_input.cpp gives: a node
// touches the renderer when it draws and never when it takes an event. The
// image factory is the one part a device would be needed for, so this supplies
// its own. That is not a shortcut around the real one, which tests/test_ui_*.cpp
// cover elsewhere: what matters here is which identity the surface asked for.
//
// **Nothing here presses a button, and no layout below carries one.** A cooked
// .mothui cannot express a button that a press could name. moth_ui reads a
// widget class only for a group entity, the only group a file can hold as a
// child is a reference to another layout, and Layout::Deserialize never reads an
// id for the root. So the only button a layout can carry is its root, and that
// root has no name to report a press under. The surface records a press and
// wires every clickable node it finds. Nothing can build one for it yet.

#include "assets/content.h"
#include "assets/manifest.h"
#include "check.h"
#include "import/cook.h"
#include "ui/font_factory.h"
#include "ui/input_bridge.h"
#include "ui/renderer.h"
#include "ui/script_surface.h"

#include <moth_ui/context.h>
#include <moth_ui/events/event_mouse.h>
#include <moth_ui/graphics/iimage.h>
#include <moth_ui/iimage_factory.h>
#include <moth_ui/nodes/node.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

    namespace as = engine::assets;
    using engine::ui::ScriptSurface;

    std::filesystem::path scratch(std::string_view name) {
        const std::filesystem::path path =
            std::filesystem::temp_directory_path() / "camina_test_ui_surface" / name;
        test::remove_tree(path);
        std::filesystem::create_directories(path);
        return path;
    }

    void write_file(const std::filesystem::path& path, std::string_view text) {
        std::filesystem::create_directories(path.parent_path());
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file << text;
    }

    void write_bytes(const std::filesystem::path& path, std::span<const std::uint8_t> bytes) {
        std::filesystem::create_directories(path.parent_path());
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file.write(reinterpret_cast<const char*>(bytes.data()), // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
                   static_cast<std::streamsize>(bytes.size()));
    }

    /// A 4 by 4 RGBA PNG. Four wide so a block compressor gets a whole block.
    constexpr std::array<std::uint8_t, 80> kPng{
        0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D, 0x49, 0x48,
        0x44, 0x52, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x04, 0x08, 0x06, 0x00, 0x00,
        0x00, 0xA9, 0xF1, 0x9E, 0x7E, 0x00, 0x00, 0x00, 0x17, 0x49, 0x44, 0x41, 0x54, 0x78,
        0xDA, 0x63, 0x60, 0x70, 0x68, 0xF8, 0xFF, 0x1F, 0x88, 0xE1, 0x34, 0x0A, 0x07, 0x84,
        0x09, 0xAA, 0x00, 0x00, 0xB7, 0x5C, 0x23, 0xE9, 0x62, 0xD1, 0xE3, 0x9A, 0x00, 0x00,
        0x00, 0x00, 0x49, 0x45, 0x4E, 0x44, 0xAE, 0x42, 0x60, 0x82
    };

    /// The four offset tracks that give a node a rectangle at frame zero.
    std::string rect_tracks(int left, int top, int right, int bottom) {
        const auto track = [](const char* target, int value) {
            return std::string{ R"({"target":")" } + target +
                   R"(","keyframes":[{"frame":0,"interp":"Linear","value":)" +
                   std::to_string(value) + "}]}";
        };
        return R"("tracks":[)" + track("LeftOffset", left) + "," + track("TopOffset", top) +
               "," + track("RightOffset", right) + "," + track("BottomOffset", bottom) + "]";
    }

    std::string text_node(std::string_view id, std::string_view text) {
        return std::string{ R"({"type":"Text","id":")" } + std::string{ id } +
               R"(","class":"","visible":true,"fontName":"body","fontSize":16,"text":")" +
               std::string{ text } + R"(","horizontalAlignment":"Center",)" +
               R"("verticalAlignment":"Middle",)" + rect_tracks(0, 0, 200, 40) + "}";
    }

    /// An image entity with nothing assigned yet, which the cooker allows.
    std::string image_node(std::string_view id) {
        return std::string{ R"({"type":"Image","id":")" } + std::string{ id } +
               R"(","class":"","visible":true,"imagePath":"","imageScaleType":"Stretch",)" +
               R"("imageScale":1.0,)" + rect_tracks(0, 40, 64, 104) + "}";
    }

    std::string layout_of(std::string_view children) {
        return std::string{ R"({"type":"Layout","mothui_version":1,"class":"","children":[)" } +
               std::string{ children } + "]}";
    }

    /// An image the surface hands back, so set_image needs no device.
    class StubImage final : public moth_ui::IImage {
    public:
        int GetWidth() const override { return 4; }
        int GetHeight() const override { return 4; }
        moth_ui::IntVec2 GetDimensions() const override { return { 4, 4 }; }
    };

    /**
     * An image factory that records what it was asked for.
     *
     * The real one needs a device. What this test has to know is which identity
     * reached the factory, because that is the half `set_image` is responsible
     * for. Whether a texture uploads is `engine::ui::ImageFactory`'s business.
     */
    class RecordingImages final : public moth_ui::IImageFactory {
    public:
        std::unique_ptr<moth_ui::IImage> GetImage(const moth_ui::AssetId& id) override {
            asked.push_back(id.str());
            if (refuse) {
                return nullptr;
            }
            return std::make_unique<StubImage>();
        }

        std::vector<std::string> asked;
        bool refuse = false;
    };

    /// A surface over a cooked tree, with no device under any of it.
    struct Fixture {
        RecordingImages images;
        engine::ui::FontFactory fonts;
        engine::ui::Renderer renderer;
        moth_ui::Context context{ &images, &fonts, &renderer };
        const as::Content* content = nullptr;
        std::unique_ptr<ScriptSurface> surface;

        void open(const as::Content& tree) {
            content = &tree;
            surface = std::make_unique<ScriptSurface>(tree, context);
            surface->set_screen_rect(moth_ui::IntRect{ { 0, 0 }, { 800, 600 } });
        }
    };

    /**
     * Cooks a tree holding two layouts, an image and a document that is not a
     * layout.
     *
     * Two layouts on purpose. One layout can never show that a node of one is
     * not reachable through the other, and it cannot show that a reload touches
     * the layout it names and no other.
     */
    void with_a_cooked_tree(const std::function<void(const as::Content&)>& run) {
        const std::filesystem::path source = scratch("src");
        const std::filesystem::path out = scratch("out");

        write_file(source / "ui" / "menu.mothui",
                   layout_of(text_node("title", "Camina") + "," + image_node("logo")));
        write_file(source / "ui" / "hud.mothui", layout_of(text_node("score", "0")));

        // Readable JSON whose root is not a Layout. A reference left pointing at
        // the wrong asset looks exactly like this.
        write_file(source / "ui" / "wrong.mothui", R"({"type":"Group","children":[]})");

        write_bytes(source / "ui" / "panel.png", kPng);

        const engine::import::Options options{ .content = source, .out = out };
        engine::import::Result result;
        test::check(engine::import::cook_all(options, result), "the tree cooks");

        as::Content content;
        test::check(content.open(out), "and it opens");
        run(content);

        test::remove_tree(source.parent_path());
    }

    void a_script_shows_a_layout_and_it_loads_on_demand() {
        test::section("a script shows a layout and it loads on demand");

        with_a_cooked_tree([](const as::Content& content) {
            Fixture fixture;
            fixture.open(content);

            test::check(fixture.surface->loaded_count() == 0, "nothing is loaded at the start");
            test::check(!fixture.surface->visible("ui/menu.mothui"),
                        "and nothing is showing");

            test::check(fixture.surface->show("ui/menu.mothui"), "the layout shows");
            test::check(fixture.surface->loaded_count() == 1, "which loaded it");
            test::check(fixture.surface->showing_count() == 1, "and it is showing");
            test::check(fixture.surface->visible("ui/menu.mothui"), "and it says so");

            test::check(fixture.surface->show("ui/menu.mothui"), "showing it again works");
            test::check(fixture.surface->loaded_count() == 1, "and loads nothing twice");
        });
    }

    void a_layout_the_tree_does_not_hold_is_refused() {
        test::section("a layout the tree does not hold is refused");

        with_a_cooked_tree([](const as::Content& content) {
            Fixture fixture;
            fixture.open(content);

            test::check(!fixture.surface->show("ui/nothing.mothui"),
                        "a name the tree does not hold is refused");
            test::check(fixture.surface->loaded_count() == 0, "and nothing is loaded");

            // A real asset that is not a layout. The surface has to refuse it
            // the same way, because a wrong reference is likelier than a typo.
            test::check(!fixture.surface->show("ui/wrong.mothui"),
                        "and a document that is not a layout is refused too");
            test::check(fixture.surface->loaded_count() == 0, "and still nothing is loaded");
        });
    }

    void hiding_a_layout_keeps_it_loaded() {
        test::section("hiding a layout keeps it loaded");

        with_a_cooked_tree([](const as::Content& content) {
            Fixture fixture;
            fixture.open(content);
            test::check(fixture.surface->show("ui/menu.mothui"), "the layout shows");

            test::check(fixture.surface->hide("ui/menu.mothui"), "and it hides");
            test::check(!fixture.surface->visible("ui/menu.mothui"), "so it is not showing");
            test::check(fixture.surface->loaded_count() == 1, "and it is still loaded");
            test::check(fixture.surface->showing_count() == 0, "and nothing is showing");

            // The node is still reachable while the layout is hidden, so a
            // script can fill a menu in before it puts it on the screen.
            test::check(fixture.surface->has_node("ui/menu.mothui", "title"),
                        "and a script can still reach its nodes");

            test::check(!fixture.surface->hide("ui/nothing.mothui"),
                        "hiding a layout that was never shown is refused");
        });
    }

    void a_script_finds_a_node_by_name() {
        test::section("a script finds a node by name");

        with_a_cooked_tree([](const as::Content& content) {
            Fixture fixture;
            fixture.open(content);
            test::check(fixture.surface->show("ui/menu.mothui"), "the layout shows");

            test::check(fixture.surface->has_node("ui/menu.mothui", "title"),
                        "a node that is there is found");
            test::check(fixture.surface->has_node("ui/menu.mothui", "logo"),
                        "and so is the image");
            test::check(!fixture.surface->has_node("ui/menu.mothui", "nope"),
                        "and a node that is not there is not");
            test::check(!fixture.surface->has_node("ui/hud.mothui", "title"),
                        "and a node of another layout is not found through this one");
        });
    }

    void a_script_reads_and_writes_the_text_of_a_node() {
        test::section("a script reads and writes the text of a node");

        with_a_cooked_tree([](const as::Content& content) {
            Fixture fixture;
            fixture.open(content);
            test::check(fixture.surface->show("ui/menu.mothui"), "the layout shows");

            test::check(fixture.surface->text("ui/menu.mothui", "title") == "Camina",
                        "the text reads what the layout authored");
            test::check(fixture.surface->set_text("ui/menu.mothui", "title", "Paused"),
                        "and a script writes over it");
            test::check(fixture.surface->text("ui/menu.mothui", "title") == "Paused",
                        "and reads back what it wrote");

            test::check(!fixture.surface->set_text("ui/menu.mothui", "logo", "no"),
                        "a node that shows no text refuses the write");
            test::check(fixture.surface->text("ui/menu.mothui", "logo").empty(),
                        "and reads as empty rather than failing");
            test::check(!fixture.surface->set_text("ui/menu.mothui", "nope", "no"),
                        "and a node that is not there refuses it");
        });
    }

    void a_script_shows_and_hides_one_node() {
        test::section("a script shows and hides one node");

        with_a_cooked_tree([](const as::Content& content) {
            Fixture fixture;
            fixture.open(content);
            test::check(fixture.surface->show("ui/menu.mothui"), "the layout shows");

            test::check(fixture.surface->node_visible("ui/menu.mothui", "title"),
                        "the node starts visible");
            test::check(fixture.surface->set_node_visible("ui/menu.mothui", "title", false),
                        "and a script hides it");
            test::check(!fixture.surface->node_visible("ui/menu.mothui", "title"),
                        "and it says so");

            test::check(!fixture.surface->set_node_visible("ui/menu.mothui", "nope", true),
                        "a node that is not there refuses it");
            test::check(!fixture.surface->node_visible("ui/menu.mothui", "nope"),
                        "and reads as hidden rather than failing");
        });
    }

    void a_script_changes_the_image_of_a_node() {
        test::section("a script changes the image of a node");

        with_a_cooked_tree([](const as::Content& content) {
            Fixture fixture;
            fixture.open(content);
            test::check(fixture.surface->show("ui/menu.mothui"), "the layout shows");
            fixture.images.asked.clear();

            test::check(fixture.surface->set_image("ui/menu.mothui", "logo", "ui/panel.png"),
                        "a script sets the image by its source path");
            test::check(fixture.images.asked.size() == 1 &&
                            fixture.images.asked.front() == "ui/panel.png",
                        "and the factory was asked for exactly that");

            test::check(!fixture.surface->set_image("ui/menu.mothui", "title", "ui/panel.png"),
                        "a node that draws no image refuses it");
            test::check(!fixture.surface->set_image("ui/menu.mothui", "nope", "ui/panel.png"),
                        "and so does a node that is not there");
        });
    }

    void an_image_the_tree_does_not_hold_leaves_the_node_alone() {
        test::section("an image the tree does not hold leaves the node alone");

        with_a_cooked_tree([](const as::Content& content) {
            // NodeImage::Load drops the image it holds before it asks the
            // factory, so a name that will not resolve would blank the node. A
            // typo in a skin name must leave the art that is already there.
            Fixture fixture;
            fixture.open(content);
            test::check(fixture.surface->show("ui/menu.mothui"), "the layout shows");
            test::check(fixture.surface->set_image("ui/menu.mothui", "logo", "ui/panel.png"),
                        "and the node has an image");
            fixture.images.asked.clear();

            test::check(!fixture.surface->set_image("ui/menu.mothui", "logo", "ui/gone.png"),
                        "a name the tree does not hold is refused");
            test::check(fixture.images.asked.empty(),
                        "and the factory was never asked, so the node kept its image");
        });
    }

    void an_image_that_will_not_load_is_reported() {
        test::section("an image that will not load is reported");

        with_a_cooked_tree([](const as::Content& content) {
            // The guard above is conservative rather than authoritative. An
            // identity that is in the manifest and will not read gets past it,
            // and the factory is what decides.
            Fixture fixture;
            fixture.open(content);
            test::check(fixture.surface->show("ui/menu.mothui"), "the layout shows");

            fixture.images.refuse = true;
            test::check(!fixture.surface->set_image("ui/menu.mothui", "logo", "ui/panel.png"),
                        "an image the factory refuses is reported as a failure");
        });
    }

    void a_reload_keeps_the_layout_showing() {
        test::section("a reload keeps the layout showing");

        with_a_cooked_tree([](const as::Content& content) {
            Fixture fixture;
            fixture.open(content);
            test::check(fixture.surface->show("ui/hud.mothui"), "the HUD shows");
            test::check(fixture.surface->show("ui/menu.mothui"), "and the menu shows over it");
            test::check(fixture.surface->set_text("ui/menu.mothui", "title", "Paused"),
                        "and a script wrote into it");

            const as::ManifestEntry* entry = content.find("ui/menu.mothui");
            test::check(entry != nullptr, "the layout is in the manifest");
            const std::array<engine::Guid, 1> changed{ entry->guid };

            test::check(fixture.surface->reload_layouts(changed), "the layout reloads");
            test::check(fixture.surface->loaded_count() == 2, "both layouts are still loaded");
            test::check(fixture.surface->visible("ui/menu.mothui"), "and it is still showing");
            test::check(fixture.surface->text("ui/menu.mothui", "title") == "Camina",
                        "and its text is the authored one again");
        });
    }

    void a_reload_of_another_asset_changes_nothing() {
        test::section("a reload of another asset changes nothing");

        with_a_cooked_tree([](const as::Content& content) {
            Fixture fixture;
            fixture.open(content);
            test::check(fixture.surface->show("ui/menu.mothui"), "the layout shows");
            test::check(fixture.surface->set_text("ui/menu.mothui", "title", "Paused"),
                        "and a script wrote into it");

            const as::ManifestEntry* other = content.find("ui/hud.mothui");
            test::check(other != nullptr, "the other layout is in the manifest");
            const std::array<engine::Guid, 1> changed{ other->guid };

            test::check(!fixture.surface->reload_layouts(changed),
                        "a change to a layout nobody showed reloads nothing");
            test::check(fixture.surface->text("ui/menu.mothui", "title") == "Paused",
                        "so what the script wrote is still there");
        });
    }

} // namespace

int main() {
    a_script_shows_a_layout_and_it_loads_on_demand();
    a_layout_the_tree_does_not_hold_is_refused();
    hiding_a_layout_keeps_it_loaded();
    a_script_finds_a_node_by_name();
    a_script_reads_and_writes_the_text_of_a_node();
    a_script_shows_and_hides_one_node();
    a_script_changes_the_image_of_a_node();
    an_image_the_tree_does_not_hold_leaves_the_node_alone();
    an_image_that_will_not_load_is_reported();
    a_reload_keeps_the_layout_showing();
    a_reload_of_another_asset_changes_nothing();
    return test::report();
}
