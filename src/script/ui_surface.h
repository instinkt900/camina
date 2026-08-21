#pragma once

/**
 * @file
 * @brief What a script may do to the game UI.
 *
 * M10.6. `src/script/` sits in `engine_core`, and `DESIGN.md` §8.5 keeps
 * `engine_core` free of moth_ui so that a build without game UI carries none of
 * it. So the binding cannot name a moth_ui type, and this interface is the seam:
 * `src/script/` calls it, and `engine::ui::ScriptSurface` in `src/ui/` is the one
 * implementation that knows what a layout is.
 *
 * A build with no UI passes no surface. Every call in the `ui` table then
 * answers false, the same way an action reads false when nobody bound an input
 * module. See `script::Services`.
 *
 * **Everything here is named by string.** A layout is its source path and a node
 * is the id somebody typed in `moth_editor`. Nothing hands a script a pointer to
 * a node, because a hot reload frees the node tree and a script that held one
 * would be holding freed memory on the next frame. `DESIGN.md` §8.4 has that
 * trap three times over.
 */

#include <cstddef>
#include <span>
#include <string>
#include <string_view>

namespace engine::script {

    /**
     * @brief One press a layout reported, named the way a script names things.
     *
     * A press is the whole gesture: the pointer went down on a node and came up
     * on the same node. That is what `moth_ui::UIButton` calls an activation,
     * and it is the only reading under which a drag off a button cancels it.
     */
    struct UiPress {
        std::string layout; ///< The source path of the layout that reported it.
        std::string node;   ///< The id of the node inside that layout.
    };

    /**
     * @brief The game UI, as a script sees it.
     *
     * @warning A press is recorded on the frame clock and delivered on the fixed
     * step, because M10.5 settled that a UI event is a frame event and a script
     * runs on the step. So the presses gather between two steps and
     * `Host::deliver_ui_events` drains them, the same way
     * `Host::deliver_physics_events` drains the touches of one step.
     */
    class UiSurface {
    public:
        UiSurface() = default;
        UiSurface(const UiSurface&) = delete;
        UiSurface& operator=(const UiSurface&) = delete;
        UiSurface(UiSurface&&) = delete;
        UiSurface& operator=(UiSurface&&) = delete;
        virtual ~UiSurface() = default;

        /**
         * @brief Shows a layout, and loads it when it is not loaded yet.
         *
         * @param layout The source path, for example `ui/pause.mothui`.
         * @return False when there is no such layout, or when it would not read.
         */
        virtual bool show(std::string_view layout) = 0;

        /**
         * @brief Hides a layout. It stays loaded.
         * @param layout The source path.
         * @return False when no layout of that name is loaded.
         */
        virtual bool hide(std::string_view layout) = 0;

        /// @brief Whether a layout is loaded and showing.
        /// @param layout The source path.
        /// @return True while it draws.
        [[nodiscard]] virtual bool visible(std::string_view layout) const = 0;

        /// @brief Whether a layout holds a node with that id.
        /// @param layout The source path.
        /// @param node The node id.
        /// @return True when both are there.
        [[nodiscard]] virtual bool has_node(std::string_view layout,
                                            std::string_view node) const = 0;

        /**
         * @brief What a text node shows.
         * @param layout The source path.
         * @param node The node id.
         * @return The text, or an empty string when the node is missing or shows none.
         */
        [[nodiscard]] virtual std::string text(std::string_view layout,
                                               std::string_view node) const = 0;

        /**
         * @brief Changes what a text node shows.
         * @param layout The source path.
         * @param node The node id.
         * @param text The new text.
         * @return False when the node is missing or is not a text node.
         */
        virtual bool set_text(std::string_view layout, std::string_view node,
                              std::string_view text) = 0;

        /// @brief Whether a node is shown.
        /// @param layout The source path.
        /// @param node The node id.
        /// @return True while the node draws.
        [[nodiscard]] virtual bool node_visible(std::string_view layout,
                                                std::string_view node) const = 0;

        /**
         * @brief Shows or hides one node.
         * @param layout The source path.
         * @param node The node id.
         * @param visible True to show it.
         * @return False when the node is missing.
         */
        virtual bool set_node_visible(std::string_view layout, std::string_view node,
                                      bool visible) = 0;

        /**
         * @brief Changes the image an image node draws.
         *
         * @param layout The source path.
         * @param node The node id.
         * @param image The source path of the image, the way a layout names one.
         * @return False when the node is missing, is not an image node, or the
         *         image is not in the content tree.
         */
        virtual bool set_image(std::string_view layout, std::string_view node,
                               std::string_view image) = 0;

        /**
         * @brief The presses gathered since the last drain.
         * @return Every press, in the order the frames reported them.
         */
        [[nodiscard]] virtual std::span<const UiPress> presses() const = 0;

        /// @brief Forgets every press. `Host::deliver_ui_events` calls this.
        virtual void clear_presses() = 0;
    };

} // namespace engine::script
