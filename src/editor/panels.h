#pragma once

/**
 * @file
 * @brief The panels both applications draw: the view, the hierarchy, and the
 * generated inspector.
 *
 * These lived in `apps/runtime/main.cpp` until M9.2, because M2 had nowhere
 * else to put them. Two applications cannot share a panel that sits in the
 * `main.cpp` of one of them, so they moved here.
 *
 * **These are not behind `WITH_EDITOR`.** Hard rule 3 says the editor is an
 * application and not a build mode, and the inspector runs as a debug overlay
 * in the runtime. So this is code both applications link. See DESIGN.md
 * section 6.
 *
 * The header names no ImGui type. `panels.cpp` holds every ImGui call, the way
 * `reflect/inspector.h` does, so a caller that never opens a window still
 * compiles.
 *
 * A panel opens its own window and closes it. Call each one between
 * `gfx::imgui_new_frame()` and `gfx::imgui_render()`.
 */

#include "core/entt.h"
#include "editor/play_mode.h"
#include "editor/view_settings.h"
#include "gfx/imgui.h"
#include "gfx/types.h"

#include <entt/entity/fwd.hpp>

#include <cstdint>
#include <filesystem>

namespace engine::assets {
    class Content;
}

namespace engine::scene {
    class World;
}

namespace engine::editor {

    /// @brief Which handles the gizmo shows.
    enum class GizmoOperation : std::uint8_t {
        Translate, ///< Arrows that move the entity.
        Rotate,    ///< Rings that turn it.
        Scale,     ///< Boxes that resize it.
    };

    /// @brief Which axes those handles line up with.
    enum class GizmoSpace : std::uint8_t {
        World, ///< The axes of the world, whatever the entity is turned to.
        Local, ///< The axes of the entity itself.
    };

    /**
     * @brief What the gizmo is set to, which the viewport bar edits.
     *
     * Plain data with no ImGui type and no ImGuizmo type in it. The panel
     * changes it, and whoever draws a gizmo reads it. `apps/editor/gizmo.h`
     * turns these two into what ImGuizmo asks for.
     */
    struct GizmoControls {
        /// @brief Which handles the gizmo shows.
        GizmoOperation operation = GizmoOperation::Translate;
        /// @brief Which axes those handles line up with.
        GizmoSpace space = GizmoSpace::World;
    };

    /**
     * @brief Something to draw inside the viewport window, over the picture.
     *
     * A gizmo has to draw into that window's draw list and needs the rectangle
     * the picture occupies, and both are only knowable between the `Begin` and
     * the `End` of the panel. So the caller hands over a function to run there
     * rather than the panel handing out an ImGui type.
     *
     * @warning The rectangle is in screen coordinates, which is what ImGui
     * reports and what ImGuizmo::SetRect expects. It is not relative to the
     * panel.
     */
    struct ViewportOverlay {
        /// @brief Called with the picture rectangle, or null for nothing to draw.
        void (*draw)(void* user, float x, float y, float width, float height) = nullptr;
        /// @brief Handed back to draw(), because a function pointer carries no state.
        void* user = nullptr;
    };

    /**
     * @brief What the viewport panel reported about the frame it drew.
     *
     * The focus is here because a running session reads the keyboard, and a
     * person typing a value into the inspector must not drive the game with it.
     * The panel knows which window the user is working in and nothing else
     * does.
     */
    struct ViewportReport {
        /// @brief What the user clicked on the play bar.
        PlayRequest request = PlayRequest::None;

        /// @brief True while this panel is the focused one.
        bool focused = false;
    };

    /**
     * @brief Places the next panel, for an application that does not dock.
     *
     * The position and the size apply on the first use alone, so a person can
     * move the window afterwards and it stays where they put it. An application
     * with docking calls nothing and lets the dockspace place the panel.
     *
     * @param x Left edge in pixels, from the top left of the viewport.
     * @param y Top edge in pixels.
     * @param width Width in pixels.
     * @param height Height in pixels.
     *
     * @warning This applies to the next panel alone, so it goes immediately
     * before the draw call it places.
     */
    void place_next_panel(float x, float y, float width, float height);

    /**
     * @brief Draws the view settings, with a save and a load button.
     *
     * Nothing here names a field of ViewSettings. The widgets come from the
     * descriptors, and the two buttons read and write the same file the
     * program reads at startup.
     *
     * @param settings The settings to edit, changed in place.
     * @param file Where the two buttons save and load.
     * @param open Cleared when the user closes the panel, which also draws the
     * close button. Pass null for a panel the user cannot close.
     * @return True when the user changed a field this frame.
     */
    bool draw_view_panel(ViewSettings& settings, const std::filesystem::path& file,
                         bool* open = nullptr);

    /**
     * @brief Draws the entity hierarchy, and lets the user pick one out of it.
     *
     * The tree reads the world the game loaded and names no game type, so a
     * different game shows the same tree. The save button writes the source
     * scene, and it is disabled when there is no source tree to write to.
     *
     * @param world The world to show.
     * @param selected The selected entity, changed when the user clicks one.
     * Pass entt::null for nothing selected.
     * @param scene_path The source scene to save to, or empty for no save.
     * @param content The open cooked content, which turns an identity back into
     * the reference a person wrote.
     * @param open Cleared when the user closes the panel. Pass null for a panel
     * the user cannot close.
     * @param save_blocked Why the save is not allowed right now, or null when
     * it is. The text is shown beside the disabled button. The editor passes a
     * reason while a play session runs, because the world is then a game part
     * way through a step rather than the scene somebody authored.
     */
    void draw_world_panel(const scene::World& world, entt::entity& selected,
                          const std::filesystem::path& scene_path,
                          const assets::Content& content, bool* open = nullptr,
                          const char* save_blocked = nullptr);

    /**
     * @brief Draws every component the selected entity carries.
     *
     * Nothing here names a component type. The registry holds a function that
     * already knows the type, and it calls reflect::inspect() through it. Add a
     * described component to the registry and it appears here, game types
     * included.
     *
     * @param world The world holding the entity. An edit marks it dirty, so
     * this takes it by reference.
     * @param selected The entity to show, or entt::null for none.
     * @param open Cleared when the user closes the panel. Pass null for a panel
     * the user cannot close.
     */
    void draw_inspector_panel(scene::World& world, entt::entity selected, bool* open = nullptr);

    /**
     * @brief Draws the rendered scene inside a panel, under a play bar.
     *
     * The picture fills the panel, so the camera aspect follows what the user
     * dragged the edges to and nothing is stretched or letterboxed.
     *
     * The three buttons sit above the picture, because that is the thing a
     * person watches while a session runs. The panel changes no state: it
     * reports what was clicked and the caller acts on it, which is what keeps
     * the session out of a panel.
     *
     * The size it reports is what the panel wants, not what the picture is. A
     * target cannot be rebuilt while a frame is recording, so the caller
     * compares the two at the top of the next frame and rebuilds there. One
     * frame of a dragged edge therefore shows the old picture stretched, which
     * is the usual answer and is invisible at a normal frame rate.
     *
     * @param picture The image to draw, from gfx::imgui_texture_id().
     * kInvalidImGuiTexture draws a message instead, which is what a panel shows
     * before the first target is built.
     * @param size The size of @p picture, so it draws at its own resolution.
     * @param wanted Receives the size of the panel content area. Left untouched
     * when the panel is closed or collapsed, so a hidden panel never asks for a
     * target of zero.
     * @param state What the play bar draws. Edit offers a play button, and a
     * running session offers pause or resume and stop.
     * @param gizmo Which handles the bar offers, changed in place when the user
     * picks another. The panel draws no gizmo itself.
     * @param overlay Drawn over the picture, inside this window. A gizmo goes
     * here. The default draws nothing.
     * @param open Cleared when the user closes the panel. Pass null for a panel
     * the user cannot close.
     * @return What the user clicked, and whether this panel holds the focus.
     */
    ViewportReport draw_viewport_panel(gfx::ImGuiTextureId picture, gfx::Extent2D size,
                                       gfx::Extent2D& wanted, PlayState state,
                                       GizmoControls& gizmo,
                                       const ViewportOverlay& overlay = {},
                                       bool* open = nullptr);

    /**
     * @brief Writes the world out as a source scene.
     *
     * Every prefab instance collapses again here, so what a person changed
     * comes back as an override rather than as entities. Each asset identity
     * becomes the reference the source named, because an identity is derived
     * and nobody chose it.
     *
     * The write goes through a temporary in the same directory and then a
     * rename. A disk that fills up, or a close that fails, therefore leaves the
     * scene untouched rather than half written.
     *
     * @param path The source scene to write.
     * @param world The world to save.
     * @param content The open cooked content, for the references.
     * @return True when the whole file reached the disk under @p path.
     */
    [[nodiscard]] bool save_scene_source(const std::filesystem::path& path,
                                         const scene::World& world,
                                         const assets::Content& content);

} // namespace engine::editor
