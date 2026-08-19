#pragma once

/**
 * @file
 * @brief The cooker rule that turns a GLSL source into a cooked shader.
 *
 * This compiles GLSL through libshaderc and reflects the SPIR-V module.
 * `src/assets/shader.h` holds the file format the two sides agree on, and the
 * runtime reads that and nothing else.
 *
 * The reflection runs here rather than in the runtime so that the shipping
 * binary carries no reflection library, and so that a shader describes itself
 * the way every other asset type does. See DESIGN.md section 9 "Shader
 * pipeline".
 */

#include "assets/shader.h"

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace engine::import {

    /**
     * @brief Whether this rule handles a file with this extension.
     * @param extension The extension, with the dot, in lower case.
     * @return True for a GLSL stage source glslc compiles.
     */
    [[nodiscard]] bool is_shader_extension(std::string_view extension);

    /**
     * @brief Which stage a GLSL source belongs to, from its extension.
     * @param source The source path.
     * @param out The stage to fill.
     * @return True when the extension names a stage this build cooks.
     */
    [[nodiscard]] bool shader_stage_for(const std::filesystem::path& source,
                                        engine::assets::ShaderStage& out);

    /**
     * @brief Reflects a compiled module and fills in everything but the words.
     *
     * The caller supplies the compiled SPIR-V module. This works out the
     * descriptors it reads, the members of every uniform block, and the size of
     * the push constant range.
     *
     * A binding this build has no ::engine::assets::DescriptorKind for reports
     * and fails the cook. Passing it through would give a pipeline layout that
     * does not match the module, and the driver would reject it later with a
     * message that names nothing useful.
     *
     * @param words The SPIR-V module.
     * @param stage Which stage the module belongs to.
     * @param out The shader to fill. Its `spirv` member is left alone.
     * @param where A name for the log, usually the source path.
     * @return True when the module reflected and every binding is one this
     * build understands.
     */
    [[nodiscard]] bool reflect_shader(std::span<const std::uint32_t> words,
                                      engine::assets::ShaderStage stage,
                                      engine::assets::Shader& out, std::string_view where);

    /**
     * @brief Cooks one GLSL source into one cooked shader file.
     *
     * This compiles the source through libshaderc, reflects the module, and
     * writes the module and its description together.
     *
     * @param source The GLSL source to read.
     * @param destination The cooked file to write. The directory must exist.
     * @param defines The defines to compile with. An empty list is the base form.
     * @return True when the cooked file was written. False reports why in the
     * log, by source path.
     */
    [[nodiscard]] bool cook_shader(const std::filesystem::path& source,
                                   const std::filesystem::path& destination,
                                   const std::vector<std::string>& defines);

} // namespace engine::import
