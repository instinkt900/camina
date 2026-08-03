#include "assets/mesh.h"

#include "core/log.h"

#include <cstring>

namespace engine::assets {

    namespace {

        /// The byte offset of the index list, which follows the vertices.
        [[nodiscard]] std::size_t index_offset(std::uint32_t vertex_count) {
            return kMeshHeaderSize + (static_cast<std::size_t>(vertex_count) * kMeshVertexSize);
        }

        /// The byte offset of the submesh list, which follows the indices.
        [[nodiscard]] std::size_t submesh_offset(std::uint32_t vertex_count,
                                                 std::uint32_t index_count) {
            return index_offset(vertex_count) +
                   (static_cast<std::size_t>(index_count) * sizeof(std::uint32_t));
        }

    } // namespace

    std::size_t mesh_payload_bytes(std::uint32_t vertex_count, std::uint32_t index_count,
                                   std::uint32_t submesh_count) {
        return (static_cast<std::size_t>(vertex_count) * kMeshVertexSize) +
               (static_cast<std::size_t>(index_count) * sizeof(std::uint32_t)) +
               (static_cast<std::size_t>(submesh_count) * sizeof(MeshSubmesh));
    }

    bool read_mesh(std::span<const std::byte> bytes, Mesh& out, std::string_view where) {
        if (bytes.size() < kMeshHeaderSize) {
            ENGINE_LOG_ERROR("{}: too short to be a cooked mesh. It holds {} bytes.", where,
                             bytes.size());
            return false;
        }

        // A copy, not a cast. The file may sit at any alignment in the caller's
        // buffer, and reading a struct through a misaligned pointer is undefined.
        MeshHeader header{};
        std::memcpy(&header, bytes.data(), sizeof(header));

        if (header.magic != kMeshMagic) {
            ENGINE_LOG_ERROR("{}: not a cooked mesh. Cook the content tree again.", where);
            return false;
        }
        if (header.version != kMeshVersion) {
            ENGINE_LOG_ERROR("{}: written by version {} and this build reads version {}. "
                             "Cook the content tree again.",
                             where, header.version, kMeshVersion);
            return false;
        }
        if (header.vertex_count == 0 || header.index_count == 0 || header.submesh_count == 0) {
            ENGINE_LOG_ERROR("{}: the header describes an empty mesh. It says {} vertices, "
                             "{} indices, and {} submeshes.",
                             where, header.vertex_count, header.index_count,
                             header.submesh_count);
            return false;
        }

        const std::size_t wanted =
            kMeshHeaderSize + mesh_payload_bytes(header.vertex_count, header.index_count,
                                                 header.submesh_count);
        if (bytes.size() != wanted) {
            ENGINE_LOG_ERROR("{}: the header calls for {} bytes and the file holds {}.", where,
                             wanted, bytes.size());
            return false;
        }

        // Copy the submeshes rather than pointing at them. They sit after the
        // index list, which is a whole number of 4-byte words, so the offset can
        // land 4 bytes off the 8 that the GUID inside a submesh wants.
        std::vector<MeshSubmesh> submeshes(header.submesh_count);
        std::memcpy(submeshes.data(), bytes.data() + submesh_offset(header.vertex_count, header.index_count),
                    static_cast<std::size_t>(header.submesh_count) * sizeof(MeshSubmesh));

        // A submesh that names a run outside the index list becomes a draw call
        // that reads past the end of a GPU buffer.
        for (std::size_t at = 0; at < submeshes.size(); ++at) {
            const MeshSubmesh& submesh = submeshes[at];
            const std::size_t last =
                static_cast<std::size_t>(submesh.first_index) + submesh.index_count;
            if (submesh.index_count == 0 || last > header.index_count) {
                ENGINE_LOG_ERROR("{}: submesh {} names indices {} to {}, and the mesh holds {}.",
                                 where, at, submesh.first_index, last, header.index_count);
                return false;
            }
        }

        std::vector<MeshVertex> vertices(header.vertex_count);
        std::memcpy(vertices.data(), bytes.data() + kMeshHeaderSize,
                    static_cast<std::size_t>(header.vertex_count) * kMeshVertexSize);

        std::vector<std::uint32_t> indices(header.index_count);
        std::memcpy(indices.data(), bytes.data() + index_offset(header.vertex_count),
                    static_cast<std::size_t>(header.index_count) * sizeof(std::uint32_t));

        // An index that names a vertex that is not there becomes a draw call
        // reading past the end of a GPU buffer. The message that comes back
        // from that names nothing useful, so the check belongs here.
        for (std::uint32_t at = 0; at < header.index_count; ++at) {
            if (indices[at] >= header.vertex_count) {
                ENGINE_LOG_ERROR("{}: index {} names vertex {}, and the mesh holds {}.", where,
                                 at, indices[at], header.vertex_count);
                return false;
            }
        }

        // The bounds get the same treatment as the counts above. #34 picks an
        // entity with this box and M5 culls with it. An inverted box culls the
        // mesh always or never, and a NaN corner makes the answer undefined.
        // Neither failure names the mesh that caused it.
        //
        // The comparison is written as !(min <= max) rather than as min > max,
        // because every comparison against NaN is false. The negated form
        // therefore rejects NaN and the plain form would let it through.
        for (std::size_t axis = 0; axis < 3; ++axis) {
            if (!(header.min[axis] <= header.max[axis])) {
                ENGINE_LOG_ERROR("{}: the bounds on axis {} run from {} to {}.", where, axis,
                                 header.min[axis], header.max[axis]);
                return false;
            }
        }

        out.vertices = std::move(vertices);
        out.indices = std::move(indices);
        out.submeshes = std::move(submeshes);
        out.min = Vec3{ header.min[0], header.min[1], header.min[2] };
        out.max = Vec3{ header.max[0], header.max[1], header.max[2] };
        return true;
    }

} // namespace engine::assets
