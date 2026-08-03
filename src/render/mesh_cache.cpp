#include "render/mesh_cache.h"

#include "core/assert.h"
#include "core/log.h"

#include <cstddef>
#include <utility>

namespace engine::render {

    namespace {

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
            return true;
        }

    } // namespace

    MeshCache::~MeshCache() {
        ENGINE_ASSERT(loaded_.empty(), "MeshCache::destroy was not called, so GPU buffers leaked.");
    }

    const GpuMesh* MeshCache::get(gfx::Device* device, const assets::Content& content, Guid guid) {
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
        if (!content.read_bytes(guid, bytes) ||
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
