#include "shader.h"

#include "core/log.h"
#include "platform/process.h"

#include <spirv_reflect.h>

#include <algorithm>
#include <fstream>
#include <system_error>
#include <vector>

namespace cooker {

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
            for (const SpvReflectBlockVariable* block : blocks) {
                if (block == nullptr) {
                    continue;
                }
                out.push_constant_size =
                    std::max(out.push_constant_size, block->offset + block->size);
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

        /// Reads a whole file into 32-bit words, which is what SPIR-V is.
        [[nodiscard]] bool read_words(const std::filesystem::path& path,
                                      std::vector<std::uint32_t>& out) {
            std::ifstream file(path, std::ios::binary | std::ios::ate);
            if (!file) {
                ENGINE_LOG_ERROR("{}: could not open the module glslc wrote.", path.string());
                return false;
            }
            const std::streamsize size = file.tellg();
            if (size <= 0 || (size % 4) != 0) {
                ENGINE_LOG_ERROR("{}: {} bytes is not a SPIR-V module.", path.string(), size);
                return false;
            }
            file.seekg(0);
            out.resize(static_cast<std::size_t>(size) / sizeof(std::uint32_t));
            if (!file.read(reinterpret_cast<char*>(out.data()), size)) {
                ENGINE_LOG_ERROR("{}: the module did not read.", path.string());
                return false;
            }
            return true;
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
        out.stage = stage;
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

    bool cook_shader(const std::filesystem::path& glslc, const std::filesystem::path& source,
                     const std::filesystem::path& destination) {
        as::ShaderStage stage{};
        if (!shader_stage_for(source, stage)) {
            ENGINE_LOG_ERROR("{}: no stage goes with that extension.", source.string());
            return false;
        }

        // glslc writes a module and this rule writes a module with a description
        // in front of it, so the compiler output lands beside the cooked file
        // and goes away again.
        std::filesystem::path module_path = destination;
        module_path += ".spv";

        // No -O here, and that is deliberate. glslc passes -O to spirv-opt,
        // which strips every OpName from the module. The set and the binding
        // survive, because they are decorations the module needs to be correct,
        // and every name does not.
        //
        // A parameter block with no names gives an inspector nothing to label a
        // field with, and DESIGN.md section 7 asks for that editor. The driver
        // runs its own optimizer over whatever it is given, so the cost of
        // leaving this out is small on the desktop targets. Issue #90 holds the
        // measurement and the two ways to get both.
        const engine::platform::ProcessResult result = engine::platform::run_process(
            glslc, { "--target-env=vulkan1.3", "-o", module_path.string(), source.string() });

        const auto clean_up = [&module_path]() {
            std::error_code error;
            std::filesystem::remove(module_path, error);
        };

        if (!result.ran) {
            ENGINE_LOG_ERROR("{}: glslc could not start.", source.string());
            clean_up();
            return false;
        }
        if (result.exit_code != 0) {
            ENGINE_LOG_ERROR("{}: glslc returned {}. Its messages are above.", source.string(),
                             result.exit_code);
            clean_up();
            return false;
        }

        std::vector<std::uint32_t> words;
        if (!read_words(module_path, words)) {
            clean_up();
            return false;
        }

        as::Shader shader;
        if (!reflect_shader(words, stage, shader, source.string())) {
            clean_up();
            return false;
        }
        shader.spirv = std::move(words);

        const std::vector<std::byte> bytes = as::write_shader(shader);
        clean_up();
        return write_bytes(destination, bytes);
    }

} // namespace cooker
