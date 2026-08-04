// M4.5 tests for the render caches, and M5.2 tests for the material block.
//
// A material that was injected rather than built owns nothing on the device, so
// its drop is testable without a GPU. Packing the parameter block needs no
// device at all. The other two caches need either a headless device or a seam.
// Issue #62 records that decision.

#include "check.h"
#include "render/material_cache.h"

#include <utility>

namespace {

    using test::check;
    using engine::render::MaterialCache;
    using engine::render::GpuMaterial;
    using engine::render::MaterialMap;
    using engine::render::pack_material_uniforms;

    void test_material_cache_drop() {
        MaterialCache cache;
        check(cache.size() == 0, "starts empty");

        const engine::Guid mat_a = engine::Guid::generate();
        const engine::Guid mat_b = engine::Guid::generate();
        const engine::Guid mat_c = engine::Guid::generate();
        const engine::Guid tex_x = engine::Guid::generate();
        const engine::Guid tex_y = engine::Guid::generate();

        // A null device throughout. These materials were injected rather than
        // built, so they own no buffer and no descriptor set, and drop() has
        // nothing to free. That keeps the test free of a graphics device.
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
        cache.drop(nullptr, tex_x);
        check(cache.size() == 1, "only one left after dropping tex_x");
        check(!cache.has(mat_a), "mat_a was dropped");
        check(cache.has(mat_b), "mat_b stays");
        check(!cache.has(mat_c), "mat_c was dropped");

        // Dropping a GUID nothing knows is harmless.
        cache.drop(nullptr, engine::Guid::generate());
        check(cache.size() == 1, "dropping an unknown GUID changes nothing");
        check(cache.has(mat_b), "mat_b is still there");

        // Dropping the material's own identity works the same way.
        cache.drop(nullptr, mat_b);
        check(cache.size() == 0, "dropping mat_b leaves nothing");
        check(!cache.has(mat_b), "mat_b is gone");
    }

    [[nodiscard]] std::uint32_t bit(MaterialMap which) {
        return static_cast<std::uint32_t>(which);
    }

    /**
     * The mask says which maps the material really named.
     *
     * A slot with no texture binds the fallback white texel, which is the right
     * default for a base color and the wrong one for a normal map. So the shader
     * tests these bits, and a bit set for a map that is not there tilts every
     * normal towards the same direction with nothing to say why.
     */
    void test_the_map_mask_names_only_the_maps_that_are_there() {
        engine::assets::Material material;
        check(pack_material_uniforms(material).has_maps == 0,
              "a material that names no texture sets no bit");

        material.base_color = engine::Guid::generate();
        material.normal = engine::Guid::generate();
        const std::uint32_t mask = pack_material_uniforms(material).has_maps;
        check((mask & bit(MaterialMap::BaseColor)) != 0, "the base color bit is set");
        check((mask & bit(MaterialMap::Normal)) != 0, "the normal bit is set");
        check((mask & bit(MaterialMap::MetallicRoughness)) == 0,
              "the metallic-roughness bit stays clear");
        check((mask & bit(MaterialMap::Occlusion)) == 0, "the occlusion bit stays clear");
        check((mask & bit(MaterialMap::Emissive)) == 0, "the emissive bit stays clear");

        material.metallic_roughness = engine::Guid::generate();
        material.occlusion = engine::Guid::generate();
        material.emissive = engine::Guid::generate();
        const std::uint32_t all = pack_material_uniforms(material).has_maps;
        check(all == (bit(MaterialMap::BaseColor) | bit(MaterialMap::MetallicRoughness) |
                      bit(MaterialMap::Normal) | bit(MaterialMap::Occlusion) |
                      bit(MaterialMap::Emissive)),
              "a material that names all five sets all five bits");

        // Every bit is its own, so no two maps share one.
        check(bit(MaterialMap::BaseColor) != bit(MaterialMap::MetallicRoughness) &&
                  bit(MaterialMap::Normal) != bit(MaterialMap::Occlusion) &&
                  bit(MaterialMap::Occlusion) != bit(MaterialMap::Emissive),
              "no two maps share a bit");
    }

    /// Every factor has to reach the block, because the shader multiplies by it.
    void test_every_factor_reaches_the_block() {
        engine::assets::Material material;
        material.base_color_factor = engine::Vec4{ 0.1F, 0.2F, 0.3F, 0.4F };
        material.emissive_factor = engine::Vec3{ 0.5F, 0.6F, 0.7F };
        material.metallic_factor = 0.25F;
        material.roughness_factor = 0.75F;
        material.normal_scale = 2.0F;
        material.occlusion_strength = 0.5F;
        material.alpha_cutoff = 0.125F;
        material.alpha_mode = engine::assets::AlphaMode::Mask;

        const auto block = pack_material_uniforms(material);
        check(block.base_color_factor[0] == 0.1F && block.base_color_factor[3] == 0.4F,
              "the base color factor survives, alpha included");
        check(block.emissive_factor[0] == 0.5F && block.emissive_factor[2] == 0.7F,
              "the emissive factor survives");
        // The fourth component is padding the shader never reads, and a written
        // zero is easier to trust than whatever the stack held.
        check(block.emissive_factor[3] == 0.0F, "its padding word is zero");
        check(block.metallic_factor == 0.25F, "the metallic factor survives");
        check(block.roughness_factor == 0.75F, "the roughness factor survives");
        check(block.normal_scale == 2.0F, "the normal scale survives");
        check(block.occlusion_strength == 0.5F, "the occlusion strength survives");
        check(block.alpha_cutoff == 0.125F, "the alpha cutoff survives");
        check(block.alpha_mode == static_cast<std::uint32_t>(engine::assets::AlphaMode::Mask),
              "the alpha mode survives as its number");
    }

} // namespace

int main() {
    test::section("MaterialCache");
    test_material_cache_drop();
    test::section("the material parameter block");
    test_the_map_mask_names_only_the_maps_that_are_there();
    test_every_factor_reaches_the_block();
    return test::report();
}
