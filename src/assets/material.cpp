#include "assets/material.h"

#include "core/log.h"

#include <cmath>
#include <cstring>

namespace engine::assets {

    namespace {

        /**
         * Whether every number in a header is a number.
         *
         * A factor that is NaN multiplies a surface into nothing, and one that
         * is infinite blows it out to white. Neither failure names the material
         * that caused it, and both survive every other check here, because a
         * NaN is a valid bit pattern of the right size.
         */
        [[nodiscard]] bool numbers_are_finite(const MaterialHeader& header) {
            for (const float value : header.base_color_factor) {
                if (!std::isfinite(value)) {
                    return false;
                }
            }
            for (const float value : header.emissive_factor) {
                if (!std::isfinite(value)) {
                    return false;
                }
            }
            return std::isfinite(header.metallic_factor) &&
                   std::isfinite(header.roughness_factor) && std::isfinite(header.normal_scale) &&
                   std::isfinite(header.occlusion_strength) && std::isfinite(header.alpha_cutoff);
        }

    } // namespace

    MaterialHeader pack_material(const Material& material) {
        MaterialHeader header;
        header.base_color = material.base_color;
        header.metallic_roughness = material.metallic_roughness;
        header.normal = material.normal;
        header.occlusion = material.occlusion;
        header.emissive = material.emissive;
        header.base_color_factor = { material.base_color_factor.x, material.base_color_factor.y,
                                     material.base_color_factor.z, material.base_color_factor.w };
        header.emissive_factor = { material.emissive_factor.x, material.emissive_factor.y,
                                   material.emissive_factor.z };
        header.metallic_factor = material.metallic_factor;
        header.roughness_factor = material.roughness_factor;
        header.normal_scale = material.normal_scale;
        header.occlusion_strength = material.occlusion_strength;
        header.alpha_cutoff = material.alpha_cutoff;
        header.alpha_mode = static_cast<std::uint32_t>(material.alpha_mode);
        header.double_sided = material.double_sided ? 1U : 0U;
        return header;
    }

    bool read_material(std::span<const std::byte> bytes, Material& out, std::string_view where) {
        // The header is the whole file, so one size check covers both a file
        // that ends early and a file that carries something extra.
        if (bytes.size() != kMaterialSize) {
            ENGINE_LOG_ERROR("{}: a cooked material is {} bytes and this file holds {}.", where,
                             kMaterialSize, bytes.size());
            return false;
        }

        // A copy, not a cast. The file may sit at any alignment in the caller's
        // buffer, and reading a struct through a misaligned pointer is undefined.
        MaterialHeader header{};
        std::memcpy(&header, bytes.data(), sizeof(header));

        if (header.magic != kMaterialMagic) {
            ENGINE_LOG_ERROR("{}: not a cooked material. Cook the content tree again.", where);
            return false;
        }
        if (header.version != kMaterialVersion) {
            ENGINE_LOG_ERROR("{}: written by version {} and this build reads version {}. "
                             "Cook the content tree again.",
                             where, header.version, kMaterialVersion);
            return false;
        }
        if (header.alpha_mode > kAlphaModeMax) {
            ENGINE_LOG_ERROR("{}: it names alpha mode {} and this build knows {} of them.",
                             where, header.alpha_mode, kAlphaModeMax + 1);
            return false;
        }
        if (!numbers_are_finite(header)) {
            ENGINE_LOG_ERROR("{}: one of its factors is not a number.", where);
            return false;
        }

        out.base_color = header.base_color;
        out.metallic_roughness = header.metallic_roughness;
        out.normal = header.normal;
        out.occlusion = header.occlusion;
        out.emissive = header.emissive;
        out.base_color_factor =
            Vec4{ header.base_color_factor[0], header.base_color_factor[1],
                  header.base_color_factor[2], header.base_color_factor[3] };
        out.emissive_factor = Vec3{ header.emissive_factor[0], header.emissive_factor[1],
                                    header.emissive_factor[2] };
        out.metallic_factor = header.metallic_factor;
        out.roughness_factor = header.roughness_factor;
        out.normal_scale = header.normal_scale;
        out.occlusion_strength = header.occlusion_strength;
        out.alpha_cutoff = header.alpha_cutoff;
        out.alpha_mode = static_cast<AlphaMode>(header.alpha_mode);
        out.double_sided = header.double_sided != 0;
        return true;
    }

} // namespace engine::assets
