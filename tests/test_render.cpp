// M4.5 tests for the render caches, and M5.2 tests for the material block.
//
// A material that was injected rather than built owns nothing on the device, so
// its drop is testable without a GPU. Packing the parameter block needs no
// device at all. The other two caches need either a headless device or a seam.
// Issue #62 records that decision.

#include "check.h"
#include "render/material_cache.h"
#include "render/mesh_pass.h"

#include <cstdlib>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

    using test::check;
    using engine::render::MaterialCache;
    using engine::render::GpuMaterial;
    using engine::render::MaterialMap;
    using engine::render::check_material_block;
    using engine::render::material_uniform_layout;
    using engine::render::pack_material_uniforms;

    /// A shader that declares exactly what MaterialUniforms expects.
    [[nodiscard]] engine::assets::Shader matching_shader() {
        engine::assets::Shader shader;
        shader.stage = engine::assets::ShaderStage::Fragment;
        for (const auto& member : material_uniform_layout()) {
            engine::assets::ShaderParam param;
            param.name = member.name;
            param.set = engine::render::kMaterialSet;
            param.binding = 5;
            param.offset = member.offset;
            param.type = member.type;
            shader.params.push_back(std::move(param));
        }
        return shader;
    }

    /// Finds one param of a shader by name, so a test can spoil just that one.
    [[nodiscard]] engine::assets::ShaderParam& param_named(engine::assets::Shader& shader,
                                                           std::string_view name) {
        for (engine::assets::ShaderParam& param : shader.params) {
            if (param.name == name) {
                return param;
            }
        }
        std::abort();
    }

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

    void test_a_shader_that_agrees_passes() {
        check(check_material_block(matching_shader(), "test"),
              "a shader that declares the block passes");

        // The real check has to say yes to the real shader, and a table with no
        // entries would say yes to anything. This is what makes the refusals
        // below mean something.
        check(!material_uniform_layout().empty(), "the expected layout is not empty");
        check(material_uniform_layout().size() == 10,
              "every member of the block is expected");
    }

    void test_a_renamed_member_is_refused() {
        engine::assets::Shader shader = matching_shader();
        param_named(shader, "normal_scale").name = "normal_scale_typo";
        check(!check_material_block(shader, "test"), "a renamed member is refused");
    }

    void test_a_moved_member_is_refused() {
        // Two members of the same type that traded places. Nothing about the
        // block size changes, and the shader still compiles, so this is the case
        // that no other check can catch.
        engine::assets::Shader shader = matching_shader();
        std::swap(param_named(shader, "metallic_factor").offset,
                  param_named(shader, "roughness_factor").offset);
        check(!check_material_block(shader, "test"), "two members that swapped are refused");
    }

    void test_a_retyped_member_is_refused() {
        engine::assets::Shader shader = matching_shader();
        param_named(shader, "alpha_mode").type = engine::assets::ParamType::Float;
        check(!check_material_block(shader, "test"), "a member of the wrong type is refused");
    }

    void test_a_missing_member_is_refused() {
        engine::assets::Shader shader = matching_shader();
        shader.params.clear();
        check(!check_material_block(shader, "test"), "a shader that declares nothing is refused");
    }

    void test_a_member_in_another_set_does_not_count() {
        // The frame block declares its own members, and one of them could share
        // a name with a material one. Matching on the name alone would find it.
        engine::assets::Shader shader = matching_shader();
        param_named(shader, "has_maps").set = 0;
        check(!check_material_block(shader, "test"),
              "a member in the frame set does not satisfy the material block");
    }

    // ---- Which compiled form of mesh.frag a material needs ----

    using engine::render::kMeshVariantCount;
    using engine::render::mesh_variant_defines;
    using engine::render::mesh_variant_index;
    using engine::render::pick_shader_variant;

    /// A cooked form that was compiled with @p defines.
    [[nodiscard]] engine::assets::Shader form_with(std::vector<std::string> defines) {
        engine::assets::Shader shader;
        shader.defines = std::move(defines);
        return shader;
    }

    void test_the_variant_index_reads_only_the_two_maps_it_compiles_out() {
        const auto bit = [](MaterialMap map) { return static_cast<std::uint32_t>(map); };
        check(mesh_variant_index(0) == 0, "a material with no maps takes the base form");
        check(mesh_variant_index(bit(MaterialMap::Normal)) == 1, "a normal map takes form 1");
        check(mesh_variant_index(bit(MaterialMap::Occlusion)) == 2, "an occlusion map takes form 2");
        check(mesh_variant_index(bit(MaterialMap::Normal) | bit(MaterialMap::Occlusion)) == 3,
              "both take form 3");

        // The other three maps are read with no branch, so they must not move
        // the index. A material with all of them and neither of the two the
        // shader compiles out still wants the base form.
        const std::uint32_t others = bit(MaterialMap::BaseColor) |
                                     bit(MaterialMap::MetallicRoughness) |
                                     bit(MaterialMap::Emissive);
        check(mesh_variant_index(others) == 0, "the maps that never branch do not pick a form");
        check(mesh_variant_index(others | bit(MaterialMap::Normal)) == 1,
              "and they do not disturb the ones that do");
    }

    void test_every_index_names_defines_that_match_it() {
        for (std::size_t at = 0; at < kMeshVariantCount; ++at) {
            const auto defines = mesh_variant_defines(at);
            // The count of defines is the count of set bits in the index, so
            // the table and the index cannot drift apart.
            const std::size_t bits = ((at & 1U) != 0 ? 1U : 0U) + ((at & 2U) != 0 ? 1U : 0U);
            check(defines.size() == bits, "the form names one define for each bit of its index");
        }
        check(mesh_variant_defines(0).empty(), "the base form defines nothing");
    }

    void test_a_form_is_picked_by_what_it_declares() {
        std::vector<engine::assets::Shader> forms;
        forms.push_back(form_with({}));
        forms.push_back(form_with({ "HAS_NORMAL_MAP" }));
        forms.push_back(form_with({ "HAS_OCCLUSION_MAP" }));
        forms.push_back(form_with({ "HAS_NORMAL_MAP", "HAS_OCCLUSION_MAP" }));

        for (std::size_t at = 0; at < kMeshVariantCount; ++at) {
            const engine::assets::Shader* found =
                pick_shader_variant(forms, mesh_variant_defines(at));
            check(found == &forms[at], "each index finds the form built for it");
        }
    }

    void test_a_form_is_found_whatever_order_it_lists_its_defines() {
        // The cooker passes the sidecar list to glslc as it stands, so a person
        // may write the two defines either way round. Matching on the sequence
        // would then miss the form and the pass would refuse to build.
        std::vector<engine::assets::Shader> forms;
        forms.push_back(form_with({ "HAS_OCCLUSION_MAP", "HAS_NORMAL_MAP" }));
        check(pick_shader_variant(forms, mesh_variant_defines(3)) == forms.data(),
              "the order the module lists its defines in does not matter");
    }

    void test_a_form_with_more_defines_is_not_a_substitute() {
        // A form built with the normal map compiled in shades differently, so
        // it cannot stand in for the base form. Matching on "contains all of
        // what was asked" rather than on the exact set would return it.
        std::vector<engine::assets::Shader> forms;
        forms.push_back(form_with({ "HAS_NORMAL_MAP" }));
        check(pick_shader_variant(forms, mesh_variant_defines(0)) == nullptr,
              "a form that defines more than was asked for is refused");
    }

    void test_a_missing_form_reports_rather_than_guesses() {
        std::vector<engine::assets::Shader> forms;
        forms.push_back(form_with({}));
        check(pick_shader_variant(forms, mesh_variant_defines(3)) == nullptr,
              "a sidecar that lists no such variant gives no form");
    }

} // namespace

int main() {
    test::section("MaterialCache");
    test_material_cache_drop();
    test::section("the material parameter block");
    test_the_map_mask_names_only_the_maps_that_are_there();
    test_every_factor_reaches_the_block();
    test::section("the block and the shader agree");
    test_a_shader_that_agrees_passes();
    test_a_renamed_member_is_refused();
    test_a_moved_member_is_refused();
    test_a_retyped_member_is_refused();
    test_a_missing_member_is_refused();
    test_a_member_in_another_set_does_not_count();
    test::section("picking a compiled form of mesh.frag");
    test_the_variant_index_reads_only_the_two_maps_it_compiles_out();
    test_every_index_names_defines_that_match_it();
    test_a_form_is_picked_by_what_it_declares();
    test_a_form_is_found_whatever_order_it_lists_its_defines();
    test_a_form_with_more_defines_is_not_a_substitute();
    test_a_missing_form_reports_rather_than_guesses();
    return test::report();
}
