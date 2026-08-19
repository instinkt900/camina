#include "import/shader.h"

#include "core/log.h"

#include <shaderc/shaderc.h>
#include <spirv_reflect.h>

#include <algorithm>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

namespace engine::import {

    namespace {

        namespace as = engine::assets;

        /// The stage bit that goes into a cooked binding, for one stage.
        [[nodiscard]] std::uint32_t stage_bit(as::ShaderStage stage) {
            switch (stage) {
            case as::ShaderStage::Vertex:
                return as::kStageBitVertex;
            case as::ShaderStage::Fragment:
                return as::kStageBitFragment;
            case as::ShaderStage::Compute:
                return as::kStageBitCompute;
            }
            return 0;
        }

        /**
         * Turns a SPIRV-Reflect descriptor type into one this build stores.
         *
         * The cooked format carries the three kinds the engine binds today. A
         * shader that asks for anything else fails the cook rather than cooking
         * a layout that does not match the module.
         */
        [[nodiscard]] bool to_descriptor_kind(SpvReflectDescriptorType type,
                                              as::DescriptorKind& out) {
            switch (type) {
            case SPV_REFLECT_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
                out = as::DescriptorKind::CombinedImageSampler;
                return true;
            case SPV_REFLECT_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
                out = as::DescriptorKind::UniformBuffer;
                return true;
            case SPV_REFLECT_DESCRIPTOR_TYPE_STORAGE_BUFFER:
                out = as::DescriptorKind::StorageBuffer;
                return true;
            default:
                break;
            }
            return false;
        }

        /**
         * Works out the element type of one member of a uniform block.
         *
         * A type this build has no editor for becomes ParamType::Unknown rather
         * than failing the cook. The pipeline layout does not depend on it, so a
         * shader that reads an exotic type still works, and the inspector shows
         * that member as read-only.
         */
        [[nodiscard]] as::ParamType to_param_type(const SpvReflectTypeDescription* type) {
            if (type == nullptr) {
                return as::ParamType::Unknown;
            }

            const std::uint32_t flags = type->type_flags;
            const bool is_float = (flags & SPV_REFLECT_TYPE_FLAG_FLOAT) != 0;
            const bool is_int = (flags & SPV_REFLECT_TYPE_FLAG_INT) != 0;

            if ((flags & SPV_REFLECT_TYPE_FLAG_MATRIX) != 0) {
                const bool is_four_by_four = type->traits.numeric.matrix.column_count == 4 &&
                                             type->traits.numeric.matrix.row_count == 4;
                return is_float && is_four_by_four ? as::ParamType::Mat4 : as::ParamType::Unknown;
            }

            if ((flags & SPV_REFLECT_TYPE_FLAG_VECTOR) != 0) {
                if (!is_float) {
                    return as::ParamType::Unknown;
                }
                switch (type->traits.numeric.vector.component_count) {
                case 2:
                    return as::ParamType::Vec2;
                case 3:
                    return as::ParamType::Vec3;
                case 4:
                    return as::ParamType::Vec4;
                default:
                    return as::ParamType::Unknown;
                }
            }

            if (is_float) {
                return as::ParamType::Float;
            }
            if (is_int) {
                // Signedness 0 means unsigned, which is what the GLSL `uint` is.
                return type->traits.numeric.scalar.signedness == 0 ? as::ParamType::UInt
                                                                   : as::ParamType::Int;
            }
            return as::ParamType::Unknown;
        }

        /**
         * Adds every member of one uniform block to the parameter list.
         *
         * A member with no name is skipped. The inspector has nothing to label
         * it with, and a padding member reflects that way.
         */
        void collect_params(const SpvReflectBlockVariable& block, std::uint32_t set,
                            std::uint32_t binding, std::vector<as::ShaderParam>& out) {
            for (std::uint32_t i = 0; i < block.member_count; ++i) {
                const SpvReflectBlockVariable& member = block.members[i];
                if (member.name == nullptr || member.name[0] == '\0') {
                    continue;
                }
                as::ShaderParam param;
                param.name = member.name;
                param.set = set;
                param.binding = binding;
                param.offset = member.offset;
                param.size = member.size;
                param.type = to_param_type(member.type_description);
                out.push_back(std::move(param));
            }
        }

        /// Reads every descriptor the module declares into the cooked shader.
        [[nodiscard]] bool take_bindings(const SpvReflectShaderModule& module,
                                         as::ShaderStage stage, as::Shader& out,
                                         std::string_view where) {
            std::uint32_t count = 0;
            if (spvReflectEnumerateDescriptorBindings(&module, &count, nullptr) !=
                SPV_REFLECT_RESULT_SUCCESS) {
                ENGINE_LOG_ERROR("{}: the descriptor bindings did not enumerate.", where);
                return false;
            }
            if (count == 0) {
                return true;
            }

            std::vector<SpvReflectDescriptorBinding*> bindings(count);
            if (spvReflectEnumerateDescriptorBindings(&module, &count, bindings.data()) !=
                SPV_REFLECT_RESULT_SUCCESS) {
                ENGINE_LOG_ERROR("{}: the descriptor bindings did not enumerate.", where);
                return false;
            }

            for (const SpvReflectDescriptorBinding* source : bindings) {
                if (source == nullptr) {
                    continue;
                }

                as::DescriptorKind kind{};
                if (!to_descriptor_kind(source->descriptor_type, kind)) {
                    ENGINE_LOG_ERROR("{}: set {} binding {} is a descriptor type this build "
                                     "does not bind. See assets::DescriptorKind.",
                                     where, source->set, source->binding);
                    return false;
                }

                as::ShaderBinding binding;
                binding.name = source->name != nullptr ? source->name : "";
                binding.set = source->set;
                binding.binding = source->binding;
                // count is 0 for a runtime-sized array, which nothing here binds.
                binding.count = std::max(source->count, 1U);
                binding.stages = stage_bit(stage);
                binding.kind = kind;
                if (kind != as::DescriptorKind::CombinedImageSampler) {
                    binding.block_size = source->block.size;
                    collect_params(source->block, source->set, source->binding, out.params);
                }
                out.bindings.push_back(std::move(binding));
            }
            return true;
        }

        /// Works out how many bytes of push constants the stage reads.
        [[nodiscard]] bool take_push_constant_size(const SpvReflectShaderModule& module,
                                                   as::Shader& out, std::string_view where) {
            std::uint32_t count = 0;
            if (spvReflectEnumeratePushConstantBlocks(&module, &count, nullptr) !=
                SPV_REFLECT_RESULT_SUCCESS) {
                ENGINE_LOG_ERROR("{}: the push constant blocks did not enumerate.", where);
                return false;
            }
            if (count == 0) {
                return true;
            }

            std::vector<SpvReflectBlockVariable*> blocks(count);
            if (spvReflectEnumeratePushConstantBlocks(&module, &count, blocks.data()) !=
                SPV_REFLECT_RESULT_SUCCESS) {
                ENGINE_LOG_ERROR("{}: the push constant blocks did not enumerate.", where);
                return false;
            }

            // Vulkan gives one push constant range to each stage, so the size is
            // the end of the last block rather than a sum of the blocks.
            //
            // absolute_offset and not offset. For a push constant block, offset
            // is the lowest offset of any member, and size already counts from
            // the start of the range. Adding the two would count a member that
            // starts late twice. A block holding one mat4 at `layout(offset =
            // 64)` reports offset 64 and size 128, and the range really is 128.
            for (const SpvReflectBlockVariable* block : blocks) {
                if (block == nullptr) {
                    continue;
                }
                out.push_constant_size =
                    std::max(out.push_constant_size, block->absolute_offset + block->size);
            }
            return true;
        }

        /**
         * Puts the bindings and the parameters in a settled order.
         *
         * SPIRV-Reflect reports them in the order it found them. Sorting makes
         * the cooked bytes the same for the same source, which the freshness
         * hash and a person reading a diff both want. The pipeline layout needs
         * the bindings sorted by set as well.
         */
        void sort_reflection(as::Shader& out) {
            std::sort(out.bindings.begin(), out.bindings.end(),
                      [](const as::ShaderBinding& a, const as::ShaderBinding& b) {
                          return a.set != b.set ? a.set < b.set : a.binding < b.binding;
                      });
            std::sort(out.params.begin(), out.params.end(),
                      [](const as::ShaderParam& a, const as::ShaderParam& b) {
                          if (a.set != b.set) {
                              return a.set < b.set;
                          }
                          if (a.binding != b.binding) {
                              return a.binding < b.binding;
                          }
                          return a.offset < b.offset;
                      });
        }

        /// Reads a whole text file from @p path and returns it as a string.
        [[nodiscard]] std::string read_text(const std::filesystem::path& path) {
            std::ifstream file(path, std::ios::binary | std::ios::ate);
            if (!file) {
                return {};
            }
            const std::streamsize size = file.tellg();
            if (size <= 0) {
                return {};
            }
            file.seekg(0);
            std::string text(static_cast<std::size_t>(size), '\0');
            if (!file.read(text.data(), size)) {
                return {};
            }
            return text;
        }

        /// Writes a whole cooked file.
        [[nodiscard]] bool write_bytes(const std::filesystem::path& path,
                                       std::span<const std::byte> bytes) {
            std::ofstream file(path, std::ios::binary | std::ios::trunc);
            if (!file) {
                ENGINE_LOG_ERROR("{}: could not open it to write.", path.string());
                return false;
            }
            file.write(reinterpret_cast<const char*>(bytes.data()),
                       static_cast<std::streamsize>(bytes.size()));
            if (!file) {
                ENGINE_LOG_ERROR("{}: the cooked shader did not write.", path.string());
                return false;
            }
            return true;
        }

    } // namespace

    bool is_shader_extension(std::string_view extension) {
        return extension == ".vert" || extension == ".frag" || extension == ".comp";
    }

    bool shader_stage_for(const std::filesystem::path& source, as::ShaderStage& out) {
        const std::string extension = source.extension().string();
        if (extension == ".vert") {
            out = as::ShaderStage::Vertex;
            return true;
        }
        if (extension == ".frag") {
            out = as::ShaderStage::Fragment;
            return true;
        }
        if (extension == ".comp") {
            out = as::ShaderStage::Compute;
            return true;
        }
        return false;
    }

    bool reflect_shader(std::span<const std::uint32_t> words, as::ShaderStage stage,
                        as::Shader& out, std::string_view where) {
        SpvReflectShaderModule module{};
        const SpvReflectResult created = spvReflectCreateShaderModule(
            words.size() * sizeof(std::uint32_t), words.data(), &module);
        if (created != SPV_REFLECT_RESULT_SUCCESS) {
            ENGINE_LOG_ERROR("{}: the module did not reflect. SPIRV-Reflect returned {}.", where,
                             static_cast<int>(created));
            return false;
        }

        // One exit, so the module is destroyed on every path below.
        //
        // Every field this fills is cleared first, because take_push_constant_size
        // folds with std::max and would otherwise keep a larger size the caller
        // left behind. The header promises to fill everything but the words.
        out.stage = stage;
        out.push_constant_size = 0;
        out.bindings.clear();
        out.params.clear();

        bool ok = take_bindings(module, stage, out, where) &&
                  take_push_constant_size(module, out, where);
        if (ok) {
            sort_reflection(out);
        }

        spvReflectDestroyShaderModule(&module);
        return ok;
    }

    [[nodiscard]] shaderc_shader_kind to_shaderc_kind(as::ShaderStage stage) {
        switch (stage) {
        case as::ShaderStage::Vertex:
            return shaderc_vertex_shader;
        case as::ShaderStage::Fragment:
            return shaderc_fragment_shader;
        case as::ShaderStage::Compute:
            return shaderc_compute_shader;
        }
        return shaderc_vertex_shader;
    }

    bool cook_shader(const std::filesystem::path& source,
                     const std::filesystem::path& destination,
                     const std::vector<std::string>& defines) {
        as::ShaderStage stage{};
        if (!shader_stage_for(source, stage)) {
            ENGINE_LOG_ERROR("{}: no stage goes with that extension.", source.string());
            return false;
        }

        const std::string text = read_text(source);
        if (text.empty()) {
            ENGINE_LOG_ERROR("{}: could not read the source file.", source.string());
            return false;
        }

        // No optimizations here, and that is deliberate. spirv-opt strips every
        // OpName from the module, and a parameter block with no names gives an
        // inspector nothing to label a field with. The driver runs its own
        // optimizer. Measured on the M5.6 sandbox scene at 1280x720 on a
        // GeForce MX250: median frame time without -O was 1.808 ms and with -O
        // was 1.812 ms (average of three 300-frame runs each). That is inside
        // the run-to-run noise, so dropping -O costs nothing measurable.
        shaderc_compiler_t compiler = shaderc_compiler_initialize();
        shaderc_compile_options_t options = shaderc_compile_options_initialize();
        shaderc_compile_options_set_target_env(options, shaderc_target_env_vulkan,
                                               shaderc_env_version_vulkan_1_3);
        shaderc_compile_options_set_source_language(options,
                                                    shaderc_source_language_glsl);
        for (const std::string& define : defines) {
            shaderc_compile_options_add_macro_definition(
                options, define.c_str(), define.size(), nullptr, 0);
        }

        const shaderc_compilation_result_t result = shaderc_compile_into_spv(
            compiler, text.c_str(), text.size(), to_shaderc_kind(stage),
            source.string().c_str(), "main", options);

        shaderc_compile_options_release(options);
        shaderc_compiler_release(compiler);

        const shaderc_compilation_status status =
            shaderc_result_get_compilation_status(result);
        if (status != shaderc_compilation_status_success) {
            ENGINE_LOG_ERROR("{}: {}\n{}", source.string(),
                             status == shaderc_compilation_status_compilation_error
                                 ? "compilation failed"
                                 : "an internal error stopped the compiler",
                             shaderc_result_get_error_message(result));
            shaderc_result_release(result);
            return false;
        }

        const std::size_t byte_count = shaderc_result_get_length(result);
        const auto* words_begin =
            reinterpret_cast<const std::uint32_t*>(shaderc_result_get_bytes(result));
        std::vector<std::uint32_t> words(words_begin,
                                         words_begin + (byte_count / sizeof(std::uint32_t)));

        shaderc_result_release(result);

        as::Shader shader;
        if (!reflect_shader(words, stage, shader, source.string())) {
            return false;
        }
        shader.spirv = std::move(words);
        shader.defines = defines;

        const std::vector<std::byte> bytes = as::write_shader(shader);
        return write_bytes(destination, bytes);
    }

} // namespace engine::import
