#pragma once

/**
 * @file
 * @brief The game UI a script drives, over named moth_ui layouts.
 *
 * M10.6. `script::UiSurface` declares what a script may do to the UI, and this
 * is the one implementation of it. The split exists because `engine_core` may
 * not name a moth_ui type: `DESIGN.md` section 8.5 keeps game UI optional, so
 * `src/script/` calls the interface and `src/ui/` knows what a layout is.
 *
 * A build with `with_ui=False` links none of this and passes no surface. Every
 * call in the `ui` table then answers false, the same way an action reads false
 * when nobody bound an input module.
 *
 * **This owns every layout a script asked for.** The runtime used to hold one
 * layout, loaded at start. A script names any layout by its source path and the
 * surface loads it on demand, because M10.7 wants a main menu, a pause menu and
 * a HUD as three separate layouts.
 */

#include "assets/content.h"
#include "core/guid.h"
#include "script/ui_surface.h"

#include <moth_ui/context.h>
#include <moth_ui/events/event_listener.h>
#include <moth_ui/nodes/node.h>
#include <moth_ui/utils/rect.h>

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace engine::ui {

    /**
     * @brief Every layout a script named, and what a script may do to them.
     *
     * It is the `moth_ui::IEventListener` the input bridge sends a frame to, so
     * `InputBridge::take` takes it directly and no caller holds a layout root.
     *
     * @warning Nothing here hands out a pointer to a node. A reload frees the
     * whole node tree, so every call looks the node up again from the two names
     * it was given. `DESIGN.md` section 8.4 records that trap four times over.
     *
     * @code
     * engine::ui::ScriptSurface surface{ content, context };
     * surface.set_screen_rect({ { 0, 0 }, { width, height } });
     * bridge.take(frame, &surface);   // a press lands in presses()
     * surface.update(ticks);
     * surface.draw();                 // into the moth_ui renderer
     * @endcode
     */
    class ScriptSurface final : public script::UiSurface, public moth_ui::IEventListener {
    public:
        /**
         * @brief Builds a surface over a content tree and a moth_ui context.
         *
         * @param content Where a layout and its images are read from. Held, not
         *                owned, and it must outlive this.
         * @param context What turns a layout into live nodes. Held, not owned,
         *                and it must outlive this.
         */
        ScriptSurface(const assets::Content& content, moth_ui::Context& context);

        ScriptSurface(const ScriptSurface&) = delete;
        ScriptSurface& operator=(const ScriptSurface&) = delete;
        ScriptSurface(ScriptSurface&&) = delete;
        ScriptSurface& operator=(ScriptSurface&&) = delete;

        /// @brief Drops every layout and every click action wired into one.
        ~ScriptSurface() override;

        /// @cond
        // These implement script::UiSurface, which documents every one of them.
        // Repeating the block here would be two copies to keep in step.
        bool show(std::string_view layout) override;
        bool hide(std::string_view layout) override;
        [[nodiscard]] bool visible(std::string_view layout) const override;
        [[nodiscard]] bool has_node(std::string_view layout,
                                    std::string_view node) const override;
        [[nodiscard]] std::string text(std::string_view layout,
                                       std::string_view node) const override;
        bool set_text(std::string_view layout, std::string_view node,
                      std::string_view text) override;
        [[nodiscard]] bool node_visible(std::string_view layout,
                                        std::string_view node) const override;
        bool set_node_visible(std::string_view layout, std::string_view node,
                              bool visible) override;
        bool set_image(std::string_view layout, std::string_view node,
                       std::string_view image) override;
        [[nodiscard]] std::span<const script::UiPress> presses() const override;
        void clear_presses() override;
        /// @endcond

        /**
         * @brief Sends one event to the layouts that are showing.
         *
         * The topmost layout answers first, and the first one to consume the
         * event ends the walk. So a pause menu shown over a HUD takes the click
         * and the HUD never sees it.
         *
         * @param event The event to route.
         * @return True when a layout consumed it.
         */
        bool OnEvent(const moth_ui::Event& event) override;

        /**
         * @brief Sets the rectangle every layout lays its children out from.
         *
         * @warning Call this before the events as well as before the draw. A
         * layout that has never been given a rectangle sizes every child at
         * zero, and a hit test then answers no click at all.
         *
         * @param rect The whole drawing area, in pixels.
         */
        void set_screen_rect(const moth_ui::IntRect& rect);

        /**
         * @brief Advances the animation of every layout that is showing.
         * @param ticks Elapsed time in milliseconds.
         */
        void update(std::uint32_t ticks);

        /**
         * @brief Draws every layout that is showing, bottom to top.
         *
         * A layout shown later draws over one shown earlier, which is the
         * reverse of the order OnEvent walks.
         */
        void draw();

        /**
         * @brief Builds again every layout an identity in @p changed names.
         *
         * **A layout that will not read keeps the one already drawing.** A
         * person editing a layout passes through broken states on the way to a
         * working one, and dropping the UI at each of them is worse than
         * drawing the last good version. `reload_ui_layout` had this rule for
         * one layout and it holds for all of them.
         *
         * Whether a layout is showing, and where it sits in the order, both
         * survive. What a script wrote into a node does not: the nodes are
         * built again from the layout file, so a changed text goes back to the
         * authored one.
         *
         * @param changed The identities that were cooked again.
         * @return True when at least one layout was built again. The caller
         *         must then call `InputBridge::forget`, because the new nodes
         *         know nothing about a button the old ones took.
         */
        [[nodiscard]] bool reload_layouts(std::span<const Guid> changed);

        /**
         * @brief Asks every layout for its images again, after one was cooked.
         *
         * M10.4 lists the four holders of a UI texture. This is the third of
         * them, for every layout rather than for the one the runtime held.
         *
         * @warning `moth_ui::Node::ReloadEntity` destroys and builds every child
         * node again, so every click action wired into the old nodes goes with
         * them. This wires them again. Without that a button answers once and
         * is silent after the first image reload, which nothing would report.
         */
        void reload_images();

        /// @brief How many layouts are loaded, showing or not.
        /// @return The count.
        [[nodiscard]] std::size_t loaded_count() const;

        /// @brief How many layouts are showing.
        /// @return The count.
        [[nodiscard]] std::size_t showing_count() const;

    private:
        /// One layout a script named, whether it is showing or not.
        struct Loaded {
            std::string source;                  ///< The path a script names it by.
            Guid guid;                           ///< The identity it was read under.
            std::shared_ptr<moth_ui::Node> root; ///< The live nodes.
            bool showing = false;                ///< Whether it draws and takes events.
        };

        /// Finds a loaded layout by source path, or null.
        [[nodiscard]] const Loaded* find(std::string_view layout) const;
        /// Finds a loaded layout by source path, or null.
        [[nodiscard]] Loaded* find(std::string_view layout);
        /// Finds a node of a loaded layout, or null. Null for either name missing.
        [[nodiscard]] std::shared_ptr<moth_ui::Node> node_of(std::string_view layout,
                                                             std::string_view node) const;
        /// Reads and instantiates the layout an identity names, or null.
        [[nodiscard]] std::shared_ptr<moth_ui::Node> build(std::string_view source,
                                                           Guid guid) const;
        /**
         * Walks a tree once, from a node whose path is @p path.
         *
         * It wires a click action into every node that can take one, so a press
         * reports the path a script would look the node up by. It also reports
         * an id no script could name.
         *
         * @param root The node to walk. The layout root passes an empty path.
         * @param source The source path of the layout, for a press and a report.
         * @param path The path of @p root itself, from the layout root.
         */
        void wire_tree(const std::shared_ptr<moth_ui::Node>& root, const std::string& source,
                       const std::string& path);

        const assets::Content& content_;
        moth_ui::Context& context_;

        /**
         * Every layout, in the order they were shown. The back is on top.
         *
         * The records are held by pointer rather than by value so that growing
         * this vector cannot move one. `show` loads a layout, which grows this,
         * and a record read before that call must still be the same record
         * after it.
         */
        std::vector<std::unique_ptr<Loaded>> layouts_;

        /// The rectangle every layout lays out from, kept so a new one gets it.
        moth_ui::IntRect screen_;

        /// The presses the frames gathered since the last drain.
        std::vector<script::UiPress> presses_;
    };

} // namespace engine::ui
