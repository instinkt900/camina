// M10.6. The one implementation of script::UiSurface.
//
// Every call here takes two strings and looks the node up again. That is slower
// than keeping a pointer and it is the only shape that survives a hot reload,
// which frees the whole node tree and builds a new one. DESIGN.md section 8.4
// records that trap four times, and this file is where it would bite hardest.

#include "ui/script_surface.h"

#include "core/log.h"
#include "ui/input_bridge.h"
#include "ui/layout_loader.h"

#include <moth_ui/flow/iclickable.h>
#include <moth_ui/layout/layout.h>
#include <moth_ui/layout/layout_entity_group.h>
#include <moth_ui/node_factory.h>
#include <moth_ui/nodes/group.h>
#include <moth_ui/nodes/node_image.h>
#include <moth_ui/nodes/node_text.h>
#include <moth_ui/widgets/widget.h>

#include <algorithm>
#include <filesystem>
#include <utility>

namespace engine::ui {

    namespace {

        /**
         * Registers the widgets moth_ui ships, once.
         *
         * moth_ui registers a widget from a static initializer inside the
         * translation unit that defines it. It is a static library here and no
         * engine code names `moth_ui::UIButton`, so the linker drops that
         * object file and the registration never runs. A layout that names the
         * class "button" then builds a plain group: it draws correctly, it
         * takes no click action, and nothing reports a fault.
         *
         * So the whole press path depends on this call, and its absence would
         * look exactly like a layout somebody authored wrongly.
         */
        void register_widgets_once() {
            static const bool done = [] {
                moth_ui::EnsureWidgetsRegistered();
                return true;
            }();
            (void)done;
        }

    } // namespace

    ScriptSurface::ScriptSurface(const assets::Content& content, moth_ui::Context& context)
        : content_(content)
        , context_(context) {
        register_widgets_once();
    }

    ScriptSurface::~ScriptSurface() = default;

    const ScriptSurface::Loaded* ScriptSurface::find(std::string_view layout) const {
        const auto match = std::ranges::find_if(
            layouts_, [layout](const std::unique_ptr<Loaded>& held) {
                return held->source == layout;
            });
        return match == layouts_.end() ? nullptr : match->get();
    }

    ScriptSurface::Loaded* ScriptSurface::find(std::string_view layout) {
        // Written out rather than casting the const away from the other one. A
        // const_cast here would be safe and it would still be the only one in
        // this engine, and four lines are cheaper than an exception to a rule.
        const auto match = std::ranges::find_if(
            layouts_, [layout](const std::unique_ptr<Loaded>& held) {
                return held->source == layout;
            });
        return match == layouts_.end() ? nullptr : match->get();
    }

    std::shared_ptr<moth_ui::Node> ScriptSurface::build(std::string_view source,
                                                        Guid guid) const {
        std::shared_ptr<moth_ui::Layout> layout;
        const LayoutLoad read = read_layout(content_, guid, layout);
        if (read != LayoutLoad::Ok) {
            ENGINE_LOG_ERROR("The UI layout {} did not load: {}.", source, describe(read));
            return nullptr;
        }

        // NodeFactory rather than Layout::Instantiate. Instantiate builds a
        // plain group and never asks what class the layout named, so a layout
        // whose root is a widget would come back as a group that draws right
        // and answers nothing. The factory is the only call that reads a class.
        // The cast picks the group overload. Both overloads accept a Layout and
        // neither is a better match, so the call is ambiguous without it.
        std::shared_ptr<moth_ui::Node> root = moth_ui::NodeFactory::Get().Create(
            context_, std::static_pointer_cast<moth_ui::LayoutEntityGroup>(layout));
        if (!root) {
            ENGINE_LOG_ERROR("The UI layout {} loaded and would not instantiate.", source);
            return nullptr;
        }
        return root;
    }

    void ScriptSurface::wire_presses(const std::shared_ptr<moth_ui::Node>& root,
                                     const std::string& source) {
        if (!root) {
            return;
        }

        // The action captures the two names rather than the node, because the
        // node is what a reload frees. It captures this, and this owns the node
        // it is wired into, so the action cannot outlive the surface.
        if (auto* clickable = dynamic_cast<moth_ui::IClickable*>(root.get())) {
            const std::string& id = root->GetId();
            if (id.empty()) {
                // A press names a node, so one with no id has nothing to report
                // under. This is a layout somebody has not finished, and it is
                // worth saying so rather than dropping the press in silence.
                ENGINE_LOG_WARN("The UI layout {} holds a button with no id, so a press on "
                                "it reaches no script.",
                                source);
            } else {
                clickable->SetClickAction([this, source, id] {
                    presses_.push_back(script::UiPress{ source, id });
                });
            }
        }

        if (const auto group = std::dynamic_pointer_cast<moth_ui::Group>(root)) {
            for (const std::shared_ptr<moth_ui::Node>& child : group->GetChildren()) {
                wire_presses(child, source);
            }
        }
    }

    bool ScriptSurface::show(std::string_view layout) {
        Loaded* held = find(layout);

        if (held == nullptr) {
            const assets::ManifestEntry* entry = content_.find(layout);
            if (entry == nullptr) {
                ENGINE_LOG_ERROR("A script asked for the UI layout {}, which the game "
                                 "content tree does not hold.",
                                 layout);
                return false;
            }

            std::shared_ptr<moth_ui::Node> root = build(layout, entry->guid);
            if (!root) {
                return false;
            }

            auto record = std::make_unique<Loaded>();
            record->source = std::string{ layout };
            record->guid = entry->guid;
            record->root = std::move(root);
            wire_presses(record->root, record->source);
            record->root->SetScreenRect(screen_);
            layouts_.push_back(std::move(record));
            layouts_.back()->showing = true;
            return true;
        }

        held->showing = true;

        // Showing raises a layout to the top, so a pause menu shown over a HUD
        // draws over it and answers a click before it. A script that shows the
        // same layouts in the same order every step gets the same order back.
        const auto at = std::ranges::find_if(layouts_,
                                             [held](const std::unique_ptr<Loaded>& one) {
                                                 return one.get() == held;
                                             });
        std::rotate(at, at + 1, layouts_.end());
        return true;
    }

    bool ScriptSurface::hide(std::string_view layout) {
        Loaded* held = find(layout);
        if (held == nullptr) {
            return false;
        }
        held->showing = false;
        return true;
    }

    bool ScriptSurface::visible(std::string_view layout) const {
        const Loaded* held = find(layout);
        return held != nullptr && held->showing;
    }

    std::shared_ptr<moth_ui::Node> ScriptSurface::node_of(std::string_view layout,
                                                          std::string_view node) const {
        const Loaded* held = find(layout);
        if (held == nullptr || !held->root) {
            return nullptr;
        }
        return held->root->FindChild(node);
    }

    bool ScriptSurface::has_node(std::string_view layout, std::string_view node) const {
        return node_of(layout, node) != nullptr;
    }

    std::string ScriptSurface::text(std::string_view layout, std::string_view node) const {
        const auto found = std::dynamic_pointer_cast<moth_ui::NodeText>(node_of(layout, node));
        return found ? found->GetText() : std::string{};
    }

    bool ScriptSurface::set_text(std::string_view layout, std::string_view node,
                                 std::string_view text) {
        const auto found = std::dynamic_pointer_cast<moth_ui::NodeText>(node_of(layout, node));
        if (!found) {
            return false;
        }
        found->SetText(text);
        return true;
    }

    bool ScriptSurface::node_visible(std::string_view layout, std::string_view node) const {
        const std::shared_ptr<moth_ui::Node> found = node_of(layout, node);
        return found && found->IsVisible();
    }

    bool ScriptSurface::set_node_visible(std::string_view layout, std::string_view node,
                                         bool visible) {
        const std::shared_ptr<moth_ui::Node> found = node_of(layout, node);
        if (!found) {
            return false;
        }
        found->SetVisible(visible);
        return true;
    }

    bool ScriptSurface::set_image(std::string_view layout, std::string_view node,
                                  std::string_view image) {
        const auto found =
            std::dynamic_pointer_cast<moth_ui::NodeImage>(node_of(layout, node));
        if (!found) {
            return false;
        }

        // The node is not touched until the name is known to resolve, because
        // `NodeImage::Load` drops the image it holds before it asks the factory.
        // A typo would otherwise leave the node blank rather than unchanged.
        //
        // This guard is conservative rather than authoritative: `ImageFactory`
        // decides what an identity means, and the check below reads what it
        // decided. Both are needed. An identity that is in the manifest and will
        // not read passes this guard and fails that check.
        Guid parsed;
        const std::filesystem::path normalized =
            std::filesystem::path{ image }.lexically_normal();
        if (!Guid::parse(image, parsed) &&
            content_.find(normalized.generic_string()) == nullptr) {
            ENGINE_LOG_ERROR("A script set the image of {} in {} to {}, which the game "
                             "content tree does not hold. The node keeps the image it had.",
                             node, layout, image);
            return false;
        }

        found->Load(moth_ui::AssetId{ std::string{ image } });
        return found->GetImage() != nullptr;
    }

    std::span<const script::UiPress> ScriptSurface::presses() const {
        return presses_;
    }

    void ScriptSurface::clear_presses() {
        presses_.clear();
    }

    bool ScriptSurface::OnEvent(const moth_ui::Event& event) {
        // Topmost first, and the first layout to consume ends the walk. The
        // draw order is the other way round, so what draws on top answers first.
        for (auto held = layouts_.rbegin(); held != layouts_.rend(); ++held) {
            if (!(*held)->showing) {
                continue;
            }

            // LayoutListener is what repeats moth_ui's own routing: a captured
            // node answers before the depth-first broadcast. M10.5 settled that,
            // and building one here rather than keeping one per layout is what
            // keeps the record free to move.
            LayoutListener listener{ &(*held)->root };
            if (listener.OnEvent(event)) {
                return true;
            }
        }
        return false;
    }

    void ScriptSurface::set_screen_rect(const moth_ui::IntRect& rect) {
        screen_ = rect;
        for (const std::unique_ptr<Loaded>& held : layouts_) {
            if (held->root) {
                held->root->SetScreenRect(rect);
            }
        }
    }

    void ScriptSurface::update(std::uint32_t ticks) {
        for (const std::unique_ptr<Loaded>& held : layouts_) {
            if (held->showing && held->root) {
                held->root->Update(ticks);
            }
        }
    }

    void ScriptSurface::draw() {
        for (const std::unique_ptr<Loaded>& held : layouts_) {
            if (held->showing && held->root) {
                held->root->Draw();
            }
        }
    }

    bool ScriptSurface::reload_layouts(std::span<const Guid> changed) {
        bool any = false;

        for (const std::unique_ptr<Loaded>& held : layouts_) {
            // The identity is read again rather than reused. A cook that gives
            // the layout a new sidecar gives it a new identity, and the old one
            // then names nothing.
            const assets::ManifestEntry* now = content_.find(held->source);
            const Guid current = now != nullptr ? now->guid : Guid{};

            const bool named = std::ranges::any_of(changed, [&](Guid guid) {
                return (held->guid.valid() && guid == held->guid) ||
                       (current.valid() && guid == current);
            });
            if (!named) {
                continue;
            }

            if (now == nullptr) {
                ENGINE_LOG_ERROR("The UI layout {} left the content tree. The one already "
                                 "drawing stays up.",
                                 held->source);
                continue;
            }

            std::shared_ptr<moth_ui::Node> rebuilt = build(held->source, current);
            if (!rebuilt) {
                ENGINE_LOG_ERROR("The UI layout {} would not reload. The one already "
                                 "drawing stays up.",
                                 held->source);
                continue;
            }

            held->guid = current;
            held->root = std::move(rebuilt);
            wire_presses(held->root, held->source);
            held->root->SetScreenRect(screen_);
            any = true;
            ENGINE_LOG_INFO("The UI layout {} reloaded.", held->source);
        }

        return any;
    }

    void ScriptSurface::reload_images() {
        for (const std::unique_ptr<Loaded>& held : layouts_) {
            if (!held->root) {
                continue;
            }
            held->root->ReloadEntity();

            // ReloadEntity builds every child node again, so the click actions
            // wired into the old ones are gone with them.
            wire_presses(held->root, held->source);
            held->root->SetScreenRect(screen_);
        }
    }

    std::size_t ScriptSurface::loaded_count() const {
        return layouts_.size();
    }

    std::size_t ScriptSurface::showing_count() const {
        return static_cast<std::size_t>(
            std::ranges::count_if(layouts_, [](const std::unique_ptr<Loaded>& held) {
                return held->showing;
            }));
    }

} // namespace engine::ui
