// M10.3 tests for reading a cooked moth_ui layout by identity.
//
// These run with no device, for the reason test_ui_renderer.cpp gives: the
// reading half names no Vulkan type. Turning a layout into live nodes needs a
// moth_ui::Context, which needs a renderer and both factories, and that half
// stays in the runtime.
//
// The failure cases matter more than the success one. A layout that will not
// read must say which failure it was, because the runtime keeps the layout
// already drawing and a person then has only the message to work from.

#include "assets/content.h"
#include "assets/manifest.h"
#include "check.h"
#include "import/cook.h"
#include "ui/layout_loader.h"

#include <array>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <fstream>
#include <string_view>

namespace {

    namespace as = engine::assets;
    using engine::ui::LayoutLoad;

    /// Names this binary's scratch tree. See test::scratch.
    constexpr std::string_view kSuite = "ui_layout";

    std::filesystem::path scratch(std::string_view name) {
        return test::scratch(kSuite, name);
    }

    void write_file(const std::filesystem::path& path, std::string_view text) {
        std::filesystem::create_directories(path.parent_path());
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file << text;
    }

    /// The identity the manifest gives a source path.
    engine::Guid identity_of(const as::Content& content, std::string_view source) {
        const as::ManifestEntry* entry = content.find(source);
        return entry != nullptr ? entry->guid : engine::Guid{};
    }

    /**
     * Cooks a tree holding a layout, a script and a document that is neither.
     *
     * Three on purpose. The script and the odd document drive the two failure
     * cases, and both are real cooked assets rather than files a test wrote
     * beside the tree, so the reader meets them the way a running game would.
     */
    void with_a_cooked_tree(const std::function<void(const as::Content&)>& run) {
        const std::filesystem::path source = scratch("src");
        const std::filesystem::path out = scratch("out");

        write_file(source / "ui" / "main.mothui",
                   R"({"type":"Layout","mothui_version":1,"children":[]})");
        write_file(source / "scripts" / "spin.lua", "-- not JSON at all\n");

        // Readable JSON whose root is not a Layout. A file hand-edited to the
        // wrong shape looks exactly like this, and so does a reference left
        // pointing at the wrong asset.
        write_file(source / "ui" / "wrong.mothui", R"({"type":"Group","children":[]})");

        // A layout that refers to itself. The cooker is happy with it, because a
        // reference is resolved to an identity and nothing there follows one.
        // Reading it is what would recurse until the stack ran out.
        write_file(source / "ui" / "loop.mothui",
                   R"({"type":"Layout","mothui_version":1,"children":[)"
                   R"({"type":"Ref","id":"itself","layoutPath":"loop.mothui",)"
                   R"("propertyOverrides":[]}]})");

        const engine::import::Options options{ .content = source, .out = out };
        engine::import::Result result;
        test::check(engine::import::cook_all(options, result), "the tree cooks");

        as::Content content;
        test::check(content.open(out), "and it opens");
        run(content);

        test::remove_tree(source.parent_path());
    }

    void a_cooked_layout_reads_by_identity() {
        with_a_cooked_tree([](const as::Content& content) {
            const engine::Guid guid = identity_of(content, "ui/main.mothui");
            test::check(guid.valid(), "the layout is in the manifest");

            std::shared_ptr<moth_ui::Layout> layout;
            test::check(engine::ui::read_layout(content, guid, layout) == LayoutLoad::Ok,
                        "and it reads by that identity");
            test::check(layout != nullptr, "and it comes back");
        });
    }

    void a_layout_that_refers_to_itself_is_reported() {
        with_a_cooked_tree([](const as::Content& content) {
            // moth_ui asks for a sub-layout while it is reading the layout that
            // names it, so the reader is re-entrant and a cycle would follow
            // itself until the stack ran out. Nothing else stops that: the
            // cooker resolves a reference to an identity and never follows one.
            const engine::Guid guid = identity_of(content, "ui/loop.mothui");
            test::check(guid.valid(), "the self-referring layout is in the manifest");

            std::shared_ptr<moth_ui::Layout> layout;
            const LayoutLoad result = engine::ui::read_layout(content, guid, layout);

            // The outer read still succeeds. The reference is the child that
            // will not resolve, and moth_ui drops a child it cannot read rather
            // than failing the layout around it.
            test::check(result == LayoutLoad::Ok, "the layout itself still reads");
            test::check(layout != nullptr, "and it comes back");
            if (layout != nullptr) {
                test::check(layout->m_children.empty(),
                            "with the reference that closed the loop dropped");
            }
        });
    }

    void an_identity_nothing_holds_is_named_as_such() {
        with_a_cooked_tree([](const as::Content& content) {
            std::shared_ptr<moth_ui::Layout> layout;
            const LayoutLoad result =
                engine::ui::read_layout(content, engine::Guid::generate(), layout);
            test::check(result == LayoutLoad::NotInTree,
                        "an identity the tree does not hold is reported as missing");
            test::check(layout == nullptr, "and nothing is handed back");
        });
    }

    void an_asset_that_is_not_json_is_named_as_such() {
        with_a_cooked_tree([](const as::Content& content) {
            // A cooked script is its source text, so this is a real asset that
            // is really not JSON. Pointing the layout loader at the wrong
            // identity is what a stale reference does.
            const engine::Guid guid = identity_of(content, "scripts/spin.lua");
            test::check(guid.valid(), "the script is in the manifest");

            std::shared_ptr<moth_ui::Layout> layout;
            test::check(engine::ui::read_layout(content, guid, layout) == LayoutLoad::NotJson,
                        "an asset that is not JSON is reported as such");
        });
    }

    void a_document_that_is_not_a_layout_is_named_as_such() {
        with_a_cooked_tree([](const as::Content& content) {
            const engine::Guid guid = identity_of(content, "ui/wrong.mothui");
            test::check(guid.valid(), "the odd document is in the manifest");

            std::shared_ptr<moth_ui::Layout> layout;
            test::check(engine::ui::read_layout(content, guid, layout) ==
                            LayoutLoad::NotALayout,
                        "readable JSON that is not a layout is reported as such");
            test::check(layout == nullptr, "and nothing is handed back");
        });
    }

    void every_failure_says_something_different() {
        // The runtime keeps the layout already drawing when a reload fails, so
        // the message is all a person gets. Two failures reading the same would
        // send somebody looking in the wrong place.
        const std::array<LayoutLoad, 4> all{ LayoutLoad::Ok, LayoutLoad::NotInTree,
                                             LayoutLoad::NotJson, LayoutLoad::NotALayout };
        for (std::size_t a = 0; a < all.size(); ++a) {
            const std::string_view first = engine::ui::describe(all.at(a));
            test::check(!first.empty(), "every result describes itself");
            for (std::size_t b = a + 1; b < all.size(); ++b) {
                test::check(first != engine::ui::describe(all.at(b)),
                            "and no two results read the same");
            }
        }
    }

} // namespace

int main() {
    a_cooked_layout_reads_by_identity();
    a_layout_that_refers_to_itself_is_reported();
    an_identity_nothing_holds_is_named_as_such();
    an_asset_that_is_not_json_is_named_as_such();
    a_document_that_is_not_a_layout_is_named_as_such();
    every_failure_says_something_different();
    return test::report();
}
