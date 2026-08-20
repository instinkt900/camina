#include "ui/layout_loader.h"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <vector>

namespace engine::ui {

    LayoutLoad read_layout(const assets::Content& content, Guid guid,
                           std::shared_ptr<moth_ui::Layout>& out) {
        std::vector<std::byte> bytes;
        if (!content.read_bytes(guid, bytes)) {
            return LayoutLoad::NotInTree;
        }

        nlohmann::json document;
        try {
            document = nlohmann::json::parse(bytes.begin(), bytes.end());
        } catch (const nlohmann::json::exception&) {
            return LayoutLoad::NotJson;
        }

        // Deserialize reads the format version out of the document itself, and
        // it refuses anything whose type is not Layout. The root path is what
        // moth_ui resolves a stored path against, and a cooked layout stores an
        // identity rather than a path, so nothing here needs one.
        auto layout = std::make_shared<moth_ui::Layout>();
        moth_ui::LayoutEntity::SerializeContext context;
        if (!layout->Deserialize(document, context)) {
            return LayoutLoad::NotALayout;
        }

        out = std::move(layout);
        return LayoutLoad::Ok;
    }

    const char* describe(LayoutLoad result) {
        switch (result) {
        case LayoutLoad::Ok:
            return "it read";
        case LayoutLoad::NotInTree:
            return "the content tree holds nothing with that identity";
        case LayoutLoad::NotJson:
            return "the bytes are not readable JSON";
        case LayoutLoad::NotALayout:
            return "the document parsed and it is not a layout";
        }
        return "it failed for a reason nothing named";
    }

} // namespace engine::ui
