#include "prefab.h"

#include "core/log.h"
#include "math/transform.h"
#include "reflect/json.h"
#include "scene/components.h"
#include "scene/prefab.h"

#include <cgltf.h>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <cmath>
#include <fstream>
#include <string>

namespace cooker {

    namespace {

        namespace as = engine::assets;

        /// The name to show for a node that the exporter left unnamed.
        constexpr const char* kUnnamed = "node";

        /**
         * Reads the local transform of one node.
         *
         * glTF gives either a TRS triple or a matrix, and never both. The TRS
         * form copies across. The matrix form has to come apart, and this reads
         * it as translate, rotate, scale, which is what Transform composes and
         * what DESIGN.md section 3 fixes.
         *
         * A matrix holding shear cannot be written as a Transform. Such a node
         * is rare, because an exporter writes shear only when an artist scaled
         * a rotated parent unevenly, and the result here drops the shear rather
         * than failing the model.
         */
        [[nodiscard]] engine::Transform node_transform(const cgltf_node& node) {
            engine::Transform out;

            if (node.has_matrix == 0) {
                if (node.has_translation != 0) {
                    out.position = engine::Vec3{ node.translation[0], node.translation[1],
                                                 node.translation[2] };
                }
                if (node.has_rotation != 0) {
                    // glTF stores xyzw and the engine stores wxyz. Getting this
                    // backwards gives a rotation that looks almost right.
                    out.rotation = engine::Quat{ node.rotation[3], node.rotation[0],
                                                 node.rotation[1], node.rotation[2] };
                }
                if (node.has_scale != 0) {
                    out.scale = engine::Vec3{ node.scale[0], node.scale[1], node.scale[2] };
                }
                return out;
            }

            // Column major, which is what glTF stores and what GLM expects, so
            // the sixteen floats go across without a transpose.
            const engine::Mat4 matrix = glm::make_mat4(static_cast<const float*>(node.matrix));

            out.position = engine::Vec3{ matrix[3] };

            engine::Mat3 basis{ matrix };
            const engine::Vec3 scale{ glm::length(basis[0]), glm::length(basis[1]),
                                      glm::length(basis[2]) };
            out.scale = scale;

            // A zero column has no direction, so normalizing it would divide by
            // zero and fill the rotation with NaN. Such an axis keeps the
            // identity direction instead.
            constexpr float kTooSmall = 1.0e-8F;
            for (int axis = 0; axis < 3; ++axis) {
                basis[axis] = scale[axis] > kTooSmall ? basis[axis] / scale[axis]
                                                      : engine::Vec3{ 0.0F };
                if (scale[axis] <= kTooSmall) {
                    basis[axis][axis] = 1.0F;
                }
            }
            out.rotation = glm::quat_cast(basis);
            return out;
        }

        /// The component set for one node, as a scene file writes it.
        [[nodiscard]] nlohmann::json node_components(const cgltf_data& data,
                                                     const cgltf_node& node,
                                                     const std::vector<engine::Guid>& meshes) {
            nlohmann::json components = nlohmann::json::object();

            engine::scene::Name name;
            name.value = node.name != nullptr && node.name[0] != '\0' ? node.name : kUnnamed;
            components["Name"] = engine::reflect::to_json(name);

            components["Transform"] = engine::reflect::to_json(node_transform(node));

            // A node with no mesh is a joint or a group, and it draws nothing.
            if (node.mesh != nullptr && data.meshes != nullptr) {
                const auto index = static_cast<std::size_t>(node.mesh - data.meshes);
                if (index < meshes.size()) {
                    engine::scene::MeshRenderer renderer;
                    renderer.mesh = meshes[index];
                    components["MeshRenderer"] = engine::reflect::to_json(renderer);
                }
            }
            return components;
        }

        /**
         * Walks one node and its children, appending an entity for each.
         *
         * A prefab needs every parent to come before its children, so this
         * appends the node and then recurses. cgltf_validate already rejects a
         * cycle, so the walk ends.
         */
        void add_node(const cgltf_data& data, const cgltf_node& node, int parent,
                      const std::vector<engine::Guid>& meshes, nlohmann::json& entities) {
            const auto self = static_cast<int>(entities.size());

            nlohmann::json entity = nlohmann::json::object();
            entity["parent"] = parent;
            entity["components"] = node_components(data, node, meshes);
            entities.push_back(std::move(entity));

            for (cgltf_size at = 0; at < node.children_count; ++at) {
                if (node.children[at] != nullptr) {
                    add_node(data, *node.children[at], self, meshes, entities);
                }
            }
        }

        [[nodiscard]] bool write_prefab(const std::filesystem::path& destination,
                                        const nlohmann::json& document) {
            std::error_code error;
            std::filesystem::create_directories(destination.parent_path(), error);

            std::ofstream file(destination, std::ios::trunc);
            if (!file) {
                ENGINE_LOG_ERROR("{}: could not open it for writing.", destination.string());
                return false;
            }
            constexpr int kIndent = 2;
            file << document.dump(kIndent) << '\n';
            if (!file) {
                ENGINE_LOG_ERROR("{}: the write failed part way through.", destination.string());
                return false;
            }
            return true;
        }

    } // namespace

    bool cook_prefabs(const cgltf_data& data, const std::filesystem::path& source,
                      const std::filesystem::path& out_root,
                      const std::filesystem::path& relative, engine::Guid parent,
                      const std::vector<engine::Guid>& meshes,
                      std::vector<as::ManifestOutput>& outputs) {
        for (cgltf_size at = 0; at < data.scenes_count; ++at) {
            const cgltf_scene& scene = data.scenes[at];

            nlohmann::json entities = nlohmann::json::array();

            // A prefab holds exactly one root. A glTF scene may list several,
            // and the Flight Helmet lists six, so this adds one when it has to.
            // The added root sits at the identity, so it moves nothing, and it
            // gives an instance a single entity to place.
            const bool needs_root = scene.nodes_count != 1;
            if (needs_root) {
                engine::scene::Name name;
                name.value = scene.name != nullptr && scene.name[0] != '\0'
                                 ? scene.name
                                 : source.stem().string();

                nlohmann::json root = nlohmann::json::object();
                root["parent"] = -1;
                root["components"] = nlohmann::json::object();
                root["components"]["Name"] = engine::reflect::to_json(name);
                root["components"]["Transform"] =
                    engine::reflect::to_json(engine::Transform{});
                entities.push_back(std::move(root));
            }

            for (cgltf_size top = 0; top < scene.nodes_count; ++top) {
                if (scene.nodes[top] != nullptr) {
                    add_node(data, *scene.nodes[top], needs_root ? 0 : -1, meshes, entities);
                }
            }

            if (entities.empty()) {
                ENGINE_LOG_WARN("{}: scene {} holds no node, so it writes no prefab.",
                                source.string(), at);
                continue;
            }

            nlohmann::json document = nlohmann::json::object();
            document["__version"] = engine::scene::kPrefabVersion;
            document["entities"] = std::move(entities);

            std::filesystem::path cooked = relative;
            cooked += "." + std::to_string(at);
            cooked += kPrefabExtension;

            if (!write_prefab(out_root / cooked, document)) {
                return false;
            }

            outputs.push_back(as::ManifestOutput{
                .cooked = as::manifest_path(cooked),
                .guid = engine::Guid::derive(parent, kPrefabPartKind,
                                             static_cast<std::uint32_t>(at)) });
        }
        return true;
    }

} // namespace cooker
