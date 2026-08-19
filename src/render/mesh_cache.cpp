#include "render/mesh_cache.h"

#include "core/assert.h"
#include "core/log.h"

#include <cstddef>
#include <utility>

namespace engine::render {

    namespace {

        /// Computes per-submesh bounds from the vertex and index data.
        void compute_submesh_bounds(const assets::Mesh& mesh, GpuMesh& out) {
            out.submesh_min.resize(mesh.submeshes.size());
            out.submesh_max.resize(mesh.submeshes.size());

            for (std::size_t s = 0; s < mesh.submeshes.size(); ++s) {
                const assets::MeshSubmesh& sub = mesh.submeshes[s];
                Vec3 smin{ std::numeric_limits<float>::max() };
                Vec3 smax{ std::numeric_limits<float>::lowest() };

                for (std::uint32_t i = 0; i < sub.index_count; ++i) {
                    const std::uint32_t idx =
                        mesh.indices[static_cast<std::size_t>(sub.first_index) + i];
                    const auto& pos_arr = mesh.vertices[idx].position;
                    const Vec3 pos{ pos_arr[0], pos_arr[1], pos_arr[2] };
                    smin.x = std::min(smin.x, pos.x);
                    smin.y = std::min(smin.y, pos.y);
                    smin.z = std::min(smin.z, pos.z);
                    smax.x = std::max(smax.x, pos.x);
                    smax.y = std::max(smax.y, pos.y);
                    smax.z = std::max(smax.z, pos.z);
                }

                out.submesh_min[s] = smin;
                out.submesh_max[s] = smax;
            }
        }

        /// Uploads one cooked mesh and fills in the handles.
        [[nodiscard]] bool upload(gfx::Device* device, const assets::Mesh& mesh, GpuMesh& out) {
            const gfx::BufferDesc vertices{
                .data = mesh.vertices.data(),
                .size = mesh.vertices.size() * sizeof(assets::MeshVertex),
                .usage = gfx::BufferUsage::Vertex,
            };
            gfx::Result result = gfx::create_buffer(device, vertices, &out.vertices);
            if (!gfx::succeeded(result)) {
                ENGINE_LOG_ERROR("A mesh vertex buffer failed: {}", gfx::result_name(result));
                return false;
            }

            const gfx::BufferDesc indices{
                .data = mesh.indices.data(),
                .size = mesh.indices.size() * sizeof(std::uint32_t),
                .usage = gfx::BufferUsage::Index,
            };
            result = gfx::create_buffer(device, indices, &out.indices);
            if (!gfx::succeeded(result)) {
                ENGINE_LOG_ERROR("A mesh index buffer failed: {}", gfx::result_name(result));
                // The vertex buffer is live and the mesh is not going in the
                // cache, so nothing would ever free it.
                gfx::destroy_buffer(device, out.vertices);
                out.vertices = gfx::BufferHandle{};
                return false;
            }

            out.submeshes = mesh.submeshes;
            out.min = mesh.min;
            out.max = mesh.max;
            compute_submesh_bounds(mesh, out);
            return true;
        }

    } // namespace

    MeshCache::~MeshCache() {
        ENGINE_ASSERT(loaded_.empty(), "MeshCache::destroy was not called, so GPU buffers leaked.");
    }

    const GpuMesh* MeshCache::get(gfx::Device* device, const assets::AssetSource& content, Guid guid) {
        if (const auto found = loaded_.find(guid); found != loaded_.end()) {
            return &found->second;
        }
        // A scene that names a mesh it does not have would otherwise report on
        // every frame, and the log would say nothing else.
        if (failed_.contains(guid)) {
            return nullptr;
        }

        std::vector<std::byte> bytes;
        assets::Mesh mesh;
        if (!content.read(guid, bytes) ||
            !assets::read_mesh(bytes, mesh, guid.to_text())) {
            failed_.emplace(guid, true);
            return nullptr;
        }

        GpuMesh built;
        if (!upload(device, mesh, built)) {
            ENGINE_LOG_ERROR("{} would not upload.", guid.to_text());
            failed_.emplace(guid, true);
            return nullptr;
        }

        ENGINE_LOG_INFO("Uploaded mesh {} with {} vertices and {} submeshes.", guid.to_text(),
                        mesh.vertices.size(), built.submeshes.size());
        return &loaded_.emplace(guid, std::move(built)).first->second;
    }

    void MeshCache::drop(gfx::Device* device, Guid guid) {
        failed_.erase(guid);

        const auto found = loaded_.find(guid);
        if (found == loaded_.end()) {
            return;
        }
        if (device != nullptr) {
            gfx::destroy_buffer(device, found->second.vertices);
            gfx::destroy_buffer(device, found->second.indices);
        }
        loaded_.erase(found);
    }

    void MeshCache::destroy(gfx::Device* device) {
        if (device == nullptr) {
            loaded_.clear();
            failed_.clear();
            return;
        }
        for (auto& [guid, mesh] : loaded_) {
            gfx::destroy_buffer(device, mesh.indices);
            gfx::destroy_buffer(device, mesh.vertices);
        }
        loaded_.clear();
        failed_.clear();
    }

} // namespace engine::render
