// M4.4 tests for the glTF importer, the cooked mesh, and the cooked material.
//
// The property that carries this part is that a cooked mesh describes itself
// exactly. A file whose header disagrees with its contents becomes a draw call
// that reads past the end of a GPU buffer, and the message that comes back
// from that names nothing. So read_mesh checks every count and every index,
// and the tests here drive each refusal.
//
// The glTF files are built in the test rather than committed. A committed
// binary says nothing in a diff, and a builder here can make the exact shape a
// case needs, including the broken ones.

#include "assets/manifest.h"
#include "assets/material.h"
#include "assets/mesh.h"
#include "assets/meta.h"
#include "assets/texture.h"
#include "check.h"
#include "cook.h"
#include "mesh.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

    using test::check;
    namespace as = engine::assets;

    std::filesystem::path scratch(std::string_view name) {
        const std::filesystem::path path =
            std::filesystem::temp_directory_path() / "camina_test_mesh" / name;
        std::filesystem::remove_all(path);
        std::filesystem::create_directories(path);
        return path;
    }

    void write_bytes(const std::filesystem::path& path, std::span<const std::byte> bytes) {
        std::filesystem::create_directories(path.parent_path());
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file.write(reinterpret_cast<const char*>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()));
    }

    std::vector<std::byte> read_bytes(const std::filesystem::path& path) {
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file) {
            return {};
        }
        const std::streamoff size = file.tellg();
        file.seekg(0);
        std::vector<std::byte> bytes(static_cast<std::size_t>(size));
        file.read(reinterpret_cast<char*>(bytes.data()), size);
        return bytes;
    }

    /// Appends a value to a byte buffer, in the layout a glTF buffer holds.
    template <typename T>
    void append(std::vector<std::byte>& out, const T& value) {
        const auto* first = reinterpret_cast<const std::byte*>(&value);
        out.insert(out.end(), first, first + sizeof(T));
    }

    /// Interleaved-free geometry: positions, normals, UVs, then 16-bit indices.
    struct Geometry {
        std::vector<std::byte> buffer;
        std::size_t positions_at = 0;
        std::size_t normals_at = 0;
        std::size_t uvs_at = 0;
        std::size_t indices_at = 0;
        std::uint32_t vertex_count = 0;
        std::uint32_t index_count = 0;
        /// Where an embedded image sits, when the file carries one.
        std::size_t image_at = 0;
        std::uint32_t image_size = 0;
    };

    /// Which material slot uses the image, which is what decides its color space.
    enum class ImageSlot : std::uint8_t {
        Both,      ///< Base color and normal, so a test can tell the fields apart.
        BaseColor, ///< Color, so the image has to read as sRGB.
        Normal,    ///< Numbers, so the image has to read as linear.
    };

    /**
     * Builds the buffer for one triangle in the XY plane.
     *
     * The shape is deliberate. The three corners span a known box, so the AABB
     * has an answer the test can name, and the UVs are not degenerate, so the
     * tangent has a direction rather than a fallback.
     */
    Geometry build_triangle() {
        Geometry out;

        out.positions_at = out.buffer.size();
        for (const std::array<float, 3> position :
             { std::array<float, 3>{ 0.0F, 0.0F, 0.0F }, std::array<float, 3>{ 2.0F, 0.0F, 0.0F },
               std::array<float, 3>{ 0.0F, 4.0F, 0.0F } }) {
            for (const float value : position) {
                append(out.buffer, value);
            }
        }

        out.normals_at = out.buffer.size();
        for (int corner = 0; corner < 3; ++corner) {
            for (const float value : { 0.0F, 0.0F, 1.0F }) {
                append(out.buffer, value);
            }
        }

        out.uvs_at = out.buffer.size();
        for (const std::array<float, 2> uv :
             { std::array<float, 2>{ 0.0F, 0.0F }, std::array<float, 2>{ 1.0F, 0.0F },
               std::array<float, 2>{ 0.0F, 1.0F } }) {
            for (const float value : uv) {
                append(out.buffer, value);
            }
        }

        out.indices_at = out.buffer.size();
        for (const std::uint16_t index : std::array<std::uint16_t, 3>{ 0, 1, 2 }) {
            append(out.buffer, index);
        }
        out.vertex_count = 3;
        out.index_count = 3;
        return out;
    }

    /// The glTF JSON for one or two meshes over the triangle buffer.
    /**
     * The glTF JSON over one geometry buffer, with one mesh or several.
     *
     * Built with appends rather than as one expression. A single concatenation
     * of this many pieces is what a formatter cannot lay out readably, and a
     * test that nobody can read is a test nobody will fix.
     */
    /// Appends an encoded image to the buffer, the way a .glb carries one.
    Geometry with_image(Geometry parts, std::span<const std::byte> image) {
        parts.image_at = parts.buffer.size();
        parts.buffer.insert(parts.buffer.end(), image.begin(), image.end());
        parts.image_size = static_cast<std::uint32_t>(image.size());
        return parts;
    }

    std::string geometry_json(const Geometry& parts, int mesh_count,
                              std::string_view buffer_uri, std::string_view image_uri = {},
                              ImageSlot slot = ImageSlot::Both) {
        const auto number = [](std::size_t value) { return std::to_string(value); };
        const auto bytes = [&](std::uint32_t count, std::size_t stride) {
            return number(static_cast<std::size_t>(count) * stride);
        };

        // One view for each attribute, then the indices. The order matches the
        // order build_triangle and build_grid write them in.
        std::string views = R"("bufferViews":[)";
        views += R"({"buffer":0,"byteOffset":)" + number(parts.positions_at) +
                 R"(,"byteLength":)" + bytes(parts.vertex_count, 12) + "},";
        views += R"({"buffer":0,"byteOffset":)" + number(parts.normals_at) +
                 R"(,"byteLength":)" + bytes(parts.vertex_count, 12) + "},";
        views += R"({"buffer":0,"byteOffset":)" + number(parts.uvs_at) +
                 R"(,"byteLength":)" + bytes(parts.vertex_count, 8) + "},";
        views += R"({"buffer":0,"byteOffset":)" + number(parts.indices_at) +
                 R"(,"byteLength":)" + bytes(parts.index_count, 2) + "}";
        // A fifth view for an image the buffer carries, which is the form a
        // .glb uses and the one most exporters produce.
        if (parts.image_size > 0) {
            views += R"(,{"buffer":0,"byteOffset":)" + number(parts.image_at) +
                     R"(,"byteLength":)" + number(parts.image_size) + "}";
        }
        views += "],";

        const std::string count = number(parts.vertex_count);
        std::string accessors = R"("accessors":[)";
        accessors += R"({"bufferView":0,"componentType":5126,"count":)" + count +
                     R"(,"type":"VEC3"},)";
        accessors += R"({"bufferView":1,"componentType":5126,"count":)" + count +
                     R"(,"type":"VEC3"},)";
        accessors += R"({"bufferView":2,"componentType":5126,"count":)" + count +
                     R"(,"type":"VEC2"},)";
        accessors += R"({"bufferView":3,"componentType":5123,"count":)" +
                     number(parts.index_count) + R"(,"type":"SCALAR"}],)";

        // One material over one image, when the caller asked for one. The
        // image arrives either as a URI or in the buffer view added above.
        std::string materials;
        std::string material_of;
        if (!image_uri.empty() || parts.image_size > 0) {
            if (parts.image_size > 0) {
                // The declared type does not reach this pipeline. stb_image
                // sniffs the bytes, so the cook works whatever this says.
                materials = R"("images":[{"bufferView":4,"mimeType":"image/png"}],)";
            } else {
                materials = R"("images":[{"uri":")" + std::string{ image_uri } + R"("}],)";
            }
            materials += R"("textures":[{"source":0}],)";

            // Which slot the image lands in decides how it has to be read, and
            // for an image with no file that is the only thing that can.
            std::string slots;
            if (slot != ImageSlot::Normal) {
                slots += R"("pbrMetallicRoughness":{"baseColorTexture":{"index":0},)"
                         R"("baseColorFactor":[0.25,0.5,0.75,1.0],"metallicFactor":0.125},)";
            }
            if (slot != ImageSlot::BaseColor) {
                slots += R"("normalTexture":{"index":0},)";
            }
            materials += R"("materials":[{"name":"only",)" + slots + R"("doubleSided":true}],)";
            material_of = R"(,"material":0)";
        }

        // Every mesh names the same accessors. That is enough to give a file
        // several meshes, which is what the sub-asset identities need.
        std::string meshes = R"("meshes":[)";
        for (int at = 0; at < mesh_count; ++at) {
            if (at > 0) {
                meshes += ",";
            }
            meshes += R"({"name":"mesh)" + std::to_string(at) +
                      R"(","primitives":[{"attributes":)"
                      R"({"POSITION":0,"NORMAL":1,"TEXCOORD_0":2},"indices":3)" +
                      material_of + "}]}";
        }
        meshes += "]";

        std::string buffer = R"("buffers":[{)";
        if (!buffer_uri.empty()) {
            buffer += R"("uri":")" + std::string{ buffer_uri } + R"(",)";
        }
        buffer += R"("byteLength":)" + number(parts.buffer.size()) + "}],";

        return R"({"asset":{"version":"2.0"},)" + buffer + views + accessors + materials +
               meshes + "}";
    }

    /// Writes a .glb, which carries its buffer inside rather than beside it.
    void write_glb(const std::filesystem::path& path, const Geometry& parts, int mesh_count,
                   std::string_view image_uri = {}, ImageSlot slot = ImageSlot::Both) {
        std::string json = geometry_json(parts, mesh_count, "", image_uri, slot);
        while (json.size() % 4 != 0) {
            json.push_back(' ');
        }
        std::vector<std::byte> binary = parts.buffer;
        while (binary.size() % 4 != 0) {
            binary.push_back(std::byte{ 0 });
        }

        constexpr std::uint32_t kGlbMagic = 0x46546C67;  // "glTF"
        constexpr std::uint32_t kChunkJson = 0x4E4F534A; // "JSON"
        constexpr std::uint32_t kChunkBin = 0x004E4942;  // "BIN"
        constexpr std::uint32_t kHeaderSize = 12;
        constexpr std::uint32_t kChunkHeaderSize = 8;

        std::vector<std::byte> out;
        const auto total = static_cast<std::uint32_t>(
            kHeaderSize + kChunkHeaderSize + json.size() + kChunkHeaderSize + binary.size());
        append(out, kGlbMagic);
        append(out, std::uint32_t{ 2 });
        append(out, total);

        append(out, static_cast<std::uint32_t>(json.size()));
        append(out, kChunkJson);
        const auto* json_first = reinterpret_cast<const std::byte*>(json.data());
        out.insert(out.end(), json_first, json_first + json.size());

        append(out, static_cast<std::uint32_t>(binary.size()));
        append(out, kChunkBin);
        out.insert(out.end(), binary.begin(), binary.end());

        write_bytes(path, out);
    }

    /// Writes a .glb that carries its image in a buffer view, with no URI.
    void write_glb_with_image(const std::filesystem::path& path, const Geometry& parts,
                              std::span<const std::byte> image,
                              ImageSlot slot = ImageSlot::BaseColor) {
        write_glb(path, with_image(parts, image), 1, {}, slot);
    }

    /// Writes a .gltf and the .bin it names, which is the two-file form.
    void write_gltf(const std::filesystem::path& path, const Geometry& parts, int mesh_count) {
        const std::string bin_name = path.stem().string() + ".bin";
        write_bytes(path.parent_path() / bin_name, parts.buffer);
        const std::string json = geometry_json(parts, mesh_count, bin_name);
        write_bytes(path, std::as_bytes(std::span{ json.data(), json.size() }));
    }

    /// A parent identity for a test that calls the rule rather than the cooker.
    const engine::Guid kParent{ .high = 0x0102030405060708ULL, .low = 0x090A0B0C0D0E0F10ULL };

    /// Calls the glTF rule with that parent, so a case reads as one line.
    bool cook_gltf(const std::filesystem::path& source, const std::filesystem::path& out,
                   const std::filesystem::path& relative,
                   std::vector<as::ManifestOutput>& outputs) {
        return cooker::cook_gltf(source, out, relative, kParent, outputs);
    }

    void test_glb_cooks() {
        const std::filesystem::path dir = scratch("glb");
        const std::filesystem::path out = scratch("glb_out");
        write_glb(dir / "one.glb", build_triangle(), 1);

        std::vector<as::ManifestOutput> cooked;
        check(cook_gltf(dir / "one.glb", out, "one.glb", cooked), "a .glb cooks");
        check(cooked.size() == 1, "and one mesh with no material gives one cooked file");
        check(cooked.at(0).cooked == "one.glb.0.mesh",
              "and the name numbers the part before the extension");

        as::Mesh mesh;
        const std::vector<std::byte> bytes = read_bytes(out / cooked.at(0).cooked);
        check(as::read_mesh(bytes, mesh, "one.glb.0.mesh"), "the cooked mesh reads back");
        check(mesh.vertices.size() == 3, "and it holds the three vertices");
        check(mesh.indices.size() == 3, "and the three indices");
        check(mesh.submeshes.size() == 1, "and one submesh for the one primitive");
        check(mesh.submeshes.at(0).index_count == 3, "which covers every index");

        // The corners were (0,0,0), (2,0,0) and (0,4,0). #34 picks an entity
        // with this box and M5 culls with it, so a wrong one is not cosmetic.
        check(mesh.min == engine::Vec3(0.0F, 0.0F, 0.0F), "the bounds start at the low corner");
        check(mesh.max == engine::Vec3(2.0F, 4.0F, 0.0F), "and reach the high one");

        std::filesystem::remove_all(dir.parent_path());
    }

    void test_gltf_with_a_separate_buffer_cooks() {
        const std::filesystem::path dir = scratch("gltf");
        const std::filesystem::path out = scratch("gltf_out");
        write_gltf(dir / "two.gltf", build_triangle(), 2);

        // The .bin sits next to the .gltf and the loader has to find it there.
        // A path that resolved against the working directory instead would work
        // on the machine that wrote the test and fail everywhere else.
        std::vector<as::ManifestOutput> cooked;
        check(cook_gltf(dir / "two.gltf", out, "two.gltf", cooked),
              "a .gltf with a separate buffer cooks");
        check(cooked.size() == 2, "and two meshes give two cooked files");
        check(cooked.at(1).cooked == "two.gltf.1.mesh", "numbered in mesh order");
        check(std::filesystem::exists(out / cooked.at(0).cooked) &&
                  std::filesystem::exists(out / cooked.at(1).cooked),
              "and both landed");

        std::filesystem::remove_all(dir.parent_path());
    }

    void test_tangents_are_built_when_the_source_has_none() {
        const std::filesystem::path dir = scratch("tangent");
        const std::filesystem::path out = scratch("tangent_out");
        write_glb(dir / "flat.glb", build_triangle(), 1);

        std::vector<as::ManifestOutput> cooked;
        check(cook_gltf(dir / "flat.glb", out, "flat.glb", cooked), "it cooks");

        as::Mesh mesh;
        const std::vector<std::byte> bytes = read_bytes(out / cooked.at(0).cooked);
        check(as::read_mesh(bytes, mesh, "flat.glb.0.mesh"), "it reads back");

        // The source names no TANGENT. A zero tangent would give a normal map
        // no direction to work in, and the lighting would be wrong everywhere
        // rather than obviously broken.
        bool every_tangent_set = true;
        for (const as::MeshVertex& vertex : mesh.vertices) {
            const float length = (vertex.tangent[0] * vertex.tangent[0]) +
                                 (vertex.tangent[1] * vertex.tangent[1]) +
                                 (vertex.tangent[2] * vertex.tangent[2]);
            every_tangent_set = every_tangent_set && length > 0.9F && length < 1.1F;
        }
        check(every_tangent_set, "a source with no tangents gets unit tangents");

        bool every_sign_valid = true;
        for (const as::MeshVertex& vertex : mesh.vertices) {
            every_sign_valid =
                every_sign_valid && (vertex.tangent[3] == 1.0F || vertex.tangent[3] == -1.0F);
        }
        check(every_sign_valid, "and the bitangent sign in w is plus or minus one");

        std::filesystem::remove_all(dir.parent_path());
    }

    /**
     * A grid, so the optimizer has something real to reorder.
     *
     * The triangle above cannot test this. meshopt reorders triangles for the
     * vertex cache and then moves the vertices to match, and with three
     * vertices neither pass has anywhere to move anything. A grid has both
     * shared vertices and enough triangles for the passes to do their work, so
     * a mistake in the remapping shows up as scrambled geometry.
     */
    Geometry build_grid(std::uint16_t side) {
        Geometry out;
        const auto count = static_cast<std::uint16_t>(side + 1);

        out.positions_at = out.buffer.size();
        for (std::uint16_t y = 0; y < count; ++y) {
            for (std::uint16_t x = 0; x < count; ++x) {
                append(out.buffer, static_cast<float>(x));
                append(out.buffer, static_cast<float>(y));
                append(out.buffer, 0.0F);
            }
        }

        out.normals_at = out.buffer.size();
        for (std::uint32_t at = 0; at < static_cast<std::uint32_t>(count) * count; ++at) {
            for (const float value : { 0.0F, 0.0F, 1.0F }) {
                append(out.buffer, value);
            }
        }

        out.uvs_at = out.buffer.size();
        for (std::uint16_t y = 0; y < count; ++y) {
            for (std::uint16_t x = 0; x < count; ++x) {
                append(out.buffer, static_cast<float>(x) / static_cast<float>(side));
                append(out.buffer, static_cast<float>(y) / static_cast<float>(side));
            }
        }

        out.indices_at = out.buffer.size();
        for (std::uint16_t y = 0; y < side; ++y) {
            for (std::uint16_t x = 0; x < side; ++x) {
                const auto corner = static_cast<int>((y * count) + x);
                const int right = 1;
                const int up = count;
                for (const int offset :
                     std::array<int, 6>{ 0, right, up, right, up + right, up }) {
                    append(out.buffer, static_cast<std::uint16_t>(corner + offset));
                }
            }
        }
        out.vertex_count = static_cast<std::uint32_t>(count) * count;
        out.index_count = static_cast<std::uint32_t>(side) * side * 6;
        return out;
    }

    /// Every triangle as a sorted triple of corners, so order does not matter.
    std::set<std::array<std::array<float, 3>, 3>> triangles_of(const as::Mesh& mesh) {
        std::set<std::array<std::array<float, 3>, 3>> out;
        for (std::size_t at = 0; at + 2 < mesh.indices.size(); at += 3) {
            std::array<std::array<float, 3>, 3> corners{
                mesh.vertices.at(mesh.indices[at]).position,
                mesh.vertices.at(mesh.indices[at + 1]).position,
                mesh.vertices.at(mesh.indices[at + 2]).position,
            };
            std::ranges::sort(corners);
            out.insert(corners);
        }
        return out;
    }

    void test_the_optimizer_keeps_the_geometry() {
        const std::filesystem::path dir = scratch("grid");
        const std::filesystem::path out = scratch("grid_out");

        constexpr std::uint16_t kSide = 8;
        constexpr std::size_t kQuads = static_cast<std::size_t>(kSide) * kSide;
        write_glb(dir / "grid.glb", build_grid(kSide), 1);

        std::vector<as::ManifestOutput> cooked;
        check(cook_gltf(dir / "grid.glb", out, "grid.glb", cooked), "a grid cooks");

        as::Mesh mesh;
        check(as::read_mesh(read_bytes(out / cooked.at(0).cooked), mesh, "grid"),
              "it reads back");
        check(mesh.indices.size() == kQuads * 6, "and it holds every triangle");
        check(mesh.vertices.size() == static_cast<std::size_t>(kSide + 1) * (kSide + 1),
              "and every vertex, with none added and none dropped");

        // The optimizer reorders the triangles and then moves the vertices to
        // match. The order is free to change and the geometry is not. A remap
        // applied to one and not the other scrambles the mesh, and the file
        // still passes every size check.
        const std::set<std::array<std::array<float, 3>, 3>> got = triangles_of(mesh);
        check(got.size() == kQuads * 2, "and the triangles are all distinct");

        std::set<std::array<std::array<float, 3>, 3>> wanted;
        for (std::uint16_t y = 0; y < kSide; ++y) {
            for (std::uint16_t x = 0; x < kSide; ++x) {
                const auto corner = [&](int dx, int dy) {
                    return std::array<float, 3>{ static_cast<float>(x + dx),
                                                 static_cast<float>(y + dy), 0.0F };
                };
                for (const auto& triple : { std::array{ corner(0, 0), corner(1, 0),
                                                        corner(0, 1) },
                                            std::array{ corner(1, 0), corner(1, 1),
                                                        corner(0, 1) } }) {
                    auto sorted = triple;
                    std::ranges::sort(sorted);
                    wanted.insert(sorted);
                }
            }
        }
        check(got == wanted, "and every triangle is the one the source had");

        std::filesystem::remove_all(dir.parent_path());
    }

    void test_a_broken_gltf_fails() {
        const std::filesystem::path dir = scratch("broken");
        const std::filesystem::path out = scratch("broken_out");

        const std::string junk = "this is not glTF";
        write_bytes(dir / "junk.gltf", std::as_bytes(std::span{ junk.data(), junk.size() }));

        std::vector<as::ManifestOutput> cooked;
        check(!cook_gltf(dir / "junk.gltf", out, "junk.gltf", cooked),
              "a file that is not glTF fails");
        check(cooked.empty(), "and nothing was recorded for it");

        // A .gltf whose .bin is missing. cgltf parses the JSON and then cannot
        // load the buffer, which is a different failure from bad JSON and needs
        // its own message.
        write_gltf(dir / "orphan.gltf", build_triangle(), 1);
        std::filesystem::remove(dir / "orphan.bin");
        check(!cook_gltf(dir / "orphan.gltf", out, "orphan.gltf", cooked),
              "a .gltf whose buffer is missing fails");

        check(!cook_gltf(dir / "not_there.gltf", out, "not_there.gltf", cooked),
              "a file that is not there fails");

        // A position that is not a number poisons the bounds, and the bounds
        // decide culling and picking. Catching it at the cook names the source
        // file. Letting it through gives a mesh that never draws for no
        // visible reason, much later.
        Geometry poisoned = build_triangle();
        const float nan = std::numeric_limits<float>::quiet_NaN();
        std::memcpy(poisoned.buffer.data(), &nan, sizeof(nan));
        write_glb(dir / "nan.glb", poisoned, 1);
        check(!cook_gltf(dir / "nan.glb", out, "nan.glb", cooked),
              "a vertex position that is not a number fails the cook");

        std::filesystem::remove_all(dir.parent_path());
    }

    /// Builds a cooked mesh file in memory, so a test can then break it.
    std::vector<std::byte> make_mesh_file(const as::MeshHeader& header,
                                          const std::vector<as::MeshVertex>& vertices,
                                          const std::vector<std::uint32_t>& indices,
                                          const std::vector<as::MeshSubmesh>& submeshes) {
        std::vector<std::byte> bytes(as::kMeshHeaderSize);
        std::memcpy(bytes.data(), &header, sizeof(header));
        const auto add = [&](const void* data, std::size_t size) {
            const auto* first = static_cast<const std::byte*>(data);
            bytes.insert(bytes.end(), first, first + size);
        };
        add(vertices.data(), vertices.size() * sizeof(as::MeshVertex));
        add(indices.data(), indices.size() * sizeof(std::uint32_t));
        add(submeshes.data(), submeshes.size() * sizeof(as::MeshSubmesh));
        return bytes;
    }

    void test_read_mesh_refuses_a_bad_file() {
        const std::vector<as::MeshVertex> vertices(3);
        const std::vector<std::uint32_t> indices{ 0, 1, 2 };
        const std::vector<as::MeshSubmesh> submeshes{
            as::MeshSubmesh{ .first_index = 0, .index_count = 3, .material = {} }
        };

        as::MeshHeader header;
        header.vertex_count = 3;
        header.index_count = 3;
        header.submesh_count = 1;

        as::Mesh mesh;
        const std::vector<std::byte> good = make_mesh_file(header, vertices, indices, submeshes);
        check(as::read_mesh(good, mesh, "good"), "a whole file reads");
        check(mesh.vertices.size() == 3 && mesh.indices.size() == 3, "and it holds the geometry");

        check(!as::read_mesh({}, mesh, "empty"), "an empty file is refused");
        check(!as::read_mesh(std::span{ good }.first(8), mesh, "short"),
              "a file too short for a header is refused");

        as::MeshHeader wrong = header;
        wrong.magic = 0;
        check(!as::read_mesh(make_mesh_file(wrong, vertices, indices, submeshes), mesh, "magic"),
              "a file that is not a cooked mesh is refused");

        wrong = header;
        wrong.version = as::kMeshVersion + 1;
        check(!as::read_mesh(make_mesh_file(wrong, vertices, indices, submeshes), mesh, "ver"),
              "a file from a later format version is refused");

        wrong = header;
        wrong.vertex_count = 0;
        check(!as::read_mesh(make_mesh_file(wrong, {}, indices, submeshes), mesh, "empty mesh"),
              "a mesh with no vertices is refused");

        // The two that matter most. Either one becomes a draw call that reads
        // past the end of a GPU buffer.
        const std::vector<std::uint32_t> past_the_end{ 0, 1, 3 };
        check(!as::read_mesh(make_mesh_file(header, vertices, past_the_end, submeshes), mesh,
                             "index"),
              "an index naming a vertex that is not there is refused");

        const std::vector<as::MeshSubmesh> too_long{
            as::MeshSubmesh{ .first_index = 0, .index_count = 4, .material = {} }
        };
        check(!as::read_mesh(make_mesh_file(header, vertices, indices, too_long), mesh, "run"),
              "a submesh running past the index list is refused");

        const std::vector<as::MeshSubmesh> empty_run{
            as::MeshSubmesh{ .first_index = 0, .index_count = 0, .material = {} }
        };
        check(!as::read_mesh(make_mesh_file(header, vertices, indices, empty_run), mesh, "none"),
              "a submesh covering no indices is refused");

        // The bounds decide culling in M5 and picking in #34. An inverted box
        // culls the mesh always or never, and a NaN corner makes the answer
        // undefined. Neither one names the mesh that caused it.
        wrong = header;
        wrong.min = { 1.0F, 0.0F, 0.0F };
        wrong.max = { -1.0F, 0.0F, 0.0F };
        check(!as::read_mesh(make_mesh_file(wrong, vertices, indices, submeshes), mesh, "flip"),
              "bounds whose low corner is above the high one are refused");

        wrong = header;
        wrong.max = { std::numeric_limits<float>::quiet_NaN(), 0.0F, 0.0F };
        check(!as::read_mesh(make_mesh_file(wrong, vertices, indices, submeshes), mesh, "nan"),
              "bounds holding a value that is not a number are refused");

        check(!as::read_mesh(std::span{ good }.first(good.size() - 4), mesh, "cut"),
              "a file shorter than its header claims is refused");
        std::vector<std::byte> longer = good;
        longer.push_back(std::byte{ 0 });
        check(!as::read_mesh(longer, mesh, "long"),
              "a file longer than its header claims is refused");
    }

    void test_the_cooker_gives_each_mesh_its_own_identity() {
        const std::filesystem::path source = scratch("ident/src");
        const std::filesystem::path out = scratch("ident/out");
        write_glb(source / "pair.glb", build_triangle(), 2);

        const cooker::Options options{ .content = source, .out = out };
        cooker::Result result;
        check(cooker::cook_all(options, result), "the cook works");
        check(result.cooked == 1, "one source cooked");

        as::Manifest manifest;
        check(as::load_manifest(out, manifest), "the manifest reads back");
        const as::ManifestEntry* entry = as::find_by_source(manifest, "pair.glb");
        check(entry != nullptr, "and it holds the glTF");
        check(entry != nullptr && entry->outputs.size() == 2,
              "a glTF with two meshes writes two outputs");

        if (entry != nullptr && entry->outputs.size() == 2) {
            const as::ManifestOutput& first = entry->outputs.at(0);
            const as::ManifestOutput& second = entry->outputs.at(1);
            check(first.guid != second.guid, "and each mesh has an identity of its own");
            check(first.guid != entry->guid, "neither of which is the source identity");
            check(first.guid == engine::Guid::derive(entry->guid, "mesh", 0),
                  "the first is derived from the source and the index");
            check(second.guid == engine::Guid::derive(entry->guid, "mesh", 1),
                  "and so is the second");

            // The lookup a prefab does. Naming the path cannot say which mesh,
            // and the GUID can.
            check(as::find_by_guid(manifest, second.guid) == &second,
                  "a mesh is findable by its own identity");
            check(as::find_source_of(manifest, second.guid) == entry,
                  "and its source is findable from it");
        }

        // A second cook must derive the same identities. They are not stored,
        // so a derivation that drifted would break every reference silently.
        cooker::Result again;
        check(cooker::cook_all(options, again), "a second cook works");
        check(again.skipped == 1, "and it skips the glTF");

        std::filesystem::remove_all(source.parent_path());
    }

    void test_the_separate_buffer_is_an_input() {
        const std::filesystem::path source = scratch("bin/src");
        const std::filesystem::path out = scratch("bin/out");
        write_gltf(source / "solo.gltf", build_triangle(), 1);

        const cooker::Options options{ .content = source, .out = out };
        cooker::Result first;
        check(cooker::cook_all(options, first), "the first cook works");

        as::Manifest manifest;
        check(as::load_manifest(out, manifest), "the manifest reads back");
        const as::ManifestEntry* entry = as::find_by_source(manifest, "solo.gltf");
        check(entry != nullptr, "and it holds the glTF");

        // The .gltf, its sidecar, and the .bin it names.
        bool names_the_bin = false;
        if (entry != nullptr) {
            for (const std::string& input : entry->inputs) {
                names_the_bin = names_the_bin || input == "solo.bin";
            }
        }
        check(names_the_bin, "a .gltf names its .bin as an input");

        // The .bin is glTF payload, not an asset. Cooking it would copy the
        // vertex data into the cooked tree a second time, where nothing reads
        // it. So the glTF is the only entry the tree produced.
        check(manifest.entries.size() == 1, "and the .bin is not an asset of its own");
        check(!std::filesystem::exists(out / "solo.bin"), "so nothing copied it through");

        cooker::Result second;
        check(cooker::cook_all(options, second), "the second cook works");
        check(second.skipped == 1, "and it cooks nothing");

        // Move a corner. The JSON does not change at all, so an entry that
        // hashed only the .gltf would call this fresh and ship the old mesh.
        Geometry moved = build_triangle();
        float wider = 9.0F;
        std::memcpy(moved.buffer.data() + (3 * sizeof(float)), &wider, sizeof(wider));
        write_bytes(source / "solo.bin", moved.buffer);

        cooker::Result third;
        check(cooker::cook_all(options, third), "the third cook works");
        check(third.cooked == 1, "a changed .bin cooks the glTF again");

        as::Mesh mesh;
        check(as::read_mesh(read_bytes(out / "solo.gltf.0.mesh"), mesh, "solo"),
              "the new mesh reads");
        check(mesh.max.x == 9.0F, "and it holds the geometry the .bin now has");

        std::filesystem::remove_all(source.parent_path());
    }

    void test_a_missing_part_cooks_again() {
        const std::filesystem::path source = scratch("part/src");
        const std::filesystem::path out = scratch("part/out");
        write_glb(source / "pair.glb", build_triangle(), 2);

        const cooker::Options options{ .content = source, .out = out };
        cooker::Result first;
        check(cooker::cook_all(options, first), "the first cook works");

        // Deleting one of the two outputs leaves the other one there and the
        // manifest unchanged. An entry that checked only its first output would
        // call this fresh and the second mesh would never come back.
        std::filesystem::remove(out / "pair.glb.1.mesh");
        cooker::Result second;
        check(cooker::cook_all(options, second), "the second cook works");
        check(second.cooked == 1, "one missing part cooks the source again");
        check(std::filesystem::exists(out / "pair.glb.1.mesh"), "and the part came back");

        std::filesystem::remove_all(source.parent_path());
    }

    /**
     * Writes a 32-bit uncompressed TGA, which stb_image reads.
     *
     * A TGA and not a PNG, because a PNG needs a deflate stream and the test
     * would then need a compressor to write one. The cooker reads both through
     * the same stb_image call, so the format proves the same thing.
     */
    /// The bytes of a 1 by 1 uncompressed true color TGA.
    std::vector<std::byte> tga_bytes() {
        // 18 bytes of header. Type 2 is uncompressed true color, 1 by 1, 32
        // bits, with the top-left origin flag so no row flip is needed. Then
        // one texel, in the blue-green-red-alpha order TGA stores.
        const std::array<std::uint8_t, 22> file{ 0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0,
                                                 0, 1, 0, 1, 0, 32, 0x28,
                                                 255, 0, 0, 255 };
        std::vector<std::byte> out(file.size());
        std::memcpy(out.data(), file.data(), file.size());
        return out;
    }

    void write_tga(const std::filesystem::path& path) { write_bytes(path, tga_bytes()); }

    /// Base64, so a test can put an image in a data URI the way a glTF does.
    std::string base64(std::span<const std::byte> bytes) {
        constexpr std::string_view kDigits =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        constexpr std::uint32_t kSixBits = 0x3FU;
        std::string out;
        for (std::size_t at = 0; at < bytes.size(); at += 3) {
            const std::size_t left = bytes.size() - at;
            std::uint32_t group = static_cast<std::uint32_t>(bytes[at]) << 16U;
            if (left > 1) {
                group |= static_cast<std::uint32_t>(bytes[at + 1]) << 8U;
            }
            if (left > 2) {
                group |= static_cast<std::uint32_t>(bytes[at + 2]);
            }
            out.push_back(kDigits[(group >> 18U) & kSixBits]);
            out.push_back(kDigits[(group >> 12U) & kSixBits]);
            out.push_back(left > 1 ? kDigits[(group >> 6U) & kSixBits] : '=');
            out.push_back(left > 2 ? kDigits[group & kSixBits] : '=');
        }
        return out;
    }

    /// Reads a cooked material file back.
    as::Material read_material_file(const std::filesystem::path& path) {
        as::Material material;
        check(as::read_material(read_bytes(path), material, path.string()),
              "the cooked material reads back");
        return material;
    }

    void test_a_material_names_its_textures_by_guid() {
        const std::filesystem::path source = scratch("mat/src");
        const std::filesystem::path out = scratch("mat/out");
        write_tga(source / "skin_BaseColor.tga");
        write_glb(source / "one.glb", build_triangle(), 1, "skin_BaseColor.tga");

        const cooker::Options options{ .content = source, .out = out };
        cooker::Result result;
        check(cooker::cook_all(options, result), "a glTF with a material cooks");

        as::Manifest manifest;
        check(as::load_manifest(out, manifest), "the manifest reads back");
        const as::ManifestEntry* entry = as::find_by_source(manifest, "one.glb");
        check(entry != nullptr, "and it holds the glTF");
        if (entry == nullptr) {
            return;
        }

        // One mesh and one material. The mesh comes first, because the cooker
        // checks the name of the first output to decide whether it may skip.
        check(entry->outputs.size() == 2, "one mesh and one material give two outputs");
        check(entry->outputs.at(0).cooked == "one.glb.0.mesh", "the mesh is first");
        check(entry->outputs.at(1).cooked == "one.glb.0.material", "and the material follows");
        check(entry->outputs.at(1).guid == engine::Guid::derive(entry->guid, "material", 0),
              "with an identity derived from the source and the index");
        check(entry->outputs.at(0).guid != entry->outputs.at(1).guid,
              "and a mesh and a material at the same index differ");

        // The identity in the material has to be the one the image sidecar
        // holds. Any other value points at nothing, and the surface would draw
        // white with no message saying why.
        as::AssetMeta image;
        check(as::load_meta(source / "skin_BaseColor.tga", image), "the image sidecar reads");

        const as::Material material = read_material_file(out / "one.glb.0.material");
        check(material.base_color == image.guid, "the base color names the image sidecar GUID");
        check(material.normal == image.guid, "and so does the normal map");
        check(!material.metallic_roughness.valid(),
              "a map the source leaves out stays the null GUID");
        check(material.base_color_factor == engine::Vec4(0.25F, 0.5F, 0.75F, 1.0F),
              "and the factors come across");
        check(material.metallic_factor == 0.125F, "including the metallic factor");
        check(material.double_sided, "and the double sided flag");

        // The submesh has to name the material, or nothing ties the two files
        // together and the renderer draws the fallback.
        as::Mesh mesh;
        check(as::read_mesh(read_bytes(out / "one.glb.0.mesh"), mesh, "one"),
              "the cooked mesh reads back");
        check(mesh.submeshes.size() == 1 &&
                  mesh.submeshes.at(0).material == entry->outputs.at(1).guid,
              "and its submesh names the material");

        std::filesystem::remove_all(source.parent_path());
    }

    void test_the_image_sidecar_is_an_input() {
        const std::filesystem::path source = scratch("input/src");
        const std::filesystem::path out = scratch("input/out");
        write_tga(source / "skin_BaseColor.tga");
        write_glb(source / "one.glb", build_triangle(), 1, "skin_BaseColor.tga");

        const cooker::Options options{ .content = source, .out = out };
        cooker::Result first;
        check(cooker::cook_all(options, first), "the first cook works");

        cooker::Result second;
        check(cooker::cook_all(options, second), "the second cook works");
        check(second.cooked == 0, "and nothing cooks twice");

        // Replace the image sidecar, which is what deleting one and cooking
        // again does. The glTF itself does not change at all, so an entry that
        // hashed only the .glb would keep a material pointing at a GUID that
        // no longer names anything.
        as::AssetMeta replaced;
        replaced.guid = engine::Guid::generate();
        check(as::save_meta(source / "skin_BaseColor.tga", replaced),
              "a new identity writes to the sidecar");

        cooker::Result third;
        check(cooker::cook_all(options, third), "the third cook works");
        check(third.cooked == 2, "a replaced image sidecar cooks the image and the glTF");

        const as::Material material = read_material_file(out / "one.glb.0.material");
        check(material.base_color == replaced.guid,
              "and the material now names the new identity");

        std::filesystem::remove_all(source.parent_path());
    }

    void test_the_gltf_rule_guesses_the_color_space() {
        const std::filesystem::path source = scratch("guess/src");
        const std::filesystem::path out = scratch("guess/out");

        // The glTF sorts before the image, so the glTF rule reaches the image
        // first and writes its sidecar. A rule that wrote the defaults there
        // would record sRGB, and every normal map in the model would read as
        // color from then on. Nothing about the result would look broken
        // enough to find.
        write_tga(source / "skin_Normal.tga");
        write_glb(source / "a.glb", build_triangle(), 1, "skin_Normal.tga");

        const cooker::Options options{ .content = source, .out = out };
        cooker::Result result;
        check(cooker::cook_all(options, result), "the cook works");

        as::AssetMeta image;
        check(as::load_meta(source / "skin_Normal.tga", image), "the image sidecar reads");
        check(image.texture.color_space == as::ColorSpace::Linear,
              "a normal map reads as linear even when the glTF rule wrote the sidecar");

        std::filesystem::remove_all(source.parent_path());
    }

    void test_an_escaped_uri_names_the_file_it_means() {
        const std::filesystem::path source = scratch("escape/src");
        const std::filesystem::path out = scratch("escape/out");

        // A URI escapes a space as %20, so the text is not a path until it is
        // decoded. A cooker that skipped the decode would look for a file
        // called "flight%20helmet.bin", which is not there, and the model
        // would fail to cook with a message naming a file nobody wrote.
        write_bytes(source / "flight helmet.bin", build_triangle().buffer);
        write_tga(source / "skin BaseColor.tga");
        const std::string json = geometry_json(build_triangle(), 1, "flight%20helmet.bin",
                                               "skin%20BaseColor.tga");
        write_bytes(source / "one.gltf", std::as_bytes(std::span{ json.data(), json.size() }));

        const cooker::Options options{ .content = source, .out = out };
        cooker::Result result;
        check(cooker::cook_all(options, result), "a glTF whose URIs hold an escaped space cooks");

        as::Manifest manifest;
        check(as::load_manifest(out, manifest), "the manifest reads back");
        const as::ManifestEntry* entry = as::find_by_source(manifest, "one.gltf");
        check(entry != nullptr, "and it holds the glTF");

        // The decoded name, not the escaped one. An input list holding
        // "flight%20helmet.bin" would name a file that is not there, and
        // hash_inputs would fail the cook rather than track the geometry.
        bool names_the_decoded_buffer = false;
        bool names_the_decoded_sidecar = false;
        if (entry != nullptr) {
            for (const std::string& input : entry->inputs) {
                names_the_decoded_buffer =
                    names_the_decoded_buffer || input == "flight helmet.bin";
                names_the_decoded_sidecar =
                    names_the_decoded_sidecar || input == "skin BaseColor.tga.meta";
            }
        }
        check(names_the_decoded_buffer, "the buffer input holds the decoded name");
        check(names_the_decoded_sidecar, "and so does the image sidecar input");

        // The material has to resolve too, through the same decode.
        as::AssetMeta image;
        check(as::load_meta(source / "skin BaseColor.tga", image), "the image sidecar reads");
        const as::Material material = read_material_file(out / "one.gltf.0.material");
        check(material.base_color == image.guid,
              "and the material names the image the escaped URI meant");

        std::filesystem::remove_all(source.parent_path());
    }

    void test_an_image_in_a_data_uri_gets_an_identity() {
        const std::filesystem::path source = scratch("inline/src");
        const std::filesystem::path out = scratch("inline/out");

        // A data URI carries the bytes inline, so there is no file and no
        // sidecar. The identity is derived from the parent instead, the way a
        // mesh and a material already are.
        write_bytes(source / "inline.bin", build_triangle().buffer);
        const std::string uri = "data:image/tga;base64," + base64(tga_bytes());
        const std::string json = geometry_json(build_triangle(), 1, "inline.bin", uri);
        write_bytes(source / "inline.gltf", std::as_bytes(std::span{ json.data(), json.size() }));

        const cooker::Options options{ .content = source, .out = out };
        cooker::Result result;
        check(cooker::cook_all(options, result), "a glTF whose image is a data URI cooks");

        as::Manifest manifest;
        check(as::load_manifest(out, manifest), "the manifest reads back");
        const as::ManifestEntry* entry = as::find_by_source(manifest, "inline.gltf");
        check(entry != nullptr, "and it holds the glTF");
        if (entry == nullptr) {
            return;
        }

        const engine::Guid wanted = engine::Guid::derive(entry->guid, "texture", 0);
        const as::Material material = read_material_file(out / "inline.gltf.0.material");
        check(material.base_color == wanted, "the material names the derived texture identity");
        check(as::find_by_guid(manifest, wanted) != nullptr,
              "and the manifest can find that texture");
        check(std::filesystem::exists(out / "inline.gltf.0.tex"),
              "and the cooked texture landed beside the mesh");

        // The cooked file has to be a real texture, not only a file of the
        // right name. read_texture is what the runtime calls.
        as::TextureView view;
        const std::vector<std::byte> bytes = read_bytes(out / "inline.gltf.0.tex");
        check(as::read_texture(bytes, view, "inline"), "the cooked texture reads back");
        check(view.width == 1 && view.height == 1, "and it holds the texel the data URI carried");

        // The data URI is not a path, so nothing may go on the input list for
        // it. An entry naming a file that is not there fails every later cook.
        bool names_a_data_uri = false;
        for (const std::string& input : entry->inputs) {
            names_a_data_uri = names_a_data_uri || input.find("data:") != std::string::npos;
        }
        check(!names_a_data_uri, "and no input names the data URI");

        cooker::Result second;
        check(cooker::cook_all(options, second), "a second cook works");
        check(second.skipped == 1, "and it skips the glTF");

        std::filesystem::remove_all(source.parent_path());
    }

    void test_an_image_in_a_glb_buffer_gets_an_identity() {
        const std::filesystem::path source = scratch("glbimg/src");
        const std::filesystem::path out = scratch("glbimg/out");

        // The form most exporters produce. The image sits in a buffer view
        // inside the .glb, so it has no URI at all.
        write_glb_with_image(source / "packed.glb", build_triangle(), tga_bytes());

        const cooker::Options options{ .content = source, .out = out };
        cooker::Result result;
        check(cooker::cook_all(options, result), "a .glb carrying its image cooks");

        as::Manifest manifest;
        check(as::load_manifest(out, manifest), "the manifest reads back");
        const as::ManifestEntry* entry = as::find_by_source(manifest, "packed.glb");
        check(entry != nullptr, "and it holds the glTF");
        if (entry == nullptr) {
            return;
        }

        const engine::Guid wanted = engine::Guid::derive(entry->guid, "texture", 0);
        const as::Material material = read_material_file(out / "packed.glb.0.material");
        check(material.base_color == wanted, "the material names the derived texture identity");

        as::TextureView view;
        const std::vector<std::byte> bytes = read_bytes(out / "packed.glb.0.tex");
        check(as::read_texture(bytes, view, "packed"), "the cooked texture reads back");

        // The base color slot holds color and the normal slot holds numbers.
        // An image inside the file has no name to guess from, so the slot is
        // the only thing that can say. Reading a base color as linear washes
        // it out everywhere, and it is the mistake nobody traces to the import.
        check(view.color_space == as::ColorSpace::Srgb,
              "and a base color slot makes it read as sRGB");

        std::filesystem::remove_all(source.parent_path());
    }

    void test_an_inline_normal_map_reads_as_linear() {
        const std::filesystem::path source = scratch("linear/src");
        const std::filesystem::path out = scratch("linear/out");

        // The same bytes in the normal slot alone. Nothing about the image
        // says which it is, so the slot has to decide.
        write_glb_with_image(source / "packed.glb", build_triangle(), tga_bytes(),
                             ImageSlot::Normal);

        const cooker::Options options{ .content = source, .out = out };
        cooker::Result result;
        check(cooker::cook_all(options, result), "it cooks");

        as::TextureView view;
        check(as::read_texture(read_bytes(out / "packed.glb.0.tex"), view, "packed"),
              "the cooked texture reads back");
        check(view.color_space == as::ColorSpace::Linear,
              "an image used only as a normal map reads as linear");

        std::filesystem::remove_all(source.parent_path());
    }

    void test_an_inline_image_in_both_kinds_of_slot_reads_as_color() {
        const std::filesystem::path source = scratch("both/src");
        const std::filesystem::path out = scratch("both/out");

        // One image in a color slot and a data slot at once. That is a broken
        // model, and it still has to cook one way rather than depend on which
        // slot the reader looked at first.
        write_glb_with_image(source / "packed.glb", build_triangle(), tga_bytes(),
                             ImageSlot::Both);

        const cooker::Options options{ .content = source, .out = out };
        cooker::Result result;
        check(cooker::cook_all(options, result), "it cooks");

        as::TextureView view;
        check(as::read_texture(read_bytes(out / "packed.glb.0.tex"), view, "packed"),
              "the cooked texture reads back");

        // Color wins. Reading color as linear washes it out everywhere, and
        // that is the failure a person notices. Reading a normal map as color
        // is wrong too, but it is the quieter of the two.
        check(view.color_space == as::ColorSpace::Srgb,
              "an image used as both color and data reads as sRGB");

        std::filesystem::remove_all(source.parent_path());
    }

    void test_a_material_naming_a_missing_image_fails() {
        const std::filesystem::path source = scratch("missing/src");
        const std::filesystem::path out = scratch("missing/out");
        write_glb(source / "one.glb", build_triangle(), 1, "not_there.tga");

        // A model that names an image nobody shipped is a broken model. Cooking
        // it anyway would give a material with a null texture, and the surface
        // would draw white with nothing in the log naming the file.
        const cooker::Options options{ .content = source, .out = out };
        cooker::Result result;
        check(!cooker::cook_all(options, result), "a material naming a missing image fails");
        check(result.failed == 1, "and the glTF is the source that failed");

        std::filesystem::remove_all(source.parent_path());
    }

} // namespace

int main() {
    test::section("importing");
    test_glb_cooks();
    test_gltf_with_a_separate_buffer_cooks();
    test_tangents_are_built_when_the_source_has_none();
    test_the_optimizer_keeps_the_geometry();
    test_a_broken_gltf_fails();
    test::section("the cooked mesh format");
    test_read_mesh_refuses_a_bad_file();
    test::section("sub-asset identities");
    test_the_cooker_gives_each_mesh_its_own_identity();
    test_the_separate_buffer_is_an_input();
    test_a_missing_part_cooks_again();
    test::section("materials");
    test_a_material_names_its_textures_by_guid();
    test_the_image_sidecar_is_an_input();
    test_the_gltf_rule_guesses_the_color_space();
    test_an_escaped_uri_names_the_file_it_means();
    test_a_material_naming_a_missing_image_fails();
    test::section("images with no file of their own");
    test_an_image_in_a_data_uri_gets_an_identity();
    test_an_image_in_a_glb_buffer_gets_an_identity();
    test_an_inline_normal_map_reads_as_linear();
    test_an_inline_image_in_both_kinds_of_slot_reads_as_color();
    return test::report();
}
