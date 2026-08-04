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

        /// Below this a basis column has no direction to normalize.
        constexpr float kTooSmall = 1.0e-8F;

        /**
         * Pulls a translate, rotate, scale triple out of a matrix.
         *
         * The scale of an axis is the length of its basis column, which is
         * always positive. A matrix that mirrors has a negative determinant,
         * and length alone cannot see that: the mirror would come back as a
         * positive scale and a rotation that turns the geometry inside out.
         * DESIGN.md section 3 exists because that failure is expensive to find.
         *
         * So the determinant decides the sign, and it goes on the X axis. Any
         * one axis would do, because a mirror on one axis and a mirror on
         * another differ by a rotation, and the rotation comes out of the same
         * decomposition.
         */
        [[nodiscard]] engine::Transform decompose(const engine::Mat4& matrix) {
            engine::Transform out;
            out.position = engine::Vec3{ matrix[3] };

            engine::Mat3 basis{ matrix };
            engine::Vec3 scale{ glm::length(basis[0]), glm::length(basis[1]),
                                glm::length(basis[2]) };

            if (glm::determinant(basis) < 0.0F) {
                scale.x = -scale.x;
                basis[0] = -basis[0];
            }
            out.scale = scale;

            // A zero column has no direction, so normalizing it would divide by
            // zero and fill the rotation with NaN. Such an axis keeps the
            // identity direction instead.
            for (int axis = 0; axis < 3; ++axis) {
                const float length = std::abs(scale[axis]);
                basis[axis] =
                    length > kTooSmall ? basis[axis] / length : engine::Vec3{ 0.0F };
                if (length <= kTooSmall) {
                    basis[axis][axis] = 1.0F;
                }
            }
            out.rotation = glm::quat_cast(basis);
            return out;
        }

        /**
         * Whether a transform composes back into the matrix it came from.
         *
         * A matrix holding shear has no translate, rotate, scale form, and the
         * decomposition above gives an answer for one anyway. That answer is
         * wrong in a way nothing downstream can see, so it is checked here
         * rather than shipped.
         *
         * The tolerance scales with the matrix, because a model in millimeters
         * carries values a thousand times larger than one in meters and the
         * error grows with them.
         */
        [[nodiscard]] bool matches(const engine::Transform& transform,
                                   const engine::Mat4& matrix) {
            const engine::Mat4 rebuilt = engine::to_matrix(transform);

            float largest = 1.0F;
            for (int column = 0; column < 4; ++column) {
                for (int row = 0; row < 4; ++row) {
                    largest = std::max(largest, std::abs(matrix[column][row]));
                }
            }

            constexpr float kRelative = 1.0e-4F;
            const float allowed = largest * kRelative;
            for (int column = 0; column < 4; ++column) {
                for (int row = 0; row < 4; ++row) {
                    if (std::abs(rebuilt[column][row] - matrix[column][row]) > allowed) {
                        return false;
                    }
                }
            }
            return true;
        }

        /**
         * Reads the local transform of one node.
         *
         * glTF gives either a TRS triple or a matrix, and never both. The TRS
         * form copies across. The matrix form has to come apart.
         *
         * @param node The node to read.
         * @param where A name for the log.
         * @param out The transform.
         * @return False when the matrix holds something Transform cannot say,
         * which today means shear.
         */
        [[nodiscard]] bool node_transform(const cgltf_node& node, std::string_view where,
                                          engine::Transform& out) {
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
                return true;
            }

            // Column major, which is what glTF stores and what GLM expects, so
            // the sixteen floats go across without a transpose.
            const engine::Mat4 matrix = glm::make_mat4(static_cast<const float*>(node.matrix));

            out = decompose(matrix);
            if (!matches(out, matrix)) {
                ENGINE_LOG_ERROR("{}: its matrix holds shear, and a Transform has no field "
                                 "for it. Bake the shear into the geometry, or give the node "
                                 "a translation, rotation, and scale instead.",
                                 where);
                return false;
            }
            return true;
        }

        /// The component set for one node, as a scene file writes it.
        [[nodiscard]] bool node_components(const cgltf_data& data, const cgltf_node& node,
                                           const std::vector<engine::Guid>& meshes,
                                           std::string_view where, nlohmann::json& out) {
            nlohmann::json components = nlohmann::json::object();

            engine::scene::Name name;
            name.value = node.name != nullptr && node.name[0] != '\0' ? node.name : kUnnamed;
            components["Name"] = engine::reflect::to_json(name);

            engine::Transform transform;
            if (!node_transform(node, where, transform)) {
                return false;
            }
            components["Transform"] = engine::reflect::to_json(transform);

            // A node with no mesh is a joint or a group, and it draws nothing.
            if (node.mesh != nullptr && data.meshes != nullptr) {
                const auto index = static_cast<std::size_t>(node.mesh - data.meshes);
                if (index < meshes.size()) {
                    engine::scene::MeshRenderer renderer;
                    renderer.mesh = meshes[index];
                    components["MeshRenderer"] = engine::reflect::to_json(renderer);
                }
            }
            out = std::move(components);
            return true;
        }

        /**
         * Walks one node and its children, appending an entity for each.
         *
         * A prefab needs every parent to come before its children, so this
         * appends the node and then recurses. cgltf_validate already rejects a
         * cycle, so the walk ends.
         */
        [[nodiscard]] bool add_node(const cgltf_data& data, const cgltf_node& node, int parent,
                                    const std::vector<engine::Guid>& meshes,
                                    std::string_view source, nlohmann::json& entities) {
            const auto self = static_cast<int>(entities.size());
            const std::string where =
                std::string{ source } + " node " +
                (node.name != nullptr && node.name[0] != '\0' ? node.name
                                                              : std::to_string(self));

            nlohmann::json entity = nlohmann::json::object();
            entity["parent"] = parent;
            nlohmann::json components;
            if (!node_components(data, node, meshes, where, components)) {
                return false;
            }
            entity["components"] = std::move(components);
            entities.push_back(std::move(entity));

            for (cgltf_size at = 0; at < node.children_count; ++at) {
                if (node.children[at] != nullptr &&
                    !add_node(data, *node.children[at], self, meshes, source, entities)) {
                    return false;
                }
            }
            return true;
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

            // A scene with no node has nothing to instance. Writing a
            // prefab holding only an added root would give a scene an entity
            // that does nothing.
            if (scene.nodes_count == 0) {
                ENGINE_LOG_WARN("{}: scene {} holds no node, so it writes no prefab.",
                                source.string(), at);
                continue;
            }

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
                if (scene.nodes[top] != nullptr &&
                    !add_node(data, *scene.nodes[top], needs_root ? 0 : -1, meshes,
                              source.string(), entities)) {
                    return false;
                }
            }

            nlohmann::json document = nlohmann::json::object();
            document["__version"] = engine::scene::kPrefabVersion;
            document["entities"] = std::move(entities);

            std::filesystem::path cooked = relative;
            cooked += "." + std::to_string(at);
            cooked += as::kPrefabExtension;

            if (!write_prefab(out_root / cooked, document)) {
                return false;
            }

            outputs.push_back(as::ManifestOutput{
                .cooked = as::manifest_path(cooked),
                .guid = engine::Guid::derive(parent, as::kPrefabPartKind,
                                             static_cast<std::uint32_t>(at)) });
        }
        return true;
    }

} // namespace cooker
