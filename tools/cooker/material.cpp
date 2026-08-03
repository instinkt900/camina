#include "material.h"

#include "assets/material.h"
#include "assets/meta.h"
#include "core/log.h"
#include "texture.h"

#include <cgltf.h>

#include <cstring>
#include <fstream>
#include <string>
#include <string_view>

namespace cooker {

    namespace {

        namespace as = engine::assets;

        /**
         * Turns the texture a glTF material names into the GUID of its image.
         *
         * The identity comes from the image sidecar rather than from anything
         * this file writes, because the image is a real asset that the texture
         * rule cooks on its own. Reading it here is what ties the two together.
         *
         * A texture this cannot resolve leaves @p out null and reports success.
         * The material still cooks, and the renderer draws it with the fallback
         * texture. Failing the whole model over one missing map would be worse.
         */
        [[nodiscard]] bool texture_guid(const cgltf_texture* texture,
                                        const std::filesystem::path& directory,
                                        std::string_view where, engine::Guid& out) {
            out = engine::Guid{};
            if (texture == nullptr || texture->image == nullptr) {
                return true;
            }

            const char* uri = texture->image->uri;
            if (uri == nullptr) {
                // A GLB carries its images inside the file, and a data URI
                // carries them inline. Neither one is a file with a sidecar, so
                // neither one has an identity this pipeline can store.
                ENGINE_LOG_WARN("{}: an image lives inside the file, so it has no identity "
                                "yet and the material names no texture.",
                                where);
                return true;
            }
            if (std::string_view{ uri }.starts_with("data:")) {
                ENGINE_LOG_WARN("{}: an image arrives as a data URI, so it has no identity "
                                "yet and the material names no texture.",
                                where);
                return true;
            }

            // A URI escapes a space as %20 and so on, so the text is not a path
            // until it is decoded. cgltf decodes in place, so this works on a
            // copy rather than on the parsed data.
            std::string decoded{ uri };
            cgltf_decode_uri(decoded.data());
            decoded.resize(std::strlen(decoded.c_str()));

            const std::filesystem::path image = directory / decoded;
            std::error_code error;
            if (!std::filesystem::is_regular_file(image, error)) {
                ENGINE_LOG_ERROR("{}: it names the image {}, and there is no such file.", where,
                                 image.string());
                return false;
            }

            as::AssetMeta meta;
            if (!image_meta(image, meta)) {
                return false;
            }
            out = meta.guid;
            return true;
        }

        /**
         * Copies a glTF factor into a vector.
         *
         * cgltf holds one as a plain C array, and the caller passes the first
         * element. A reference to an array would say the length in the type,
         * and the engine style forbids a C array in a declaration.
         */
        [[nodiscard]] engine::Vec4 factor4(const cgltf_float* value) {
            return engine::Vec4{ value[0], value[1], value[2], value[3] };
        }

        /// The same, for the three-component factors.
        [[nodiscard]] engine::Vec3 factor3(const cgltf_float* value) {
            return engine::Vec3{ value[0], value[1], value[2] };
        }

        /// The ::AlphaMode that matches what the glTF says.
        [[nodiscard]] as::AlphaMode alpha_mode_of(cgltf_alpha_mode mode) {
            switch (mode) {
            case cgltf_alpha_mode_mask:
                return as::AlphaMode::Mask;
            case cgltf_alpha_mode_blend:
                return as::AlphaMode::Blend;
            default:
                return as::AlphaMode::Opaque;
            }
        }

        /**
         * Reads one glTF material.
         *
         * Only the metallic-roughness model is read. glTF makes that the base
         * model and every other one an extension, so a material with no
         * `pbrMetallicRoughness` block is legal and rare. Such a material takes
         * the defaults, which give a white surface.
         */
        [[nodiscard]] bool read_material(const cgltf_material& source,
                                         const std::filesystem::path& directory,
                                         std::string_view where, as::Material& out) {
            if (source.has_pbr_metallic_roughness != 0) {
                const cgltf_pbr_metallic_roughness& pbr = source.pbr_metallic_roughness;
                if (!texture_guid(pbr.base_color_texture.texture, directory, where,
                                  out.base_color) ||
                    !texture_guid(pbr.metallic_roughness_texture.texture, directory, where,
                                  out.metallic_roughness)) {
                    return false;
                }
                out.base_color_factor = factor4(pbr.base_color_factor);
                out.metallic_factor = pbr.metallic_factor;
                out.roughness_factor = pbr.roughness_factor;
            }

            if (!texture_guid(source.normal_texture.texture, directory, where, out.normal) ||
                !texture_guid(source.occlusion_texture.texture, directory, where,
                              out.occlusion) ||
                !texture_guid(source.emissive_texture.texture, directory, where, out.emissive)) {
                return false;
            }

            out.normal_scale = source.normal_texture.scale;
            out.occlusion_strength = source.occlusion_texture.scale;
            out.emissive_factor = factor3(source.emissive_factor);
            out.alpha_mode = alpha_mode_of(source.alpha_mode);
            out.alpha_cutoff = source.alpha_cutoff;
            out.double_sided = source.double_sided != 0;
            return true;
        }

        [[nodiscard]] bool write_material(const std::filesystem::path& destination,
                                          const as::Material& material) {
            std::error_code error;
            std::filesystem::create_directories(destination.parent_path(), error);

            std::ofstream file(destination, std::ios::binary | std::ios::trunc);
            if (!file) {
                ENGINE_LOG_ERROR("{}: could not open it for writing.", destination.string());
                return false;
            }

            const as::MaterialHeader header = as::pack_material(material);
            file.write(reinterpret_cast<const char*>(&header),
                       static_cast<std::streamsize>(sizeof(header)));
            if (!file) {
                ENGINE_LOG_ERROR("{}: the write failed part way through.", destination.string());
                return false;
            }
            return true;
        }

    } // namespace

    bool cook_materials(const cgltf_data& data, const std::filesystem::path& source,
                        const std::filesystem::path& out_root,
                        const std::filesystem::path& relative, engine::Guid parent,
                        CookedMaterials& out) {
        const std::filesystem::path directory = source.parent_path();

        for (cgltf_size at = 0; at < data.materials_count; ++at) {
            const std::string where = source.string() + " material " + std::to_string(at);

            as::Material material;
            if (!read_material(data.materials[at], directory, where, material)) {
                return false;
            }

            std::filesystem::path cooked = relative;
            cooked += "." + std::to_string(at);
            cooked += as::kMaterialExtension;

            if (!write_material(out_root / cooked, material)) {
                return false;
            }

            const engine::Guid guid =
                engine::Guid::derive(parent, kMaterialPartKind, static_cast<std::uint32_t>(at));
            out.guids.push_back(guid);
            out.outputs.push_back(
                as::ManifestOutput{ .cooked = as::manifest_path(cooked), .guid = guid });
        }
        return true;
    }

} // namespace cooker
