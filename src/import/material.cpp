#include "import/material.h"

#include "assets/material.h"
#include "assets/meta.h"
#include "core/log.h"
#include "import/mesh.h"
#include "import/texture.h"

#include <cgltf.h>

#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace engine::import {

    namespace {

        namespace as = engine::assets;

        /// Which image a texture points at, or none when it points at nothing.
        [[nodiscard]] bool image_index(const cgltf_data& data, const cgltf_texture* texture,
                                       std::size_t& out) {
            if (texture == nullptr || texture->image == nullptr || data.images == nullptr) {
                return false;
            }
            const auto index = static_cast<std::size_t>(texture->image - data.images);
            if (index >= data.images_count) {
                return false;
            }
            out = index;
            return true;
        }

        /**
         * Works out how each image a material uses has to be read.
         *
         * An image inside a glTF has no file name to guess from, so the guess
         * comes from the slot instead. A base color and an emissive map hold
         * color. A normal, a metallic-roughness, and an occlusion map hold
         * numbers.
         *
         * An image used in both kinds of slot is a broken model. Color wins,
         * because reading color as linear washes it out everywhere and it is
         * the failure a person notices.
         */
        [[nodiscard]] std::map<std::size_t, as::ColorSpace> image_uses(const cgltf_data& data) {
            std::map<std::size_t, as::ColorSpace> out;

            const auto mark = [&](const cgltf_texture* texture, as::ColorSpace space) {
                std::size_t index = 0;
                if (!image_index(data, texture, index)) {
                    return;
                }
                const auto found = out.find(index);
                if (found == out.end()) {
                    out.emplace(index, space);
                } else if (space == as::ColorSpace::Srgb) {
                    found->second = space;
                }
            };

            for (cgltf_size at = 0; at < data.materials_count; ++at) {
                const cgltf_material& material = data.materials[at];
                if (material.has_pbr_metallic_roughness != 0) {
                    mark(material.pbr_metallic_roughness.base_color_texture.texture,
                         as::ColorSpace::Srgb);
                    mark(material.pbr_metallic_roughness.metallic_roughness_texture.texture,
                         as::ColorSpace::Linear);
                }
                mark(material.emissive_texture.texture, as::ColorSpace::Srgb);
                mark(material.normal_texture.texture, as::ColorSpace::Linear);
                mark(material.occlusion_texture.texture, as::ColorSpace::Linear);
            }
            return out;
        }

        /**
         * The encoded bytes of an image a glTF carries inside itself.
         *
         * Two forms arrive here. A buffer view points into the buffer that
         * cgltf_load_buffers() already loaded, which is what a `.glb` uses. A
         * data URI carries base64 in the JSON, which cgltf does not decode for
         * an image the way it does for a buffer.
         */
        [[nodiscard]] bool inline_image_bytes(const cgltf_image& image, std::string_view where,
                                              std::vector<std::byte>& out) {
            if (image.buffer_view != nullptr) {
                const cgltf_buffer_view& view = *image.buffer_view;
                if (view.buffer == nullptr || view.buffer->data == nullptr) {
                    ENGINE_LOG_ERROR("{}: its buffer did not load.", where);
                    return false;
                }
                const auto* first = static_cast<const std::byte*>(view.buffer->data);
                out.assign(first + view.offset, first + view.offset + view.size);
                return true;
            }

            const char* uri = image.uri;
            if (uri == nullptr) {
                ENGINE_LOG_ERROR("{}: it names no buffer view and no URI.", where);
                return false;
            }

            const std::string_view text{ uri };
            const std::size_t comma = text.find("base64,");
            if (comma == std::string_view::npos) {
                ENGINE_LOG_ERROR("{}: its data URI is not base64, and this reads no other "
                                 "encoding.",
                                 where);
                return false;
            }
            const std::string_view encoded = text.substr(comma + std::strlen("base64,"));

            // Four base64 characters carry three bytes, and a trailing = stands
            // for a byte that is not there.
            constexpr std::size_t kGroup = 4;
            constexpr std::size_t kCarried = 3;
            if (encoded.size() % kGroup != 0 || encoded.empty()) {
                ENGINE_LOG_ERROR("{}: its base64 is not a whole number of groups.", where);
                return false;
            }
            std::size_t padding = 0;
            while (padding < kCarried && encoded.size() > padding &&
                   encoded[encoded.size() - 1 - padding] == '=') {
                ++padding;
            }
            const std::size_t size = ((encoded.size() / kGroup) * kCarried) - padding;

            // cgltf allocates the decoded bytes and hands them over, so this
            // has to free them with whatever allocated them. Both hooks are set
            // rather than left zero, because cgltf falls back to its own
            // default for the allocation and leaves the free hook null. Calling
            // that null pointer is a crash, and using std::free against an
            // allocator this file did not choose is worse.
            cgltf_options options{};
            options.memory.alloc_func = [](void*, cgltf_size bytes) -> void* {
                return std::malloc(bytes);
            };
            options.memory.free_func = [](void*, void* held) { std::free(held); };

            void* decoded = nullptr;
            if (cgltf_load_buffer_base64(&options, size, encoded.data(), &decoded) !=
                    cgltf_result_success ||
                decoded == nullptr) {
                ENGINE_LOG_ERROR("{}: its base64 will not decode.", where);
                return false;
            }

            // The assign below can throw, so the free goes through a guard.
            const std::unique_ptr<void, decltype(&std::free)> held{ decoded, &std::free };
            const auto* first = static_cast<const std::byte*>(decoded);
            out.assign(first, first + size);
            return true;
        }

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
        [[nodiscard]] bool texture_guid(const cgltf_data& data, const cgltf_texture* texture,
                                        const std::filesystem::path& directory,
                                        const InlineImages& images, std::string_view where,
                                        engine::Guid& out) {
            out = engine::Guid{};
            if (texture == nullptr || texture->image == nullptr) {
                return true;
            }

            // A GLB carries its images inside the file, and a data URI carries
            // them inline. Neither one is a file, so neither one can carry a
            // sidecar. cook_inline_images() gave those a derived identity, so
            // the lookup is by index rather than by path.
            std::filesystem::path image;
            if (!gltf_uri_path(texture->image->uri, directory, image)) {
                std::size_t index = 0;
                if (image_index(data, texture, index)) {
                    if (const auto found = images.guids.find(index);
                        found != images.guids.end()) {
                        out = found->second;
                        return true;
                    }
                }
                ENGINE_LOG_ERROR("{}: an image inside the file was not cooked, so the material "
                                 "can name no texture.",
                                 where);
                return false;
            }

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
         *
         * The name says glTF because `assets::read_material` reads a cooked
         * one. The two do opposite things, and an unqualified call here would
         * find both by argument-dependent lookup.
         */
        [[nodiscard]] bool read_gltf_material(const cgltf_data& data,
                                              const cgltf_material& source,
                                              const std::filesystem::path& directory,
                                              const InlineImages& images, std::string_view where,
                                              as::Material& out) {
            const auto resolve = [&](const cgltf_texture* texture, engine::Guid& guid) {
                return texture_guid(data, texture, directory, images, where, guid);
            };

            if (source.has_pbr_metallic_roughness != 0) {
                const cgltf_pbr_metallic_roughness& pbr = source.pbr_metallic_roughness;
                if (!resolve(pbr.base_color_texture.texture, out.base_color) ||
                    !resolve(pbr.metallic_roughness_texture.texture, out.metallic_roughness)) {
                    return false;
                }
                out.base_color_factor = factor4(pbr.base_color_factor);
                out.metallic_factor = pbr.metallic_factor;
                out.roughness_factor = pbr.roughness_factor;
            }

            if (!resolve(source.normal_texture.texture, out.normal) ||
                !resolve(source.occlusion_texture.texture, out.occlusion) ||
                !resolve(source.emissive_texture.texture, out.emissive)) {
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

    bool cook_inline_images(const cgltf_data& data, const std::filesystem::path& source,
                            const std::filesystem::path& out_root,
                            const std::filesystem::path& relative, engine::Guid parent,
                            InlineImages& out) {
        const std::filesystem::path directory = source.parent_path();
        const std::map<std::size_t, as::ColorSpace> uses = image_uses(data);

        for (const auto& [index, space] : uses) {
            const cgltf_image& image = data.images[index];

            // An image with a URI that names a file is a real asset. It has a
            // sidecar, and the texture rule cooks it.
            std::filesystem::path named;
            if (gltf_uri_path(image.uri, directory, named)) {
                continue;
            }

            const std::string where = source.string() + " image " + std::to_string(index);

            std::vector<std::byte> bytes;
            if (!inline_image_bytes(image, where, bytes)) {
                return false;
            }

            // No sidecar, so nothing can override these. The color space comes
            // from the slot the material used the image in.
            const as::TextureImport settings{ .color_space = space };

            std::filesystem::path cooked = relative;
            cooked += "." + std::to_string(index);
            cooked += as::kTextureExtension;

            if (!cook_texture_bytes(bytes, out_root / cooked, settings, where)) {
                return false;
            }

            const engine::Guid guid =
                engine::Guid::derive(parent, as::kTexturePartKind, static_cast<std::uint32_t>(index));
            out.guids.emplace(index, guid);
            out.outputs.push_back(
                as::ManifestOutput{ .cooked = as::manifest_path(cooked), .guid = guid });

            ENGINE_LOG_INFO("{}: it has no file, so it cooked as {} and reads as {}.", where,
                            guid.to_text(), as::to_text(space));
        }
        return true;
    }

    bool cook_materials(const cgltf_data& data, const std::filesystem::path& source,
                        const std::filesystem::path& out_root,
                        const std::filesystem::path& relative, engine::Guid parent,
                        const InlineImages& images, CookedMaterials& out) {
        const std::filesystem::path directory = source.parent_path();

        for (cgltf_size at = 0; at < data.materials_count; ++at) {
            const std::string where = source.string() + " material " + std::to_string(at);

            as::Material material;
            if (!read_gltf_material(data, data.materials[at], directory, images, where,
                                    material)) {
                return false;
            }

            std::filesystem::path cooked = relative;
            cooked += "." + std::to_string(at);
            cooked += as::kMaterialExtension;

            if (!write_material(out_root / cooked, material)) {
                return false;
            }

            const engine::Guid guid =
                engine::Guid::derive(parent, as::kMaterialPartKind, static_cast<std::uint32_t>(at));
            out.guids.push_back(guid);
            out.outputs.push_back(
                as::ManifestOutput{ .cooked = as::manifest_path(cooked), .guid = guid });
        }
        return true;
    }

} // namespace engine::import
