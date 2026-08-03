#pragma once

/**
 * @file
 * @brief The cooked mesh format, shared by the cooker and the runtime.
 *
 * A cooked mesh is one file: a header, then the vertices, then the indices,
 * then the submeshes. `tools/cooker/mesh.cpp` writes it from a glTF file, and
 * nothing else writes it. This header holds only what both sides must agree
 * on, so a change here is a format change and it moves the format version.
 *
 * The vertices are interleaved rather than held as one stream for each
 * attribute. One stream suits the forward pass the engine draws today, and it
 * costs one bind rather than four. A depth prepass or a shadow pass reads
 * position alone and would rather have it separate, so M5 is the milestone
 * that may split this. The format version is how that arrives.
 */

#include "core/guid.h"
#include "math/conventions.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace engine::assets {

    /// @brief The name a cooked mesh file carries after the source name.
    inline constexpr const char* kMeshExtension = ".mesh";

    /**
     * @brief The first four bytes of a cooked mesh file.
     *
     * The value spells "CMSH" when a person opens the file in a hex viewer.
     */
    inline constexpr std::uint32_t kMeshMagic = 0x48534D43U;

    /// @brief The format version this build writes and reads.
    inline constexpr std::uint32_t kMeshVersion = 1;

    /**
     * @brief One vertex, in the layout the vertex buffer holds.
     *
     * The tangent carries the bitangent sign in `w`, which is what glTF
     * supplies and what a normal map needs. A mesh whose source has no tangent
     * gets one the cooker generated.
     */
    struct MeshVertex {
        std::array<float, 3> position{}; ///< In meters, in the mesh local space.
        std::array<float, 3> normal{};   ///< Unit length.
        std::array<float, 4> tangent{};  ///< xyz unit length, w is +1 or -1.
        std::array<float, 2> uv{};       ///< Texture coordinate. The origin is top-left.
    };

    /// @brief How many bytes one vertex takes. The vertex buffer stride.
    inline constexpr std::size_t kMeshVertexSize = 48;
    static_assert(sizeof(MeshVertex) == kMeshVertexSize,
                  "The cooked layout must match the struct, because the file is "
                  "written and read as raw bytes.");

    /**
     * @brief One run of indices that shares a material.
     *
     * A glTF mesh holds a primitive for each material it uses, and each one
     * becomes a submesh here. The draw call count follows this list.
     */
    struct MeshSubmesh {
        std::uint32_t first_index = 0; ///< Where this run starts in the index list.
        std::uint32_t index_count = 0; ///< How many indices the run holds.
        Guid material;                 ///< The material asset. Null until M4.4b writes one.
    };

    /**
     * @brief The fixed-size header at the start of a cooked mesh file.
     *
     * The size is a multiple of 8, so the vertices that follow it start at an
     * alignment every member of MeshVertex accepts.
     */
    struct MeshHeader {
        std::uint32_t magic = kMeshMagic;     ///< ::kMeshMagic. Checked first.
        std::uint32_t version = kMeshVersion; ///< ::kMeshVersion when written.
        std::uint32_t vertex_count = 0;       ///< How many vertices follow the header.
        std::uint32_t index_count = 0;        ///< How many 32-bit indices follow those.
        std::uint32_t submesh_count = 0;      ///< How many submeshes follow those.
        std::uint32_t reserved = 0;           ///< Zero. It keeps this header 48 bytes.
        std::array<float, 3> min{};           ///< The smallest corner of the bounds.
        std::array<float, 3> max{};           ///< The largest corner of the bounds.
    };

    /// @brief How many bytes the header takes.
    inline constexpr std::size_t kMeshHeaderSize = 48;
    static_assert(sizeof(MeshHeader) == kMeshHeaderSize,
                  "The header is written and read as raw bytes, so its size is "
                  "part of the file format.");

    /**
     * @brief A cooked mesh, read into memory.
     *
     * This owns its data rather than pointing into the file bytes. A cooked
     * texture hands the device a pointer into the buffer it read, because the
     * texels go to the GPU exactly as they sit on disk. A mesh cannot do the
     * same. The vertices start 48 bytes into the file, and nothing promises the
     * caller's buffer puts that offset where a float may be read. `Content` made
     * the same call for the same reason, in `read_words`.
     *
     * The caller may free the file bytes as soon as this returns.
     */
    struct Mesh {
        std::vector<MeshVertex> vertices;   ///< Every vertex, in one stream.
        std::vector<std::uint32_t> indices; ///< Every index, for every submesh.
        std::vector<MeshSubmesh> submeshes; ///< One for each material the mesh uses.
        Vec3 min{ 0.0F };                   ///< The smallest corner of the bounds.
        Vec3 max{ 0.0F };                   ///< The largest corner of the bounds.
    };

    /**
     * @brief How many bytes a cooked mesh of this size takes, after the header.
     * @param vertex_count How many vertices the mesh holds.
     * @param index_count How many indices the mesh holds.
     * @param submesh_count How many submeshes the mesh holds.
     * @return The size of the payload, in bytes.
     */
    [[nodiscard]] std::size_t mesh_payload_bytes(std::uint32_t vertex_count,
                                                 std::uint32_t index_count,
                                                 std::uint32_t submesh_count);

    /**
     * @brief Reads a cooked mesh and points at its vertices and its indices.
     *
     * This checks the magic, the version, and that the file is exactly the size
     * the counts call for. It also checks that every submesh names a run inside
     * the index list, and that every index names a vertex that is there. A draw
     * call built from either would read past the end of a GPU buffer, and the
     * message that comes back names nothing useful.
     *
     * @param bytes The whole file, as read from disk.
     * @param out The mesh to fill. It copies, so @p bytes may go away after.
     * @param where A name for the log, usually the asset path.
     * @return True when the file is a cooked mesh this build understands.
     */
    [[nodiscard]] bool read_mesh(std::span<const std::byte> bytes, Mesh& out,
                                 std::string_view where);

} // namespace engine::assets
