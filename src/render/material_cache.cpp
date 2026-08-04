#include "render/material_cache.h"

#include "core/log.h"

#include <array>
#include <cstddef>
#include <utility>
#include <vector>

namespace engine::render {

    namespace {

        /// Sets a bit in the map mask when the material named that texture.
        [[nodiscard]] std::uint32_t map_bit(Guid guid, MaterialMap which) {
            return guid.valid() ? static_cast<std::uint32_t>(which) : 0U;
        }

    } // namespace

    MaterialUniforms pack_material_uniforms(const assets::Material& material) {
        MaterialUniforms out;
        out.base_color_factor = { material.base_color_factor.x, material.base_color_factor.y,
                                  material.base_color_factor.z, material.base_color_factor.w };
        out.emissive_factor = { material.emissive_factor.x, material.emissive_factor.y,
                                material.emissive_factor.z, 0.0F };
        out.metallic_factor = material.metallic_factor;
        out.roughness_factor = material.roughness_factor;
        out.normal_scale = material.normal_scale;
        out.occlusion_strength = material.occlusion_strength;
        out.alpha_cutoff = material.alpha_cutoff;
        out.alpha_mode = static_cast<std::uint32_t>(material.alpha_mode);
        out.has_maps = map_bit(material.base_color, MaterialMap::BaseColor) |
                       map_bit(material.metallic_roughness, MaterialMap::MetallicRoughness) |
                       map_bit(material.normal, MaterialMap::Normal) |
                       map_bit(material.occlusion, MaterialMap::Occlusion) |
                       map_bit(material.emissive, MaterialMap::Emissive);
        return out;
    }

    bool MaterialCache::build(gfx::Device* device, gfx::PipelineHandle pipeline,
                              const assets::Material& material, GpuMaterial& out) {
        const MaterialUniforms uniforms = pack_material_uniforms(material);
        const gfx::BufferDesc desc{ .data = &uniforms,
                                    .size = sizeof(uniforms),
                                    .usage = gfx::BufferUsage::Uniform };
        if (!gfx::succeeded(gfx::create_buffer(device, desc, &out.uniforms))) {
            ENGINE_LOG_ERROR("A material parameter block could not be uploaded.");
            return false;
        }

        // The order matches the bindings in mesh.frag. A slot the material left
        // empty resolved to the fallback texel above, so every binding is filled
        // and the shader needs no branch to read one.
        const auto sampler = [](std::uint32_t binding, gfx::TextureHandle texture) {
            return gfx::DescriptorWrite{ .binding = binding,
                                         .kind = gfx::DescriptorKind::CombinedImageSampler,
                                         .texture = texture,
                                         .buffer = {} };
        };
        const std::array<gfx::DescriptorWrite, 6> writes{ {
            sampler(0, out.base_color),
            sampler(1, out.metallic_roughness),
            sampler(2, out.normal),
            sampler(3, out.occlusion),
            sampler(4, out.emissive),
            gfx::DescriptorWrite{ .binding = 5,
                                  .kind = gfx::DescriptorKind::UniformBuffer,
                                  .texture = {},
                                  .buffer = out.uniforms },
        } };

        if (!gfx::succeeded(gfx::create_descriptor_set(device, pipeline, kMaterialSet,
                                                       writes.data(), writes.size(), &out.set))) {
            gfx::destroy_buffer(device, out.uniforms);
            out.uniforms = gfx::BufferHandle{};
            return false;
        }
        return true;
    }

    void MaterialCache::release(gfx::Device* device, GpuMaterial& material) {
        // The set first, because it names the buffer.
        gfx::destroy_descriptor_set(device, material.set);
        gfx::destroy_buffer(device, material.uniforms);
        material.set = gfx::DescriptorSetHandle{};
        material.uniforms = gfx::BufferHandle{};
    }

    const GpuMaterial& MaterialCache::get(gfx::Device* device, const assets::Content& content,
                                          TextureCache& textures, gfx::PipelineHandle pipeline,
                                          Guid guid) {
        if (guid.valid()) {
            if (const auto found = loaded_.find(guid); found != loaded_.end()) {
                return found->second;
            }
        }

        // A submesh with no material, and one whose material will not load, both
        // draw with this. It is built once and then reused, because it owns a
        // descriptor set like any other material.
        if (!fallback_.set.valid()) {
            const gfx::TextureHandle texel = textures.fallback();
            fallback_.base_color = texel;
            fallback_.metallic_roughness = texel;
            fallback_.normal = texel;
            fallback_.occlusion = texel;
            fallback_.emissive = texel;
            // The defaults of assets::Material, which are the glTF ones, with no
            // texture named. So has_maps is zero and every factor applies alone.
            if (!build(device, pipeline, fallback_.source, fallback_)) {
                ENGINE_LOG_ERROR("The fallback material could not be built.");
            }
        }

        if (!guid.valid()) {
            return fallback_;
        }
        // A submesh that names a material it does not have would otherwise
        // report on every frame, and the log would say nothing else.
        if (failed_.contains(guid)) {
            return fallback_;
        }

        std::vector<std::byte> bytes;
        assets::Material material;
        if (!content.read_bytes(guid, bytes) ||
            !assets::read_material(bytes, material, guid.to_text())) {
            failed_.emplace(guid, true);
            return fallback_;
        }

        GpuMaterial built{
            .source = material,
            .base_color = textures.get(device, content, material.base_color),
            .metallic_roughness = textures.get(device, content, material.metallic_roughness),
            .normal = textures.get(device, content, material.normal),
            .occlusion = textures.get(device, content, material.occlusion),
            .emissive = textures.get(device, content, material.emissive),
            .uniforms = {},
            .set = {},
        };

        if (!build(device, pipeline, material, built)) {
            failed_.emplace(guid, true);
            return fallback_;
        }

        ENGINE_LOG_INFO("Read material {}.", guid.to_text());
        return loaded_.emplace(guid, std::move(built)).first->second;
    }

    void MaterialCache::drop(gfx::Device* device, Guid guid) {
        failed_.erase(guid);

        // A material holds the texture handles it resolved, and a reloaded
        // texture is a new handle. Every material that names this identity has
        // to read it again, or it binds a texture the device already freed. The
        // descriptor set names those textures, so it goes with them.
        std::erase_if(loaded_, [device, guid](auto& entry) {
            const assets::Material& material = entry.second.source;
            const bool names_it = entry.first == guid || material.base_color == guid ||
                                  material.metallic_roughness == guid ||
                                  material.normal == guid || material.occlusion == guid ||
                                  material.emissive == guid;
            if (names_it) {
                release(device, entry.second);
            }
            return names_it;
        });
    }

    void MaterialCache::destroy(gfx::Device* device) {
        for (auto& entry : loaded_) {
            release(device, entry.second);
        }
        loaded_.clear();
        failed_.clear();
        release(device, fallback_);
        fallback_ = GpuMaterial{};
    }

} // namespace engine::render
