// Every ImGui call the panels make lives here, so editor/panels.h names no
// ImGui type. reflect/inspector.cpp does the same thing for the same reason.

#include "editor/panels.h"

#include "assets/content.h"
#include "assets/manifest.h"
#include "assets/reference.h"
#include "core/guid.h"
#include "core/log.h"
#include "core/profile.h"
#include "reflect/inspector.h"
#include "reflect/json.h"
#include "render/material_cache.h"
#include "scene/component_registry.h"
#include "scene/components.h"
#include "scene/prefab.h"
#include "scene/references.h"
#include "scene/scene_file.h"
#include "scene/world.h"

#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>
#include <nlohmann/json.hpp>

#include <fstream>
#include <string>
#include <system_error>

namespace engine::editor {

    namespace {

        /// How many spaces one level of the written scene is indented by.
        constexpr int kSceneIndent = 2;

        /// The gap around the play bar, in pixels. The viewport window carries
        /// no padding of its own, because the picture reaches its edges.
        constexpr float kBarPad = 4.0F;

        /// The wider gap between the buttons and the state they report.
        constexpr float kBarGap = 12.0F;

        /// What the play bar says the session is doing.
        [[nodiscard]] const char* state_label(PlayState state) {
            switch (state) {
            case PlayState::Playing:
                return "playing";
            case PlayState::Paused:
                return "paused";
            case PlayState::Edit:
                break;
            }
            return "editing";
        }

        /**
         * Draws the three buttons above the picture.
         *
         * Every button is drawn in every state, disabled where it does not
         * apply, so the bar does not change width as a session starts and
         * stops. A button that moves under the pointer is a button somebody
         * clicks by mistake.
         *
         * @param state What the session is doing.
         * @return What the user clicked.
         */
        /**
         * Draws the gizmo mode buttons, and changes @p gizmo when one is
         * clicked.
         *
         * They sit on the play bar because that is the row a person's eye is
         * already on. There are no keyboard shortcuts for them: W, E and R are
         * the fly camera's, and a key that means two things depending on what
         * has the focus is a key nobody trusts. See issue #325.
         *
         * @param gizmo What the gizmo is set to.
         */
        void draw_gizmo_buttons(GizmoControls& gizmo) {
            const auto mode_button = [&gizmo](const char* label, GizmoOperation operation) {
                const bool active = gizmo.operation == operation;
                // A pressed-looking button for the one in use. ImGui has no
                // toggle, so the colour is the state.
                if (active) {
                    ImGui::PushStyleColor(ImGuiCol_Button,
                                          ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
                }
                if (ImGui::Button(label)) {
                    gizmo.operation = operation;
                }
                if (active) {
                    ImGui::PopStyleColor();
                }
                ImGui::SameLine(0.0F, kBarPad);
            };

            mode_button("Move", GizmoOperation::Translate);
            mode_button("Turn", GizmoOperation::Rotate);
            mode_button("Size", GizmoOperation::Scale);

            // One button that says which space it is in and swaps on a click,
            // because the two are exclusive and a person reads the state faster
            // than they read two buttons.
            const bool world = gizmo.space == GizmoSpace::World;
            if (ImGui::Button(world ? "World" : "Local")) {
                gizmo.space = world ? GizmoSpace::Local : GizmoSpace::World;
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", world ? "Handles line up with the world"
                                              : "Handles line up with the entity");
            }
        }

        [[nodiscard]] PlayRequest draw_play_bar(PlayState state, GizmoControls& gizmo) {
            PlayRequest request = PlayRequest::None;
            const bool editing = state == PlayState::Edit;

            ImGui::Dummy(ImVec2{ kBarPad, kBarPad });
            ImGui::SameLine(0.0F, 0.0F);

            ImGui::BeginDisabled(!editing);
            if (ImGui::Button("Play")) {
                request = PlayRequest::Play;
            }
            ImGui::EndDisabled();

            ImGui::SameLine(0.0F, kBarPad);
            ImGui::BeginDisabled(editing);
            if (state == PlayState::Paused) {
                if (ImGui::Button("Resume")) {
                    request = PlayRequest::Resume;
                }
            } else if (ImGui::Button("Pause")) {
                request = PlayRequest::Pause;
            }

            ImGui::SameLine(0.0F, kBarPad);
            if (ImGui::Button("Stop")) {
                request = PlayRequest::Stop;
            }
            ImGui::EndDisabled();

            ImGui::SameLine(0.0F, kBarGap);
            ImGui::TextUnformatted(state_label(state));

            ImGui::SameLine(0.0F, kBarGap);
            draw_gizmo_buttons(gizmo);

            ImGui::Dummy(ImVec2{ kBarPad, kBarPad });
            ImGui::Separator();
            return request;
        }

        /// What the label for one entity says in the tree.
        std::string entity_label(const entt::registry& entities, entt::entity entity) {
            const auto* named = entities.try_get<scene::Name>(entity);
            std::string label = named != nullptr ? named->value : std::string{ "unnamed" };
            if (entities.all_of<scene::PrefabInstance>(entity)) {
                label += "  [" + entities.get<scene::PrefabInstance>(entity).prefab + "]";
            }
            return label;
        }

        /// Draws one entity and everything under it, and reports a click.
        void draw_entity_node(const scene::World& world, entt::entity entity,
                              entt::entity& selected) {
            const entt::registry& entities = world.registry();
            const scene::Hierarchy& node = entities.get<scene::Hierarchy>(entity);

            ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow |
                                       ImGuiTreeNodeFlags_SpanAvailWidth |
                                       ImGuiTreeNodeFlags_DefaultOpen;
            if (node.child_count == 0) {
                flags |= ImGuiTreeNodeFlags_Leaf;
            }
            if (entity == selected) {
                flags |= ImGuiTreeNodeFlags_Selected;
            }

            // The entity value is the identity here. Two entities can share a
            // name, and ImGui needs the labels to differ.
            ImGui::PushID(static_cast<int>(entt::to_integral(entity)));
            const bool open = ImGui::TreeNodeEx("node", flags, "%s",
                                                entity_label(entities, entity).c_str());
            // A press and a release inside one frame make ImGui hold the click
            // for two frames, so guard on a real change rather than logging
            // twice.
            if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen() && selected != entity) {
                selected = entity;
                ENGINE_LOG_TRACE("Selected entity {}.", entt::to_integral(entity));
            }
            if (open) {
                for (entt::entity child = node.first_child; child != entt::null;
                     child = entities.get<scene::Hierarchy>(child).next_sibling) {
                    draw_entity_node(world, child, selected);
                }
                ImGui::TreePop();
            }
            ImGui::PopID();
        }

        /**
         * Draws the material block layout that mesh.frag declares.
         *
         * The renderer validates these against the shader at startup, and a
         * person editing a material needs to see the names and the types the
         * shader expects. The values come from the cooked material asset, and
         * editing them needs a material source that does not exist yet. Issue
         * #102 records that decision.
         */
        void draw_material_block_info() {
            if (!ImGui::CollapsingHeader("Material block", ImGuiTreeNodeFlags_DefaultOpen)) {
                return;
            }

            ImGui::TextDisabled("What mesh.frag expects. The values come from the cooked glTF.");
            ImGui::Separator();

            if (!ImGui::BeginTable("material_params", 3,
                                   ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
                return;
            }
            ImGui::TableSetupColumn("Name");
            ImGui::TableSetupColumn("Type");
            ImGui::TableSetupColumn("Offset");
            ImGui::TableHeadersRow();

            for (const render::MaterialUniformMember& member : render::material_uniform_layout()) {
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(member.name);
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(render::param_type_name(member.type));
                ImGui::TableNextColumn();
                ImGui::Text("%u", member.offset);
            }
            ImGui::EndTable();
        }

    } // namespace

    void place_next_panel(float x, float y, float width, float height) {
        ImGui::SetNextWindowPos(ImVec2{ x, y }, ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2{ width, height }, ImGuiCond_FirstUseEver);
    }

    bool draw_view_panel(ViewSettings& settings, const std::filesystem::path& file,
                         bool* open) {
        ENGINE_PROFILE_ZONE_N("draw_view_panel");

        bool changed = false;
        if (ImGui::Begin("View", open)) {
            changed = reflect::inspect(settings);

            ImGui::Separator();

            const std::string path = file.string();
            if (ImGui::Button("Save")) {
                if (reflect::save_json(file, settings)) {
                    ENGINE_LOG_INFO("Wrote {}.", path);
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Load")) {
                if (reflect::load_json(file, settings)) {
                    ENGINE_LOG_INFO("Read {}.", path);
                }
            }
            ImGui::SameLine();
            ImGui::TextDisabled("%s", path.c_str());
        }
        ImGui::End();
        return changed;
    }

    namespace {

        /// How many entities go when this one does, itself included.
        [[nodiscard]] std::size_t count_subtree(const scene::World& world, entt::entity entity) {
            const entt::registry& entities = world.registry();
            std::size_t total = 1;
            const auto* node = entities.try_get<scene::Hierarchy>(entity);
            for (entt::entity child = node != nullptr ? node->first_child : entt::null;
                 child != entt::null;) {
                total += count_subtree(world, child);
                child = entities.get<scene::Hierarchy>(child).next_sibling;
            }
            return total;
        }

        /**
         * Draws the delete button and the question it asks first.
         *
         * **Deleting takes the descendants.** A crate holds its lid, and a
         * person who deletes the crate means the lid too, but nobody expects to
         * find out afterwards. There is no undo yet, which is issue #331, so the
         * count goes in the question rather than in a log line after the fact.
         *
         * @param world The world to delete from.
         * @param selected The selection, cleared when the entity goes.
         */
        void draw_delete_button(scene::World& world, entt::entity& selected) {
            const bool have = selected != entt::null && world.registry().valid(selected);

            ImGui::BeginDisabled(!have);
            if (ImGui::Button("Delete") && have) {
                ImGui::OpenPopup("delete_entity");
            }
            ImGui::EndDisabled();

            if (!ImGui::BeginPopup("delete_entity")) {
                return;
            }
            if (!have) {
                // The selection went between the click and the popup opening.
                ImGui::CloseCurrentPopup();
                ImGui::EndPopup();
                return;
            }

            const std::size_t going = count_subtree(world, selected);
            const std::string label = entity_label(world.registry(), selected);
            if (going > 1) {
                ImGui::Text("Delete %s and the %zu entities under it?", label.c_str(), going - 1);
            } else {
                ImGui::Text("Delete %s?", label.c_str());
            }
            ImGui::TextDisabled("This cannot be undone.");
            ImGui::Separator();

            if (ImGui::Button("Delete")) {
                world.destroy(selected);
                // Nothing may hold an entity that no longer exists, and EnTT
                // hands the same number out again.
                selected = entt::null;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Keep")) {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

    } // namespace

    bool draw_world_panel(scene::World& world, entt::entity& selected,
                          const std::filesystem::path& scene_path, const assets::Content& content,
                          bool* open, const char* save_blocked) {
        ENGINE_PROFILE_ZONE_N("draw_world_panel");

        bool saved = false;

        if (ImGui::Begin("World", open)) {
            ImGui::Text("Entities: %zu", world.size());
            ImGui::Text("Matrices rebuilt last frame: %zu", world.rebuilt_last_update());

            // Without a source tree there is nowhere to save that a person
            // would find again. Writing into the cooked tree looks like it
            // worked and the next cook throws it away.
            const bool can_save = !scene_path.empty() && save_blocked == nullptr;
            ImGui::BeginDisabled(!can_save);
            if (ImGui::Button("Save scene") && can_save) {
                if (save_scene_source(scene_path, world, content)) {
                    saved = true;
                } else {
                    ENGINE_LOG_ERROR("The scene did not write to {}.", scene_path.string());
                }
            }
            ImGui::EndDisabled();

            // Beside the save and before the path, because the path is as long
            // as a path is and anything after it lands off the panel.
            ImGui::SameLine();
            draw_delete_button(world, selected);

            ImGui::SameLine();
            // The whole path, because which of the two trees this writes to is
            // the thing worth knowing.
            const char* why = save_blocked != nullptr
                                  ? save_blocked
                                  : "no source tree, so there is nowhere to save";
            ImGui::TextDisabled("%s", can_save ? scene_path.string().c_str() : why);
            if (can_save && ImGui::IsItemHovered()) {
                ImGui::SetTooltip("The source scene. The cooker turns it into what runs.");
            }

            ImGui::Separator();

            const entt::registry& entities = world.registry();
            for (const auto [entity, node] : entities.view<const scene::Hierarchy>().each()) {
                if (node.parent == entt::null) {
                    draw_entity_node(world, entity, selected);
                }
            }
        }
        ImGui::End();
        return saved;
    }

    namespace {

        /**
         * Draws the button that takes a component off, on the header row.
         *
         * A Transform has no button. Every entity carries one and the hierarchy
         * reads it, so removing one is not an editing mistake to be undone but a
         * world that no longer works. `owns_transform` is how a caller tells,
         * without comparing a name against a spelling.
         *
         * @param ops The component the header belongs to.
         * @return True when the user asked for it to go.
         */
        [[nodiscard]] bool draw_remove_button(const scene::ComponentOps& ops) {
            if (ops.owns_transform || ops.remove == nullptr) {
                return false;
            }

            // On the same line as the header it belongs to, at the right edge.
            const float button = ImGui::GetFrameHeight();
            ImGui::SameLine(ImGui::GetWindowWidth() - button - ImGui::GetStyle().WindowPadding.x);
            const bool clicked = ImGui::SmallButton("x");
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Take %s off this entity", ops.name);
            }
            return clicked;
        }

        /**
         * Draws the add button, listing what the entity does not carry.
         *
         * The list is the registry, so a component added to the engine or to the
         * game appears here with no editor change at all. That is rule 4.5 doing
         * its job: one description, and every consumer reads it.
         *
         * @param world The world holding the entity.
         * @param selected The entity to add to.
         */
        void draw_add_component(scene::World& world, entt::entity selected) {
            ImGui::Separator();
            if (ImGui::Button("Add component")) {
                ImGui::OpenPopup("add_component");
            }

            if (!ImGui::BeginPopup("add_component")) {
                return;
            }

            std::size_t offered = 0;
            for (const scene::ComponentOps& ops : scene::components().all()) {
                if (ops.create == nullptr || ops.has(world.registry(), selected)) {
                    continue;
                }
                ++offered;
                if (ImGui::MenuItem(ops.name)) {
                    ops.create(world.registry(), selected);
                }
            }
            if (offered == 0) {
                ImGui::TextDisabled("This entity carries every component there is.");
            }
            ImGui::EndPopup();
        }

    } // namespace

    namespace {

        /**
         * Draws the fields of one component, and records an edit when it ends.
         *
         * **The value is kept before the fields are drawn**, because a slider
         * jumps to where it was clicked on the frame it takes the focus. A
         * value read after that draw is already the edited one, and the undo
         * would then go back to part way through the edit rather than to the
         * start of it.
         *
         * A panel with no history draws exactly as it did before undo existed.
         *
         * @param world The world holding the entity.
         * @param selected The entity being shown.
         * @param ops The component to draw.
         * @param history Where an edit is recorded, or null for no undo.
         * @param pending The edit in progress, or null for no undo.
         * @return True when the user changed a field this frame.
         */
        bool draw_component_fields(scene::World& world, entt::entity selected,
                                   const scene::ComponentOps& ops, History* history,
                                   Interaction* pending) {
            reflect::widget::begin_edit_tracking();

            const bool moved = ops.inspect(world.registry(), selected);

            if (history == nullptr || pending == nullptr) {
                return moved;
            }
            if (reflect::widget::edit_began() && !pending->active()) {
                (void)pending->begin(world, selected, ops.name);
            }
            if (reflect::widget::edit_ended()) {
                (void)pending->end(world, *history);
            }
            return moved;
        }

    } // namespace

    void draw_inspector_panel(scene::World& world, entt::entity selected, bool* open,
                              History* history, Interaction* pending) {
        ENGINE_PROFILE_ZONE_N("draw_inspector_panel");

        if (ImGui::Begin("Inspector", open)) {
            if (selected == entt::null || !world.registry().valid(selected)) {
                ImGui::TextDisabled("Pick an entity in the World window.");
                ImGui::End();
                return;
            }

            ImGui::Text("%s", entity_label(world.registry(), selected).c_str());
            ImGui::Separator();

            bool moved = false;
            const scene::ComponentOps* remove_this = nullptr;

            for (const scene::ComponentOps& ops : scene::components().all()) {
                if (!ops.has(world.registry(), selected)) {
                    continue;
                }
                ImGui::PushID(ops.name);
                const bool open_header =
                    ImGui::CollapsingHeader(ops.name, ImGuiTreeNodeFlags_DefaultOpen);
                if (draw_remove_button(ops)) {
                    // Held rather than removed here. Taking a component off
                    // while this loop walks the registry storage is what
                    // invalidates the iteration.
                    remove_this = &ops;
                }
                if (open_header) {
                    moved = draw_component_fields(world, selected, ops, history, pending) ||
                            moved;
                }
                ImGui::PopID();
            }

            if (remove_this != nullptr) {
                remove_this->remove(world.registry(), selected);
                moved = true;
            }

            draw_add_component(world, selected);

            const auto* renderer = world.registry().try_get<scene::MeshRenderer>(selected);
            if (renderer != nullptr && renderer->mesh.valid()) {
                draw_material_block_info();
            }

            if (moved) {
                // The edit went through the registry, so it went around
                // set_local(). World says to mark the subtree, or the matrices
                // stay stale and only a later move would put them right.
                world.mark_dirty(selected);
            }
        }
        ImGui::End();
    }

    ViewportReport draw_viewport_panel(gfx::ImGuiTextureId picture, gfx::Extent2D size,
                                       gfx::Extent2D& wanted, PlayState state,
                                       GizmoControls& gizmo, const ViewportOverlay& overlay,
                                       bool* open) {
        ENGINE_PROFILE_ZONE_N("draw_viewport_panel");

        ViewportReport report;

        // No padding, so the picture reaches the edges of the panel. Without
        // this the content area is smaller than the panel by the style padding,
        // and the target follows the content area, so the scene would render at
        // a size that never quite matches what a person dragged.
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{ 0.0F, 0.0F });
        const bool visible = ImGui::Begin("Viewport", open);
        ImGui::PopStyleVar();

        if (visible) {
            // The whole panel, so a click on the picture counts as well as one
            // on the bar. A child window inside it would otherwise hold the
            // focus and the game would never see a key.
            report.focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows);
            // Hovered rather than focused for the mouse, because turning the
            // view is what a person does first, before they have clicked
            // anything. ImGui reports false here when another window covers
            // this one, so a popup over the viewport still keeps the pointer.
            report.hovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows);
            report.request = draw_play_bar(state, gizmo);

            // After the bar, so the target follows the picture area rather
            // than the whole panel. Sizing it to the panel would render a
            // scene taller than the space left to show it.
            const ImVec2 area = ImGui::GetContentRegionAvail();
            // A collapsed or fully shrunk panel asks for nothing. Reporting a
            // zero here would have the caller build a target of no size, which
            // the device refuses, and then try again every frame.
            if (area.x >= 1.0F && area.y >= 1.0F) {
                wanted = gfx::Extent2D{ static_cast<std::uint32_t>(area.x),
                                        static_cast<std::uint32_t>(area.y) };
            }

            if (picture == gfx::kInvalidImGuiTexture) {
                ImGui::TextDisabled("There is no picture yet.");
            } else {
                // At the size of the image rather than the size of the panel.
                // They differ for the one frame after a drag, and drawing at
                // the panel size would stretch it by the same amount either
                // way. This way the mismatch shows as a gap at the edge, which
                // is honest about what is happening.
                // Where the picture landed, before anything else moves the
                // cursor. The overlay is drawn over these pixels.
                const ImVec2 corner = ImGui::GetCursorScreenPos();
                ImGui::Image(picture, ImVec2{ static_cast<float>(size.width),
                                              static_cast<float>(size.height) });

                if (overlay.draw != nullptr) {
                    overlay.draw(overlay.user, corner.x, corner.y, static_cast<float>(size.width),
                                 static_cast<float>(size.height));
                }
            }
        }
        ImGui::End();
        return report;
    }

    namespace {

        /**
         * Draws one cooked output as a row somebody can drag.
         *
         * The label is the reference a person would write, which
         * `assets::reference_for` works out from the manifest. An output nothing
         * names falls back to its cooked path, so a row is never blank.
         *
         * @param manifest The manifest the output came from.
         * @param output The cooked file to draw.
         */
        void draw_asset_row(const assets::Manifest& manifest,
                            const assets::ManifestOutput& output) {
            const std::string name = assets::reference_for(manifest, output.guid);
            const std::string label = name.empty() ? output.cooked : name;

            ImGui::PushID(output.cooked.c_str());
            ImGui::Selectable(label.c_str());
            if (ImGui::BeginDragDropSource()) {
                // The identity as text, which is what an AssetRef field stores,
                // so nothing is looked up on the way over.
                const std::string identity = to_text(output.guid);
                ImGui::SetDragDropPayload(reflect::kAssetPayload, identity.c_str(),
                                          identity.size());
                ImGui::TextUnformatted(label.c_str());
                ImGui::EndDragDropSource();
            }
            ImGui::PopID();
        }

    } // namespace

    void draw_assets_panel(const assets::Content& content, std::string& filter, bool* open) {
        ENGINE_PROFILE_ZONE_N("draw_assets_panel");

        if (ImGui::Begin("Assets", open)) {
            ImGui::TextUnformatted("Drag a row onto an asset field.");
            ImGui::InputText("filter", &filter);

            const assets::Manifest& manifest = content.manifest();
            std::size_t shown = 0;

            for (const assets::ManifestEntry& entry : manifest.entries) {
                if (!filter.empty() && entry.source.find(filter) == std::string::npos) {
                    continue;
                }
                ++shown;

                // The source file is the node, and what it cooked into are the
                // leaves. One glTF is a mesh, a material, and a prefab, and a
                // field names one of those rather than the file.
                if (!ImGui::TreeNode(entry.source.c_str())) {
                    continue;
                }
                for (const assets::ManifestOutput& output : entry.outputs) {
                    draw_asset_row(manifest, output);
                }
                ImGui::TreePop();
            }

            if (shown == 0) {
                ImGui::TextDisabled("%s", manifest.entries.empty()
                                              ? "No cooked content. Build the cooker target."
                                              : "Nothing matches the filter.");
            }
        }
        ImGui::End();
    }

    bool save_scene_source(const std::filesystem::path& path, const scene::World& world,
                           const assets::Content& content) {
        nlohmann::json document = scene::save_scene(world);
        const std::size_t restored = scene::restore_references(document, content.manifest());

        // Through a temporary in the same directory, then a rename. Writing
        // over the scene directly means a disk that fills up, or a close that
        // fails, leaves a person with half a scene and no copy of the whole
        // one. The rename is what makes the swap all or nothing, and it is
        // only reached once the bytes are down.
        std::filesystem::path staged = path;
        staged += ".writing";

        {
            std::ofstream file(staged, std::ios::binary | std::ios::trunc);
            if (!file) {
                ENGINE_LOG_ERROR("Could not open {} for writing.", staged.string());
                return false;
            }
            file << document.dump(kSceneIndent) << '\n';
            file.close();
            if (!file) {
                ENGINE_LOG_ERROR("Could not write {}, so {} is untouched.", staged.string(),
                                 path.string());
                std::error_code ignored;
                std::filesystem::remove(staged, ignored);
                return false;
            }
        }

        std::error_code error;
        std::filesystem::rename(staged, path, error);
        if (error) {
            ENGINE_LOG_ERROR("Could not put {} in place of {}. {}", staged.string(), path.string(),
                             error.message());
            std::error_code ignored;
            std::filesystem::remove(staged, ignored);
            return false;
        }

        ENGINE_LOG_INFO("Wrote {}, with {} asset references put back.", path.string(), restored);
        return true;
    }

} // namespace engine::editor
