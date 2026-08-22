#pragma once

/**
 * @file
 * @brief Turns what a cooked shader declared into what a pipeline takes.
 */

#include "assets/asset_source.h"
#include "assets/shader.h"
#include "gfx/types.h"

#include <string_view>
#include <vector>

namespace engine::render {

    /**
     * @brief Reads one cooked shader that has a single form and no variants.
     *
     * Most stages are one module. A shader with variants gives several forms,
     * and a caller that has to pick between them reads the manifest itself.
     * This takes the first form, which is the base one the source keeps its own
     * identity under.
     *
     * @param content The engine content tree.
     * @param source The source path the manifest lists, such as "sky.vert".
     * @param out Receives the module. Untouched on failure.
     * @return False when the file is missing or will not read. The reason is
     * already reported, so a caller needs no message of its own.
     */
    [[nodiscard]] bool read_one_shader(const assets::AssetSource& content, std::string_view source,
                                       assets::Shader& out);

    /**
     * @brief Joins the descriptors of two stages into one set of bindings.
     *
     * A binding both stages read appears once with both stage bits set. Vulkan
     * takes one set layout for the whole pipeline, so declaring it twice would
     * be two layouts for one set.
     *
     * The two stages have to agree about a slot they share. The cooker cannot
     * check that, because it reflects one module at a time and never sees the
     * pair. So this is the only place the disagreement can be caught, and
     * catching it here beats a validation error or a wrong read later.
     *
     * The result comes out sorted by set and then by binding, which is what
     * gfx::GraphicsPipelineDesc::bindings asks for.
     *
     * @param vertex The cooked vertex stage.
     * @param fragment The cooked fragment stage.
     * @param merged Receives the bindings. Appended to, not cleared.
     * @return False when the two stages declare one slot differently, which is
     * a shader mistake rather than a run-time condition.
     */
    [[nodiscard]] bool merge_bindings(const assets::Shader& vertex, const assets::Shader& fragment,
                                      std::vector<gfx::DescriptorBinding>& merged);

} // namespace engine::render
