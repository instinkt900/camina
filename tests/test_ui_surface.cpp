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
// **A button here is a reference to another layout**, because that is the only
// group a .mothui can hold as a child and moth_ui reads a widget class only for
// a group. The id belongs to the reference, so two menus can stand up the same
// button file under different names. Reading a reference out of the content tree
// rather than off disk is what moth_ui 1.9.0 added and what read_layout uses.

#include "assets/content.h"
#include "assets/manifest.h"
#include "check.h"
#include "import/cook.h"
#include "ui/font_factory.h"
#include "ui/input_bridge.h"
#include "ui/renderer.h"
#include "ui/script_surface.h"

#if defined(ENGINE_WITH_LUA)
#include "scene/components.h"
#include "scene/world.h"
#include "script/components.h"
#include "script/host.h"
#endif

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

    /// Names this binary's scratch tree. See test::scratch.
    constexpr std::string_view kSuite = "ui_surface";

    std::filesystem::path scratch(std::string_view name) {
        return test::scratch(kSuite, name);
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

    /**
     * The tracks that give a node a rectangle in pixels at frame zero.
     *
     * The four anchors are written as well as the four offsets, and they have to
     * be. A moth_ui node anchors to its whole parent by default, so offsets alone
     * would be added to the parent rectangle rather than measured from its corner,
     * and a 100 by 50 button would come out the size of the screen and then some.
     * The sandbox layouts anchor at zero for the same reason.
     */
    std::string rect_tracks(int left, int top, int right, int bottom) {
        const auto track = [](const char* target, int value) {
            return std::string{ R"({"target":")" } + target +
                   R"(","keyframes":[{"frame":0,"interp":"Linear","value":)" +
                   std::to_string(value) + "}]}";
        };
        return R"("tracks":[)" + track("LeftOffset", left) + "," + track("TopOffset", top) +
               "," + track("RightOffset", right) + "," + track("BottomOffset", bottom) + "," +
               track("LeftAnchor", 0) + "," + track("TopAnchor", 0) + "," +
               track("RightAnchor", 0) + "," + track("BottomAnchor", 0) + "]";
    }

    /**
     * A reference to another layout, which is how a layout carries a widget.
     *
     * moth_ui reads a widget class only for a group entity, and the only group a
     * file can name as a child is a reference. So a button in a menu is a
     * reference to a layout whose root carries the class, and the id belongs to
     * the reference rather than to the file it names.
     */
    std::string ref_node(std::string_view id, std::string_view layout, int left, int top,
                         int right, int bottom) {
        return std::string{ R"({"type":"Ref","id":")" } + std::string{ id } +
               R"(","class":"","visible":true,"layoutPath":")" + std::string{ layout } +
               R"(","propertyOverrides":[],)" + rect_tracks(left, top, right, bottom) + "}";
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

    std::string layout_of(std::string_view children, std::string_view widget_class = "") {
        return std::string{ R"({"type":"Layout","mothui_version":1,"class":")" } +
               std::string{ widget_class } + R"(","children":[)" + std::string{ children } + "]}";
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
     * Cooks a tree holding three layouts, an image and a document that is not a
     * layout.
     *
     * Two menus on purpose. One layout can never show that a node of one is not
     * reachable through the other, that the topmost answers a click first, or
     * that a reload touches the layout it names and no other. The third layout
     * is the button both menus refer to.
     */
    void with_a_cooked_tree(const std::function<void(const as::Content&)>& run) {
        const std::filesystem::path source = scratch("src");
        const std::filesystem::path out = scratch("out");

        // The layout every button in this test refers to. Its root carries the
        // class, and it holds a label, because a node inside a referenced layout
        // is what a bare name cannot tell apart.
        write_file(source / "ui" / "button.mothui",
                   layout_of(text_node("label", "Button"), "button"));

        // Both menus put a button over the same rectangle, which is what the
        // topmost-answers-first case needs. The menu stands the same button file
        // up twice, so both copies carry a child called `label`.
        write_file(source / "ui" / "menu.mothui",
                   layout_of(text_node("title", "Camina") + "," + image_node("logo") + "," +
                             ref_node("play", "button.mothui", 0, 0, 100, 50) + "," +
                             ref_node("options", "button.mothui", 200, 0, 300, 50)));
        // A layout that holds nothing but a button, so the hud below stands a
        // button up two references deep. That is the shape where a press has to
        // carry the outer reference in front of it: the inner id is declared
        // once and both copies of it read the same.
        write_file(source / "ui" / "row.mothui",
                   layout_of(ref_node("press", "button.mothui", 0, 0, 100, 50)));

        write_file(source / "ui" / "hud.mothui",
                   layout_of(text_node("score", "0") + "," +
                             ref_node("quit", "button.mothui", 0, 0, 100, 50) + "," +
                             ref_node("row one", "row.mothui", 200, 0, 300, 50) + "," +
                             ref_node("row two", "row.mothui", 200, 60, 300, 110)));

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

    /// The pointer position both mouse events carry. moth_ui::IntVec2 is not a
    /// literal type, so these cannot be constexpr. The button is 0,0 to 100,50.
    const moth_ui::IntVec2 kOnTheButton{ 20, 20 };
    const moth_ui::IntVec2 kOffTheButton{ 400, 400 };

    /// Sends a press and a release at one point, which is what activates a button.
    void click(ScriptSurface& surface, moth_ui::IntVec2 at) {
        (void)surface.OnEvent(moth_ui::EventMouseDown{ moth_ui::MouseButton::Left, at });
        (void)surface.OnEvent(moth_ui::EventMouseUp{ moth_ui::MouseButton::Left, at });
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

    void a_reload_reports_the_layout_it_rebuilt() {
        test::section("a reload reports the layout it rebuilt");

        with_a_cooked_tree([](const as::Content& content) {
            Fixture fixture;
            fixture.open(content);
            test::check(fixture.surface->show("ui/hud.mothui"), "the HUD shows");
            test::check(fixture.surface->show("ui/menu.mothui"), "and the menu shows over it");
            test::check(fixture.surface->reloads().empty(), "nothing has been rebuilt");

            const as::ManifestEntry* entry = content.find("ui/menu.mothui");
            test::check(entry != nullptr, "the layout is in the manifest");
            const std::array<engine::Guid, 1> changed{ entry->guid };
            test::check(fixture.surface->reload_layouts(changed), "the layout reloads");

            // The report is what lets a script write its values back. Without
            // it the reload above is silent, and every text the script wrote is
            // gone with nothing to say so.
            test::check(fixture.surface->reloads().size() == 1, "one layout is reported");
            if (fixture.surface->reloads().size() == 1) {
                test::check(fixture.surface->reloads().front() == "ui/menu.mothui",
                            "and it is the one that was rebuilt");
            }

            fixture.surface->clear_reloads();
            test::check(fixture.surface->reloads().empty(), "and a drain empties them");

            // A layout nobody rebuilt is never reported, so a script is not
            // asked to write back a layout that still holds what it wrote.
            const as::ManifestEntry* other = content.find("ui/panel.png");
            test::check(other != nullptr, "the image is in the manifest");
            const std::array<engine::Guid, 1> unrelated{ other->guid };
            test::check(!fixture.surface->reload_layouts(unrelated),
                        "an asset that is not a layout rebuilds nothing");
            test::check(fixture.surface->reloads().empty(), "and reports nothing");
        });
    }

    void an_image_reload_reports_every_layout() {
        test::section("an image reload reports every layout");

        with_a_cooked_tree([](const as::Content& content) {
            Fixture fixture;
            fixture.open(content);
            test::check(fixture.surface->show("ui/hud.mothui"), "the HUD shows");
            test::check(fixture.surface->show("ui/menu.mothui"), "and the menu shows over it");
            fixture.surface->clear_reloads();

            // `Node::ReloadEntity` builds every child again, so an image reload
            // throws away what a script wrote exactly as a layout reload does.
            // It was silent before M10.7c and the loss looked like a bug in the
            // game.
            test::check(fixture.surface->set_text("ui/menu.mothui", "title", "Paused"),
                        "a script wrote into a layout");
            fixture.surface->reload_images();
            test::check(fixture.surface->text("ui/menu.mothui", "title") == "Camina",
                        "an image reload put the authored text back");
            test::check(fixture.surface->reloads().size() == 2,
                        "and both loaded layouts are reported");
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

    void a_reference_resolves_through_the_content_tree() {
        test::section("a reference resolves through the content tree");

        with_a_cooked_tree([](const as::Content& content) {
            // The cooker rewrote the stored path into an identity, and nothing
            // here has a directory to resolve a path against. So this failing
            // means the provider never ran.
            Fixture fixture;
            fixture.open(content);
            test::check(fixture.surface->show("ui/menu.mothui"), "the layout shows");

            test::check(fixture.surface->has_node("ui/menu.mothui", "play"),
                        "the referenced button is in the tree, under the id the ref gave it");
            test::check(!fixture.surface->has_node("ui/menu.mothui", "quit"),
                        "and the other menu's button is not");
        });
    }

    void two_references_to_one_layout_answer_separately() {
        test::section("two references to one layout answer separately");

        with_a_cooked_tree([](const as::Content& content) {
            Fixture fixture;
            fixture.open(content);
            test::check(fixture.surface->show("ui/menu.mothui"), "the layout shows");

            // The menu stands one button file up twice, so the tree holds two
            // nodes called `label`. A name with a separator in it says which.
            test::check(fixture.surface->has_node("ui/menu.mothui", "play/label"),
                        "the label of one reference is found");
            test::check(fixture.surface->has_node("ui/menu.mothui", "options/label"),
                        "and so is the label of the other");

            test::check(fixture.surface->set_text("ui/menu.mothui", "play/label", "Play"),
                        "one of them takes a text");
            test::check(
                fixture.surface->set_text("ui/menu.mothui", "options/label", "Options"),
                "and so does the other");

            // The whole point. Writing one label used to write whichever
            // reference came first, and the second was unreachable.
            test::check(fixture.surface->text("ui/menu.mothui", "play/label") == "Play",
                        "and each keeps what it was given");
            test::check(
                fixture.surface->text("ui/menu.mothui", "options/label") == "Options",
                "rather than the two of them being one node");
        });
    }

    void a_path_searches_inside_what_the_segment_before_it_found() {
        test::section("a path searches inside what the segment before it found");

        with_a_cooked_tree([](const as::Content& content) {
            Fixture fixture;
            fixture.open(content);
            test::check(fixture.surface->show("ui/menu.mothui"), "the layout shows");

            // `title` is a sibling of the reference and not a child of it. A
            // path that found it would be searching the whole layout again for
            // every segment, which is the scoping this case exists to hold.
            test::check(!fixture.surface->has_node("ui/menu.mothui", "play/title"),
                        "a node beside the reference is not inside it");
            test::check(!fixture.surface->has_node("ui/menu.mothui", "play/nope"),
                        "and a segment that names nothing answers nothing");
            test::check(!fixture.surface->has_node("ui/menu.mothui", "nope/label"),
                        "and so does a first segment that names nothing");

            // A name with no separator reaches a node at any depth, which is
            // what almost every call passes and what M10.6 shipped.
            test::check(fixture.surface->has_node("ui/menu.mothui", "label"),
                        "a bare name still reaches a node at any depth");
            test::check(fixture.surface->has_node("ui/menu.mothui", "title"),
                        "and one at the top of the layout");

            test::check(!fixture.surface->has_node("ui/menu.mothui", "play/"),
                        "a separator with no id after it answers nothing");
            test::check(!fixture.surface->has_node("ui/menu.mothui", ""),
                        "and so does an empty name");
        });
    }

    void a_press_on_a_button_reaches_the_surface() {
        test::section("a press on a button reaches the surface");

        with_a_cooked_tree([](const as::Content& content) {
            Fixture fixture;
            fixture.open(content);
            test::check(fixture.surface->show("ui/menu.mothui"), "the layout shows");
            test::check(fixture.surface->presses().empty(), "and nothing has been pressed");

            click(*fixture.surface, kOnTheButton);

            test::check(fixture.surface->presses().size() == 1, "a click records one press");
            if (fixture.surface->presses().size() == 1) {
                const engine::script::UiPress& press = fixture.surface->presses().front();
                test::check(press.layout == "ui/menu.mothui", "and it names the layout");
                test::check(press.node == "play", "and it names the node");
            }

            fixture.surface->clear_presses();
            test::check(fixture.surface->presses().empty(), "and a drain empties them");
        });
    }

    void a_press_names_the_reference_the_button_came_from() {
        test::section("a press names the reference the button came from");

        with_a_cooked_tree([](const as::Content& content) {
            Fixture fixture;
            fixture.open(content);
            test::check(fixture.surface->show("ui/hud.mothui"), "the layout shows");

            // Both rows are the same file, so both buttons inside them carry
            // the id `press`. A press that reported that id alone would name
            // the two of them identically, and a script could not tell which
            // was clicked.
            click(*fixture.surface, moth_ui::IntVec2{ 220, 20 });
            click(*fixture.surface, moth_ui::IntVec2{ 220, 80 });

            const std::span<const engine::script::UiPress> presses =
                fixture.surface->presses();
            test::check(presses.size() == 2, "two clicks record two presses");
            if (presses.size() == 2) {
                test::check(presses[0].node == "row one/press",
                            "and the first names the reference it came from");
                test::check(presses[1].node == "row two/press",
                            "and the second names the other one");

                // A press is handed straight back to the surface, which is the
                // whole reason the two vocabularies are one.
                test::check(fixture.surface->has_node(presses[0].layout, presses[0].node),
                            "and a press can be looked up by what it reported");
            }
        });
    }

    void a_click_that_misses_the_button_records_nothing() {
        test::section("a click that misses the button records nothing");

        with_a_cooked_tree([](const as::Content& content) {
            Fixture fixture;
            fixture.open(content);
            test::check(fixture.surface->show("ui/menu.mothui"), "the layout shows");

            click(*fixture.surface, kOffTheButton);
            test::check(fixture.surface->presses().empty(),
                        "a click away from the button records nothing");

            // A drag off a button cancels it. That is what moth_ui calls an
            // activation, and it is the only reading under which a press is the
            // whole gesture rather than the first half of one.
            (void)fixture.surface->OnEvent(
                moth_ui::EventMouseDown{ moth_ui::MouseButton::Left, kOnTheButton });
            (void)fixture.surface->OnEvent(
                moth_ui::EventMouseUp{ moth_ui::MouseButton::Left, kOffTheButton });
            test::check(fixture.surface->presses().empty(),
                        "and a press that came up elsewhere records nothing either");
        });
    }

    void a_hidden_layout_answers_no_click() {
        test::section("a hidden layout answers no click");

        with_a_cooked_tree([](const as::Content& content) {
            Fixture fixture;
            fixture.open(content);
            test::check(fixture.surface->show("ui/menu.mothui"), "the layout shows");
            test::check(fixture.surface->hide("ui/menu.mothui"), "and then it hides");

            click(*fixture.surface, kOnTheButton);
            test::check(fixture.surface->presses().empty(),
                        "a button of a hidden layout cannot be pressed");
        });
    }

    void the_topmost_layout_answers_first() {
        test::section("the topmost layout answers first");

        with_a_cooked_tree([](const as::Content& content) {
            // Both buttons cover the same rectangle. This is a pause menu over a
            // HUD, and only the menu may hear the click.
            Fixture fixture;
            fixture.open(content);
            test::check(fixture.surface->show("ui/hud.mothui"), "the HUD shows");
            test::check(fixture.surface->show("ui/menu.mothui"), "and the menu shows over it");

            click(*fixture.surface, kOnTheButton);

            test::check(fixture.surface->presses().size() == 1, "one press is recorded");
            if (fixture.surface->presses().size() == 1) {
                test::check(fixture.surface->presses().front().node == "play",
                            "and it came from the layout on top");
            }

            // Showing the HUD again raises it, so now it is the one that hears.
            fixture.surface->clear_presses();
            test::check(fixture.surface->show("ui/hud.mothui"), "showing the HUD raises it");
            click(*fixture.surface, kOnTheButton);

            test::check(fixture.surface->presses().size() == 1, "one press is recorded");
            if (fixture.surface->presses().size() == 1) {
                test::check(fixture.surface->presses().front().node == "quit",
                            "and now it is the HUD that hears it");
            }
        });
    }

    void a_reload_still_hears_a_press() {
        test::section("a reload still hears a press");

        with_a_cooked_tree([](const as::Content& content) {
            Fixture fixture;
            fixture.open(content);
            test::check(fixture.surface->show("ui/menu.mothui"), "the layout shows");

            const as::ManifestEntry* entry = content.find("ui/menu.mothui");
            test::check(entry != nullptr, "the layout is in the manifest");
            const std::array<engine::Guid, 1> changed{ entry->guid };
            test::check(fixture.surface->reload_layouts(changed), "the layout reloads");

            // The nodes are new, so every click action wired into the old ones is
            // gone with them. Wiring them again is what this proves.
            click(*fixture.surface, kOnTheButton);
            test::check(fixture.surface->presses().size() == 1,
                        "and the button still reaches the surface");
        });
    }

    void an_image_reload_rewires_the_buttons() {
        test::section("an image reload rewires the buttons");

        with_a_cooked_tree([](const as::Content& content) {
            // Node::ReloadEntity destroys and builds every child node again, so
            // every click action goes with them. Nothing reports that: the layout
            // draws correctly and the button is silent from then on.
            Fixture fixture;
            fixture.open(content);
            test::check(fixture.surface->show("ui/menu.mothui"), "the layout shows");

            fixture.surface->reload_images();

            click(*fixture.surface, kOnTheButton);
            test::check(fixture.surface->presses().size() == 1,
                        "the button still reaches the surface after an image reload");
        });
    }

    void the_screen_rectangle_reaches_a_layout_shown_later() {
        test::section("the screen rectangle reaches a layout shown later");

        with_a_cooked_tree([](const as::Content& content) {
            // A layout that has never been given a rectangle sizes every child at
            // zero, and a hit test then answers no click at all. So a layout
            // loaded after the rectangle was set has to be given it too.
            Fixture fixture;
            fixture.content = &content;
            fixture.surface = std::make_unique<ScriptSurface>(content, fixture.context);
            fixture.surface->set_screen_rect(moth_ui::IntRect{ { 0, 0 }, { 800, 600 } });

            test::check(fixture.surface->show("ui/menu.mothui"), "the layout shows");
            click(*fixture.surface, kOnTheButton);
            test::check(fixture.surface->presses().size() == 1,
                        "and its button answers a click");
        });
    }

#if defined(ENGINE_WITH_LUA)
    /**
     * The whole path, with nothing faked on either side of it.
     *
     * tests/test_script.cpp drives `on_ui_press` against a surface with no
     * moth_ui under it, and everything above drives a real surface with no script
     * over it. This is the one case where the two meet: a real moth_ui button
     * reports a press, the host delivers it to real Lua, and the script acts on
     * the same surface it was told about.
     *
     * The script hides the layout rather than writing a component, so the check
     * needs no described type of its own and the round trip closes where it
     * started.
     */
    void a_press_reaches_a_script_and_the_script_answers() {
        test::section("a press reaches a script and the script answers");

        with_a_cooked_tree([](const as::Content& content) {
            Fixture fixture;
            fixture.open(content);
            test::check(fixture.surface->show("ui/menu.mothui"), "the layout shows");

            engine::scene::ComponentRegistry components;
            components.add<engine::Transform>();
            engine::scene::World world;
            engine::script::Host host{ components };

            const std::string_view source = R"(
                function on_ui_press(pressed_layout, node)
                    if node == "play" then
                        ui.hide(pressed_layout)
                    end
                end
            )";
            const auto bytes = std::as_bytes(std::span{ source.data(), source.size() });
            const engine::Guid script = engine::Guid::generate();
            test::check(host.load(script, "press.lua", bytes), "the script compiles");

            const entt::entity entity = world.create();
            world.registry().emplace<engine::script::ScriptComponent>(
                entity, engine::script::ScriptComponent{ script });

            engine::script::Services services;
            services.ui = fixture.surface.get();

            // on_start runs here, which is what builds the instance. Without it
            // the press below reaches no listener at all.
            host.update(world, 0.0, services);

            click(*fixture.surface, kOnTheButton);
            test::check(fixture.surface->presses().size() == 1,
                        "the button reported one press");

            host.deliver_ui_events(world, services);

            test::check(!fixture.surface->visible("ui/menu.mothui"),
                        "and the script hid the layout it was told about");
            test::check(fixture.surface->presses().empty(),
                        "and the delivery drained the press");
            test::check(host.stopped_count() == 0, "and no call raised an error");
        });
    }
#endif

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
    a_reload_reports_the_layout_it_rebuilt();
    an_image_reload_reports_every_layout();
    a_reload_of_another_asset_changes_nothing();
    a_reference_resolves_through_the_content_tree();
    two_references_to_one_layout_answer_separately();
    a_path_searches_inside_what_the_segment_before_it_found();
    a_press_on_a_button_reaches_the_surface();
    a_press_names_the_reference_the_button_came_from();
    a_click_that_misses_the_button_records_nothing();
    a_hidden_layout_answers_no_click();
    the_topmost_layout_answers_first();
    a_reload_still_hears_a_press();
    an_image_reload_rewires_the_buttons();
    the_screen_rectangle_reaches_a_layout_shown_later();
#if defined(ENGINE_WITH_LUA)
    a_press_reaches_a_script_and_the_script_answers();
#endif
    return test::report();
}
