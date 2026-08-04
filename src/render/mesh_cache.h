#pragma once

/**
 * @file
 * @brief Cooked meshes, uploaded once and drawn many times.
 *
 * A scene names a mesh by GUID, and several entities name the same one. This
 * turns that GUID into a pair of GPU buffers, and it uploads each mesh once
 * however many entities draw it.
 *
 * The cache owns the buffers rather than `assets::AssetDatabase`, because a GPU
 * buffer needs the device to free it and the database frees a value by running
 * its destructor. M4.5 brings the two together, when hot reload needs to
 * replace a mesh while the program runs.
 */

#include "assets/content.h"
#include "assets/mesh.h"
#include "core/guid.h"
#include "gfx/device.h"
#include "math/conventions.h"

#include <cstdint>
#include <map>
#include <vector>

namespace engine::render {

    /// @brief One cooked mesh, on the GPU and ready to draw.
    struct GpuMesh {
        gfx::BufferHandle vertices;                 ///< Every vertex, in one stream.
        gfx::BufferHandle indices;                  ///< Every index, for every submesh.
        std::vector<assets::MeshSubmesh> submeshes; ///< One draw call for each.
        Vec3 min{ 0.0F };                           ///< The smallest corner of the bounds.
        Vec3 max{ 0.0F };                           ///< The largest corner of the bounds.
    };

    /**
     * @brief Holds every mesh the scene asked for.
     *
     * @code
     * engine::render::MeshCache cache;
     * const engine::render::GpuMesh* mesh = cache.get(device, content, guid);
     * @endcode
     */
    class MeshCache {
    public:
        /// @brief Frees nothing. Call destroy() before this runs.
        ~MeshCache();

        MeshCache() = default;
        MeshCache(const MeshCache&) = delete;
        MeshCache& operator=(const MeshCache&) = delete;
        MeshCache(MeshCache&&) = delete;
        MeshCache& operator=(MeshCache&&) = delete;

        /**
         * @brief Finds a mesh, and uploads it the first time it is asked for.
         *
         * A GUID that will not load is remembered as a failure, so a scene that
         * names a missing mesh reports once rather than on every frame.
         *
         * @param device The device that owns the buffers.
         * @param content The cooked content to read from.
         * @param guid The mesh identity, as a scene stores it.
         * @return The mesh, or nullptr when it will not load.
         */
        [[nodiscard]] const GpuMesh* get(gfx::Device* device, const assets::Content& content,
                                         Guid guid);

        /**
         * @brief Frees one mesh, so the next get() reads it again.
         *
         * This is what hot reload calls. It also forgets a remembered failure,
         * because the whole point of a reload is that a mesh which would not
         * load before may load now.
         *
         * @param device The device that owns the buffers.
         * @param guid The mesh to let go of. One that is not loaded is not an
         * error and does nothing.
         *
         * @warning The buffers go straight back to the device. The caller must
         * already have waited for the frames in flight, because a frame the GPU
         * has not finished may still read them.
         */
        void drop(gfx::Device* device, Guid guid);

        /**
         * @brief Frees every buffer.
         * @param device The device that owns them.
         */
        void destroy(gfx::Device* device);

        /// @brief How many meshes are loaded.
        /// @return The count, failures not included.
        [[nodiscard]] std::size_t size() const { return loaded_.size(); }

    private:
        std::map<Guid, GpuMesh> loaded_;
        /// The GUIDs that failed, so one bad reference reports once.
        std::map<Guid, bool> failed_;
    };

} // namespace engine::render
