#include "render/shader_bindings.h"

#include "core/log.h"

#include <algorithm>

namespace engine::render {

    bool read_one_shader(const assets::AssetSource& content, std::string_view source,
                         assets::Shader& out) {
        // assets_for() says which source it could not find, so there is no
        // message here.
        std::vector<assets::AssetRecord> forms;
        if (!content.assets_for(source, forms)) {
            return false;
        }
        std::vector<std::byte> bytes;
        if (!content.read(forms.front().guid, bytes)) {
            ENGINE_LOG_ERROR("{} would not read.", source);
            return false;
        }
        return assets::read_shader(bytes, out, forms.front().name);
    }

    namespace {

        /// The gfx name for a kind the cooked shader reports.
        [[nodiscard]] gfx::DescriptorKind to_gfx_kind(assets::DescriptorKind kind) {
            switch (kind) {
            case assets::DescriptorKind::UniformBuffer:
                return gfx::DescriptorKind::UniformBuffer;
            case assets::DescriptorKind::StorageBuffer:
                return gfx::DescriptorKind::StorageBuffer;
            case assets::DescriptorKind::CombinedImageSampler:
                break;
            }
            return gfx::DescriptorKind::CombinedImageSampler;
        }

        /// The gfx stage bits for the ones the cooked shader reports.
        [[nodiscard]] std::uint32_t to_gfx_stages(std::uint32_t stages) {
            std::uint32_t out = 0;
            if ((stages & assets::kStageBitVertex) != 0) {
                out |= gfx::kStageBitVertex;
            }
            if ((stages & assets::kStageBitFragment) != 0) {
                out |= gfx::kStageBitFragment;
            }
            if ((stages & assets::kStageBitCompute) != 0) {
                out |= gfx::kStageBitCompute;
            }
            return out;
        }

    } // namespace

    bool merge_bindings(const assets::Shader& vertex, const assets::Shader& fragment,
                        std::vector<gfx::DescriptorBinding>& merged) {
        bool ok = true;
        const auto add = [&merged, &ok](const assets::Shader& shader) {
            for (const assets::ShaderBinding& source : shader.bindings) {
                const auto found =
                    std::find_if(merged.begin(), merged.end(),
                                 [&source](const gfx::DescriptorBinding& entry) {
                                     return entry.set == source.set &&
                                            entry.binding == source.binding;
                                 });
                if (found != merged.end()) {
                    const gfx::DescriptorKind kind = to_gfx_kind(source.kind);
                    if (found->kind != kind || found->count != source.count) {
                        ENGINE_LOG_ERROR(
                            "The two stages declare set {} binding {} differently, so one "
                            "layout cannot serve both. One says {} of kind {}, the other "
                            "says {} of kind {}.",
                            source.set, source.binding, found->count,
                            static_cast<std::uint32_t>(found->kind), source.count,
                            static_cast<std::uint32_t>(kind));
                        ok = false;
                        continue;
                    }
                    found->stages |= to_gfx_stages(source.stages);
                    continue;
                }
                merged.push_back(gfx::DescriptorBinding{
                    .set = source.set,
                    .binding = source.binding,
                    .count = source.count,
                    .stages = to_gfx_stages(source.stages),
                    .kind = to_gfx_kind(source.kind),
                });
            }
        };
        add(vertex);
        add(fragment);

        std::sort(merged.begin(), merged.end(),
                  [](const gfx::DescriptorBinding& a, const gfx::DescriptorBinding& b) {
                      return a.set != b.set ? a.set < b.set : a.binding < b.binding;
                  });
        return ok;
    }

} // namespace engine::render
