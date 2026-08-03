#include "mesh.h"

#include "assets/mesh.h"
#include "core/log.h"
#include "material.h"

#include <cgltf.h>
#include <meshoptimizer.h>

#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <string_view>

namespace cooker {

    namespace {

        namespace as = engine::assets;
        using engine::Vec2;
        using engine::Vec3;

        constexpr std::size_t kVerticesPerTriangle = 3;

        /// Frees the cgltf data whatever path the function leaves by.
        struct GltfData {
            cgltf_data* data = nullptr;

            ~GltfData() {
                if (data != nullptr) {
                    cgltf_free(data);
                }
            }

            GltfData() = default;
            GltfData(const GltfData&) = delete;
            GltfData& operator=(const GltfData&) = delete;
            GltfData(GltfData&&) = delete;
            GltfData& operator=(GltfData&&) = delete;
        };

        [[nodiscard]] std::string lowered(std::string_view text) {
            std::string out;
            out.reserve(text.size());
            for (const char c : text) {
                out.push_back(c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c);
            }
            return out;
        }

        /// Names a cgltf failure, so the log says more than "it did not load".
        [[nodiscard]] const char* gltf_error(cgltf_result result) {
            switch (result) {
            case cgltf_result_data_too_short:
                return "the file ends part way through";
            case cgltf_result_unknown_format:
                return "the format is not glTF";
            case cgltf_result_invalid_json:
                return "the JSON will not parse";
            case cgltf_result_invalid_gltf:
                return "the glTF is not valid";
            case cgltf_result_out_of_memory:
                return "it ran out of memory";
            case cgltf_result_legacy_gltf:
                return "it is glTF 1.0, and this reads glTF 2.0";
            case cgltf_result_file_not_found:
                return "a file it names is missing, perhaps the .bin next to it";
            case cgltf_result_io_error:
                return "it could not be read";
            default:
                return "it did not load";
            }
        }

        /// One attribute of one primitive, read out as floats.
        [[nodiscard]] bool read_attribute(const cgltf_accessor* accessor, std::size_t components,
                                          std::vector<float>& out, std::string_view where) {
            if (accessor == nullptr) {
                return false;
            }
            if (cgltf_num_components(accessor->type) != components) {
                ENGINE_LOG_ERROR("{}: an attribute holds {} components and this needs {}.",
                                 where, cgltf_num_components(accessor->type), components);
                return false;
            }
            out.resize(accessor->count * components);
            const cgltf_size read = cgltf_accessor_unpack_floats(accessor, out.data(),
                                                                 out.size());
            if (read != out.size()) {
                ENGINE_LOG_ERROR("{}: an attribute gave {} floats and this wanted {}.", where,
                                 read, out.size());
                return false;
            }
            return true;
        }

        [[nodiscard]] Vec3 normalized(Vec3 value) {
            const float length = std::sqrt(glm::dot(value, value));
            constexpr float kTooSmall = 1e-8F;
            return length > kTooSmall ? value / length : Vec3{ 0.0F, 0.0F, 1.0F };
        }

        /**
         * Builds a tangent for every vertex, for a mesh whose source has none.
         *
         * A glTF exporter leaves TANGENT out whenever the material has no normal
         * map at export time. Adding one later then has nothing to work with, so
         * the cooker fills the gap.
         *
         * This accumulates the per-triangle tangent over every triangle a vertex
         * belongs to, then makes it perpendicular to the normal. It is not
         * MikkTSpace, so it does not match what a baking tool assumed when it
         * made a normal map. For a mesh whose exporter supplied tangents, this
         * never runs.
         */
        void build_tangents(std::vector<as::MeshVertex>& vertices,
                            const std::vector<std::uint32_t>& indices) {
            std::vector<Vec3> along(vertices.size(), Vec3{ 0.0F });
            std::vector<Vec3> across(vertices.size(), Vec3{ 0.0F });

            for (std::size_t at = 0; at + 2 < indices.size(); at += kVerticesPerTriangle) {
                const std::uint32_t i0 = indices[at];
                const std::uint32_t i1 = indices[at + 1];
                const std::uint32_t i2 = indices[at + 2];

                const auto position = [&](std::uint32_t index) {
                    const auto& p = vertices[index].position;
                    return Vec3{ p[0], p[1], p[2] };
                };
                const auto texcoord = [&](std::uint32_t index) {
                    const auto& t = vertices[index].uv;
                    return Vec2{ t[0], t[1] };
                };

                const Vec3 edge1 = position(i1) - position(i0);
                const Vec3 edge2 = position(i2) - position(i0);
                const Vec2 duv1 = texcoord(i1) - texcoord(i0);
                const Vec2 duv2 = texcoord(i2) - texcoord(i0);

                const float determinant = (duv1.x * duv2.y) - (duv2.x * duv1.y);
                // A triangle with no area in texture space says nothing about
                // the tangent. Skipping it leaves the neighbours to decide.
                constexpr float kTooSmall = 1e-12F;
                if (std::abs(determinant) < kTooSmall) {
                    continue;
                }
                const float scale = 1.0F / determinant;

                const Vec3 tangent = ((edge1 * duv2.y) - (edge2 * duv1.y)) * scale;
                const Vec3 bitangent = ((edge2 * duv1.x) - (edge1 * duv2.x)) * scale;

                for (const std::uint32_t index : { i0, i1, i2 }) {
                    along[index] += tangent;
                    across[index] += bitangent;
                }
            }

            for (std::size_t at = 0; at < vertices.size(); ++at) {
                as::MeshVertex& vertex = vertices[at];
                const Vec3 normal{ vertex.normal[0], vertex.normal[1], vertex.normal[2] };

                // Gram-Schmidt, so the tangent lies in the surface.
                const Vec3 tangent = normalized(along[at] - (normal * glm::dot(normal, along[at])));

                // glTF puts the bitangent sign in w. It says which way the
                // bitangent runs, and a mirrored UV island needs the other one.
                const float sign =
                    glm::dot(glm::cross(normal, tangent), across[at]) < 0.0F ? -1.0F : 1.0F;

                vertex.tangent = { tangent.x, tangent.y, tangent.z, sign };
            }
        }

        /// Everything one glTF mesh turned into, before it is written.
        struct Built {
            std::vector<as::MeshVertex> vertices;
            std::vector<std::uint32_t> indices;
            std::vector<as::MeshSubmesh> submeshes;
            Vec3 min{ std::numeric_limits<float>::max() };
            Vec3 max{ std::numeric_limits<float>::lowest() };
        };

        /**
         * The identity of the material a primitive names.
         *
         * cgltf stores a pointer, and the cooked file stores a GUID. The index
         * is the distance from the start of the material list, which is the
         * same index cook_materials() derived each GUID from.
         */
        [[nodiscard]] engine::Guid material_of(const cgltf_data& data,
                                               const cgltf_material* material,
                                               const CookedMaterials& materials) {
            if (material == nullptr || data.materials == nullptr) {
                return engine::Guid{};
            }
            const auto index = static_cast<std::size_t>(material - data.materials);
            if (index >= materials.guids.size()) {
                return engine::Guid{};
            }
            return materials.guids[index];
        }

        /// Reads one primitive and appends it to the mesh being built.
        [[nodiscard]] bool add_primitive(const cgltf_primitive& primitive,
                                         engine::Guid material, Built& built,
                                         std::string_view where, bool& has_tangents) {
            if (primitive.type != cgltf_primitive_type_triangles) {
                // A point or a line primitive is legal glTF and this engine
                // draws neither. Skipping beats failing the whole cook.
                ENGINE_LOG_WARN("{}: skipping a primitive that is not triangles.", where);
                return true;
            }
            if (primitive.indices == nullptr) {
                ENGINE_LOG_ERROR("{}: a primitive has no indices, and this reads indexed "
                                 "geometry only.",
                                 where);
                return false;
            }

            const cgltf_accessor* positions = nullptr;
            const cgltf_accessor* normals = nullptr;
            const cgltf_accessor* tangents = nullptr;
            const cgltf_accessor* uvs = nullptr;

            for (cgltf_size at = 0; at < primitive.attributes_count; ++at) {
                const cgltf_attribute& attribute = primitive.attributes[at];
                switch (attribute.type) {
                case cgltf_attribute_type_position:
                    positions = attribute.data;
                    break;
                case cgltf_attribute_type_normal:
                    normals = attribute.data;
                    break;
                case cgltf_attribute_type_tangent:
                    tangents = attribute.data;
                    break;
                case cgltf_attribute_type_texcoord:
                    // The first set only. A second set is for a lightmap, and
                    // nothing reads one until there is a lightmap to read.
                    if (attribute.index == 0) {
                        uvs = attribute.data;
                    }
                    break;
                default:
                    break;
                }
            }

            std::vector<float> position_floats;
            if (!read_attribute(positions, 3, position_floats, where)) {
                ENGINE_LOG_ERROR("{}: a primitive has no POSITION.", where);
                return false;
            }
            const std::size_t count = positions->count;

            std::vector<float> normal_floats;
            std::vector<float> tangent_floats;
            std::vector<float> uv_floats;
            const bool have_normals = read_attribute(normals, 3, normal_floats, where);
            const bool have_tangents = read_attribute(tangents, 4, tangent_floats, where);
            const bool have_uvs = read_attribute(uvs, 2, uv_floats, where);
            has_tangents = has_tangents && have_tangents;

            const auto first_vertex = static_cast<std::uint32_t>(built.vertices.size());
            built.vertices.reserve(built.vertices.size() + count);

            for (std::size_t at = 0; at < count; ++at) {
                as::MeshVertex vertex;
                vertex.position = { position_floats[at * 3], position_floats[(at * 3) + 1],
                                    position_floats[(at * 3) + 2] };
                if (have_normals) {
                    vertex.normal = { normal_floats[at * 3], normal_floats[(at * 3) + 1],
                                      normal_floats[(at * 3) + 2] };
                }
                if (have_tangents) {
                    vertex.tangent = { tangent_floats[at * 4], tangent_floats[(at * 4) + 1],
                                       tangent_floats[(at * 4) + 2],
                                       tangent_floats[(at * 4) + 3] };
                }
                if (have_uvs) {
                    vertex.uv = { uv_floats[at * 2], uv_floats[(at * 2) + 1] };
                }

                const Vec3 position{ vertex.position[0], vertex.position[1], vertex.position[2] };

                // A position that is not a number poisons the bounds, and the
                // bounds decide culling in M5 and picking in #34. Refusing here
                // names the source file. Letting it through gives a mesh that
                // never draws, or always draws, for no visible reason.
                if (!std::isfinite(position.x) || !std::isfinite(position.y) ||
                    !std::isfinite(position.z)) {
                    ENGINE_LOG_ERROR("{}: vertex {} has a position that is not a number.", where,
                                     at);
                    return false;
                }

                built.min = glm::min(built.min, position);
                built.max = glm::max(built.max, position);

                built.vertices.push_back(vertex);
            }

            std::vector<std::uint32_t> local(primitive.indices->count);
            const cgltf_size read = cgltf_accessor_unpack_indices(
                primitive.indices, local.data(), sizeof(std::uint32_t), local.size());
            if (read != local.size()) {
                ENGINE_LOG_ERROR("{}: the index list gave {} of {} indices.", where, read,
                                 local.size());
                return false;
            }
            if (local.size() % kVerticesPerTriangle != 0) {
                ENGINE_LOG_ERROR("{}: a primitive holds {} indices, which is not whole "
                                 "triangles.",
                                 where, local.size());
                return false;
            }

            const auto first_index = static_cast<std::uint32_t>(built.indices.size());
            for (const std::uint32_t index : local) {
                if (index >= count) {
                    ENGINE_LOG_ERROR("{}: an index names vertex {} and the primitive holds {}.",
                                     where, index, count);
                    return false;
                }
                built.indices.push_back(first_vertex + index);
            }

            // One submesh for each primitive, because a glTF primitive is
            // exactly the run of triangles that shares a material.
            built.submeshes.push_back(as::MeshSubmesh{
                .first_index = first_index,
                .index_count = static_cast<std::uint32_t>(local.size()),
                .material = material });
            return true;
        }

        /**
         * Reorders the vertices and the indices for the GPU.
         *
         * Two passes, and the order matters. The vertex cache pass reorders the
         * indices so a triangle reuses a vertex the GPU still holds. The vertex
         * fetch pass then reorders the vertices so the ones a triangle reads sit
         * near each other in memory. Running fetch first would undo it.
         *
         * Each pass runs over the whole index list rather than over each
         * submesh, so a submesh keeps its run. meshopt reorders inside a run and
         * never moves an index between two of them.
         */
        void optimize(Built& built) {
            for (as::MeshSubmesh& submesh : built.submeshes) {
                meshopt_optimizeVertexCache(built.indices.data() + submesh.first_index,
                                            built.indices.data() + submesh.first_index,
                                            submesh.index_count, built.vertices.size());
            }

            std::vector<as::MeshVertex> reordered(built.vertices.size());
            const std::size_t kept = meshopt_optimizeVertexFetch(
                reordered.data(), built.indices.data(), built.indices.size(),
                built.vertices.data(), built.vertices.size(), sizeof(as::MeshVertex));
            reordered.resize(kept);
            built.vertices = std::move(reordered);
        }

        [[nodiscard]] bool write_mesh(const std::filesystem::path& destination,
                                      const Built& built) {
            as::MeshHeader header;
            header.vertex_count = static_cast<std::uint32_t>(built.vertices.size());
            header.index_count = static_cast<std::uint32_t>(built.indices.size());
            header.submesh_count = static_cast<std::uint32_t>(built.submeshes.size());
            header.min = { built.min.x, built.min.y, built.min.z };
            header.max = { built.max.x, built.max.y, built.max.z };

            std::error_code error;
            std::filesystem::create_directories(destination.parent_path(), error);

            std::ofstream file(destination, std::ios::binary | std::ios::trunc);
            if (!file) {
                ENGINE_LOG_ERROR("{}: could not open it for writing.", destination.string());
                return false;
            }
            const auto write = [&](const void* data, std::size_t size) {
                file.write(reinterpret_cast<const char*>(data),
                           static_cast<std::streamsize>(size));
            };
            write(&header, sizeof(header));
            write(built.vertices.data(), built.vertices.size() * sizeof(as::MeshVertex));
            write(built.indices.data(), built.indices.size() * sizeof(std::uint32_t));
            write(built.submeshes.data(), built.submeshes.size() * sizeof(as::MeshSubmesh));

            if (!file) {
                ENGINE_LOG_ERROR("{}: the write failed part way through.", destination.string());
                return false;
            }
            return true;
        }

    } // namespace

    bool gltf_references(const std::filesystem::path& source,
                         const std::filesystem::path& relative, GltfReferences& out) {
        const std::string name = source.string();

        cgltf_options options{};
        GltfData held;
        if (cgltf_parse_file(&options, name.c_str(), &held.data) != cgltf_result_success) {
            return false;
        }

        // A GLB holds its buffers and its images inside itself, and a data URI
        // carries the bytes inline. Neither one names a file to watch, and
        // gltf_uri_path reports that.
        const std::filesystem::path directory = relative.parent_path();

        for (cgltf_size at = 0; at < held.data->buffers_count; ++at) {
            std::filesystem::path path;
            if (gltf_uri_path(held.data->buffers[at].uri, directory, path)) {
                out.buffers.push_back(std::move(path));
            }
        }
        for (cgltf_size at = 0; at < held.data->images_count; ++at) {
            std::filesystem::path path;
            if (gltf_uri_path(held.data->images[at].uri, directory, path)) {
                out.images.push_back(std::move(path));
            }
        }
        return true;
    }

    bool is_mesh_extension(const std::string& extension) {
        const std::string lower = lowered(extension);
        return lower == ".gltf" || lower == ".glb";
    }

    bool gltf_uri_path(const char* uri, const std::filesystem::path& directory,
                       std::filesystem::path& out) {
        if (uri == nullptr || std::string_view{ uri }.starts_with("data:")) {
            return false;
        }

        // cgltf decodes in place, so this works on a copy rather than on the
        // parsed data. The decode only ever shortens the text, and it leaves
        // the old terminator behind, so the length comes from strlen.
        std::string decoded{ uri };
        cgltf_decode_uri(decoded.data());
        decoded.resize(std::strlen(decoded.c_str()));

        out = directory / decoded;
        return true;
    }

    bool cook_gltf(const std::filesystem::path& source, const std::filesystem::path& out_root,
                   const std::filesystem::path& relative, engine::Guid parent,
                   std::vector<as::ManifestOutput>& outputs) {
        const std::string name = source.string();

        cgltf_options options{};
        GltfData held;
        cgltf_result result = cgltf_parse_file(&options, name.c_str(), &held.data);
        if (result != cgltf_result_success) {
            ENGINE_LOG_ERROR("{}: {}.", name, gltf_error(result));
            return false;
        }

        // A .gltf names its buffers in separate files, and a .glb carries them
        // inside. This call covers both, and it needs the source path so a
        // relative buffer URI resolves next to the file.
        result = cgltf_load_buffers(&options, held.data, name.c_str());
        if (result != cgltf_result_success) {
            ENGINE_LOG_ERROR("{}: {}.", name, gltf_error(result));
            return false;
        }

        result = cgltf_validate(held.data);
        if (result != cgltf_result_success) {
            ENGINE_LOG_ERROR("{}: {}.", name, gltf_error(result));
            return false;
        }

        if (held.data->meshes_count == 0) {
            ENGINE_LOG_ERROR("{}: it holds no mesh.", name);
            return false;
        }

        // The order here is what each part needs, not the order they go out in.
        // An image inside the file has to cook before the materials, because a
        // material stores its identity. A material has to cook before the
        // meshes, because a submesh stores its identity.
        //
        // The outputs go out meshes first. The cooker checks the name of the
        // first output to decide whether it may skip a source, so a glTF file
        // has to keep naming its first mesh there.
        InlineImages images;
        if (!cook_inline_images(*held.data, source, out_root, relative, parent, images)) {
            return false;
        }

        CookedMaterials materials;
        if (!cook_materials(*held.data, source, out_root, relative, parent, images, materials)) {
            return false;
        }

        for (cgltf_size at = 0; at < held.data->meshes_count; ++at) {
            const cgltf_mesh& mesh = held.data->meshes[at];
            const std::string where = name + " mesh " + std::to_string(at);

            Built built;
            bool has_tangents = true;
            for (cgltf_size part = 0; part < mesh.primitives_count; ++part) {
                const cgltf_primitive& primitive = mesh.primitives[part];
                const engine::Guid material =
                    material_of(*held.data, primitive.material, materials);
                if (!add_primitive(primitive, material, built, where, has_tangents)) {
                    return false;
                }
            }

            if (built.vertices.empty() || built.indices.empty()) {
                ENGINE_LOG_ERROR("{}: it holds no triangles.", where);
                return false;
            }

            if (!has_tangents) {
                ENGINE_LOG_INFO("{}: the source has no tangents, so the cooker built them.",
                                where);
                build_tangents(built.vertices, built.indices);
            }

            optimize(built);

            std::filesystem::path cooked_relative = relative;
            cooked_relative += "." + std::to_string(at);
            cooked_relative += as::kMeshExtension;

            if (!write_mesh(out_root / cooked_relative, built)) {
                return false;
            }
            outputs.push_back(as::ManifestOutput{
                .cooked = as::manifest_path(cooked_relative),
                .guid = engine::Guid::derive(parent, kMeshPartKind,
                                             static_cast<std::uint32_t>(at)) });
        }

        outputs.insert(outputs.end(), materials.outputs.begin(), materials.outputs.end());
        outputs.insert(outputs.end(), images.outputs.begin(), images.outputs.end());
        return true;
    }

} // namespace cooker
