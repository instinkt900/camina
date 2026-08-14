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
#include "editor/view_settings.h"

#include <entt/entity/fwd.hpp>

#include <filesystem>

namespace engine::assets {
    class Content;
}

namespace engine::scene {
    class World;
}

namespace engine::editor {

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
     * @return True when the user changed a field this frame.
     */
    bool draw_view_panel(ViewSettings& settings, const std::filesystem::path& file);

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
     */
    void draw_world_panel(const scene::World& world, entt::entity& selected,
                          const std::filesystem::path& scene_path,
                          const assets::Content& content);

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
     */
    void draw_inspector_panel(scene::World& world, entt::entity selected);

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
