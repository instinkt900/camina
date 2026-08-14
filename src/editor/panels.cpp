// Every ImGui call the panels make lives here, so editor/panels.h names no
// ImGui type. reflect/inspector.cpp does the same thing for the same reason.

#include "editor/panels.h"

#include "assets/content.h"
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
#include <nlohmann/json.hpp>

#include <fstream>
#include <string>
#include <system_error>

namespace engine::editor {

    namespace {

        /// How many spaces one level of the written scene is indented by.
        constexpr int kSceneIndent = 2;

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

    void draw_world_panel(const scene::World& world, entt::entity& selected,
                          const std::filesystem::path& scene_path, const assets::Content& content,
                          bool* open) {
        ENGINE_PROFILE_ZONE_N("draw_world_panel");

        if (ImGui::Begin("World", open)) {
            ImGui::Text("Entities: %zu", world.size());
            ImGui::Text("Matrices rebuilt last frame: %zu", world.rebuilt_last_update());

            // Without a source tree there is nowhere to save that a person
            // would find again. Writing into the cooked tree looks like it
            // worked and the next cook throws it away.
            const bool can_save = !scene_path.empty();
            ImGui::BeginDisabled(!can_save);
            if (ImGui::Button("Save scene") && can_save) {
                if (!save_scene_source(scene_path, world, content)) {
                    ENGINE_LOG_ERROR("The scene did not write to {}.", scene_path.string());
                }
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            // The whole path, because which of the two trees this writes to is
            // the thing worth knowing.
            ImGui::TextDisabled("%s", can_save ? scene_path.string().c_str()
                                               : "no source tree, so there is nowhere to save");
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
    }

    void draw_inspector_panel(scene::World& world, entt::entity selected, bool* open) {
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
            for (const scene::ComponentOps& ops : scene::components().all()) {
                if (!ops.has(world.registry(), selected)) {
                    continue;
                }
                ImGui::PushID(ops.name);
                if (ImGui::CollapsingHeader(ops.name, ImGuiTreeNodeFlags_DefaultOpen)) {
                    moved = ops.inspect(world.registry(), selected) || moved;
                }
                ImGui::PopID();
            }

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
