// M4.5 tests for the render caches.
//
// MaterialCache owns nothing on the device and takes a device only to pass
// it along, so its drop is testable without a GPU. The other two caches
// need either a headless device or a seam. Issue #62 records that decision.

#include "check.h"
#include "render/material_cache.h"

#include <utility>

namespace {

    using test::check;
    using engine::render::MaterialCache;
    using engine::render::GpuMaterial;

    void test_material_cache_drop() {
        MaterialCache cache;
        check(cache.size() == 0, "starts empty");

        const engine::Guid mat_a = engine::Guid::generate();
        const engine::Guid mat_b = engine::Guid::generate();
        const engine::Guid mat_c = engine::Guid::generate();
        const engine::Guid tex_x = engine::Guid::generate();
        const engine::Guid tex_y = engine::Guid::generate();

        GpuMaterial a;
        a.source.base_color = tex_x;

        GpuMaterial b;
        b.source.base_color = tex_y;

        GpuMaterial c;
        c.source.base_color = tex_x;

        cache.inject(mat_a, std::move(a));
        cache.inject(mat_b, std::move(b));
        cache.inject(mat_c, std::move(c));
        check(cache.size() == 3, "three materials loaded");

        // Dropping a texture identity removes every material that names
        // it. Materials A and C name tex_x, so both go. Material B
        // names tex_y, so it stays.
        cache.drop(tex_x);
        check(cache.size() == 1, "only one left after dropping tex_x");
        check(!cache.has(mat_a), "mat_a was dropped");
        check(cache.has(mat_b), "mat_b stays");
        check(!cache.has(mat_c), "mat_c was dropped");

        // Dropping a GUID nothing knows is harmless.
        cache.drop(engine::Guid::generate());
        check(cache.size() == 1, "dropping an unknown GUID changes nothing");
        check(cache.has(mat_b), "mat_b is still there");

        // Dropping the material's own identity works the same way.
        cache.drop(mat_b);
        check(cache.size() == 0, "dropping mat_b leaves nothing");
        check(!cache.has(mat_b), "mat_b is gone");
    }

} // namespace

int main() {
    test::section("MaterialCache");
    test_material_cache_drop();
    return test::report();
}
