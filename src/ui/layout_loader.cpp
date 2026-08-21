#include "ui/layout_loader.h"

#include "core/log.h"

#include <moth_ui/ilayout_provider.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstddef>
#include <utility>
#include <vector>

namespace engine::ui {

    namespace {

        /**
         * Reads a layout, and every layout it refers to, out of one content tree.
         *
         * moth_ui asks for a sub-layout while it is deserializing the layout that
         * names it, so this is re-entrant by design. The chain being read is kept
         * so that a layout which refers to itself is reported rather than followed
         * until the stack runs out.
         */
        class ContentProvider final : public moth_ui::ILayoutProvider {
        public:
            explicit ContentProvider(const assets::Content& content)
                : content_(content) {}

            /// @cond
            // This implements moth_ui::ILayoutProvider, which documents it.
            std::shared_ptr<moth_ui::Layout> GetLayout(const moth_ui::AssetId& id) override {
                // A cooked layout names a sub-layout by GUID, because the layout
                // rule resolved the authored path when it cooked. Nothing else is
                // a valid identity here: a path would need a directory, and the
                // whole reason this class exists is that there is not one.
                Guid guid;
                if (!Guid::parse(id.str(), guid)) {
                    ENGINE_LOG_ERROR("A layout refers to '{}', which is not an asset "
                                     "identity. A cooked layout names a sub-layout by "
                                     "identity, so this tree was not cooked by this engine.",
                                     id.str());
                    return nullptr;
                }

                std::shared_ptr<moth_ui::Layout> layout;
                const LayoutLoad result = read(guid, layout);
                if (result != LayoutLoad::Ok) {
                    ENGINE_LOG_ERROR("A layout refers to {}, which did not read: {}.",
                                     guid.to_text(), describe(result));
                    return nullptr;
                }
                return layout;
            }
            /// @endcond

            /// Reads one layout, with this provider set so its own refs resolve.
            [[nodiscard]] LayoutLoad read(Guid guid, std::shared_ptr<moth_ui::Layout>& out) {
                if (std::ranges::find(reading_, guid) != reading_.end()) {
                    return LayoutLoad::Cycle;
                }

                std::vector<std::byte> bytes;
                if (!content_.read_bytes(guid, bytes)) {
                    return LayoutLoad::NotInTree;
                }

                nlohmann::json document;
                try {
                    document = nlohmann::json::parse(bytes.begin(), bytes.end());
                } catch (const nlohmann::json::exception&) {
                    return LayoutLoad::NotJson;
                }

                // Deserialize reads the format version out of the document itself,
                // and it refuses anything whose type is not Layout. The root path
                // is what moth_ui resolves a stored path against, and a cooked
                // layout stores an identity, so nothing here needs one.
                moth_ui::LayoutEntity::SerializeContext context;
                context.m_layoutProvider = this;

                reading_.push_back(guid);
                auto layout = std::make_shared<moth_ui::Layout>();
                const bool read_back = layout->Deserialize(document, context);
                reading_.pop_back();

                if (!read_back) {
                    return LayoutLoad::NotALayout;
                }

                out = std::move(layout);
                return LayoutLoad::Ok;
            }

        private:
            const assets::Content& content_;

            /// The identities being read right now, outermost first.
            std::vector<Guid> reading_;
        };

    } // namespace

    LayoutLoad read_layout(const assets::Content& content, Guid guid,
                           std::shared_ptr<moth_ui::Layout>& out) {
        ContentProvider provider{ content };
        return provider.read(guid, out);
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
        case LayoutLoad::Cycle:
            return "it refers back to a layout that is already being read";
        }
        return "it failed for a reason nothing named";
    }

} // namespace engine::ui
