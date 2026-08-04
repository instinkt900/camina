#include "assets/shader.h"

#include "core/log.h"

#include <cstring>

namespace engine::assets {

    namespace {

        /// Rounds a byte count up to the next multiple of four.
        ///
        /// The SPIR-V words come last and the reader copies them out as words,
        /// so the string block before them has to end on a four-byte boundary.
        [[nodiscard]] std::size_t round_up_to_word(std::size_t bytes) {
            return (bytes + 3U) & ~static_cast<std::size_t>(3U);
        }

        /// Adds a name to the string block, and says where it landed.
        ///
        /// Two bindings with the same name share one entry. A shader has few
        /// names, so the search costs nothing and the file stays smaller.
        void place_name(std::string& strings, std::string_view name, std::uint32_t& out_offset,
                        std::uint32_t& out_length) {
            out_length = static_cast<std::uint32_t>(name.size());
            const std::size_t found = strings.find(name);
            if (found != std::string::npos) {
                out_offset = static_cast<std::uint32_t>(found);
                return;
            }
            out_offset = static_cast<std::uint32_t>(strings.size());
            strings.append(name);
        }

        /// Reads a name back out of the string block, with the bounds checked.
        [[nodiscard]] bool take_name(std::string_view strings, std::uint32_t offset,
                                     std::uint32_t length, std::string& out,
                                     std::string_view where) {
            if (static_cast<std::size_t>(offset) + length > strings.size()) {
                ENGINE_LOG_ERROR("{}: a name runs past the end of the string block.", where);
                return false;
            }
            out.assign(strings.substr(offset, length));
            return true;
        }

    } // namespace

    const char* descriptor_kind_name(DescriptorKind kind) {
        switch (kind) {
        case DescriptorKind::CombinedImageSampler:
            return "combined image sampler";
        case DescriptorKind::UniformBuffer:
            return "uniform buffer";
        case DescriptorKind::StorageBuffer:
            return "storage buffer";
        }
        return "unknown";
    }

    const char* param_type_name(ParamType type) {
        switch (type) {
        case ParamType::Float:
            return "float";
        case ParamType::Vec2:
            return "vec2";
        case ParamType::Vec3:
            return "vec3";
        case ParamType::Vec4:
            return "vec4";
        case ParamType::Int:
            return "int";
        case ParamType::UInt:
            return "uint";
        case ParamType::Mat4:
            return "mat4";
        case ParamType::Unknown:
            break;
        }
        return "unknown";
    }

    std::vector<std::byte> write_shader(const Shader& shader) {
        // The names go into one block, so a record carries an offset and a
        // length rather than a fixed-size array that would cap a name.
        std::string strings;
        std::vector<ShaderBindingRecord> bindings;
        bindings.reserve(shader.bindings.size());
        for (const ShaderBinding& source : shader.bindings) {
            ShaderBindingRecord record{};
            record.set = source.set;
            record.binding = source.binding;
            record.kind = static_cast<std::uint32_t>(source.kind);
            record.count = source.count;
            record.stages = source.stages;
            record.block_size = source.block_size;
            place_name(strings, source.name, record.name_offset, record.name_length);
            bindings.push_back(record);
        }

        std::vector<ShaderParamRecord> params;
        params.reserve(shader.params.size());
        for (const ShaderParam& source : shader.params) {
            ShaderParamRecord record{};
            record.set = source.set;
            record.binding = source.binding;
            record.offset = source.offset;
            record.size = source.size;
            record.type = static_cast<std::uint32_t>(source.type);
            place_name(strings, source.name, record.name_offset, record.name_length);
            params.push_back(record);
        }

        ShaderHeader header;
        header.stage = static_cast<std::uint32_t>(shader.stage);
        header.binding_count = static_cast<std::uint32_t>(bindings.size());
        header.param_count = static_cast<std::uint32_t>(params.size());
        header.string_bytes = static_cast<std::uint32_t>(strings.size());
        header.spirv_words = static_cast<std::uint32_t>(shader.spirv.size());
        header.push_constant_size = shader.push_constant_size;

        const std::size_t padded_strings = round_up_to_word(strings.size());
        std::vector<std::byte> bytes;
        bytes.resize(kShaderHeaderSize + (bindings.size() * kShaderBindingRecordSize) +
                     (params.size() * kShaderParamRecordSize) + padded_strings +
                     (shader.spirv.size() * sizeof(std::uint32_t)));

        std::size_t at = 0;
        const auto put = [&bytes, &at](const void* from, std::size_t size) {
            if (size > 0) {
                std::memcpy(bytes.data() + at, from, size);
            }
            at += size;
        };

        put(&header, sizeof(header));
        put(bindings.data(), bindings.size() * kShaderBindingRecordSize);
        put(params.data(), params.size() * kShaderParamRecordSize);
        put(strings.data(), strings.size());
        // The padding stays zero, because resize() zeroed the whole buffer.
        at += padded_strings - strings.size();
        put(shader.spirv.data(), shader.spirv.size() * sizeof(std::uint32_t));
        return bytes;
    }

    bool read_shader(std::span<const std::byte> bytes, Shader& out, std::string_view where) {
        if (bytes.size() < kShaderHeaderSize) {
            ENGINE_LOG_ERROR("{}: a cooked shader starts with {} bytes of header and this file "
                             "holds {}.",
                             where, kShaderHeaderSize, bytes.size());
            return false;
        }

        // A copy, not a cast. The file may sit at any alignment in the caller's
        // buffer, and reading a struct through a misaligned pointer is undefined.
        ShaderHeader header{};
        std::memcpy(&header, bytes.data(), sizeof(header));

        if (header.magic != kShaderMagic) {
            ENGINE_LOG_ERROR("{}: not a cooked shader. Cook the content tree again.", where);
            return false;
        }
        if (header.version != kShaderVersion) {
            ENGINE_LOG_ERROR("{}: written by version {} and this build reads version {}. "
                             "Cook the content tree again.",
                             where, header.version, kShaderVersion);
            return false;
        }
        if (header.stage > kShaderStageMax) {
            ENGINE_LOG_ERROR("{}: it names stage {} and this build knows {} of them.", where,
                             header.stage, kShaderStageMax + 1);
            return false;
        }
        if (header.spirv_words == 0) {
            ENGINE_LOG_ERROR("{}: it carries no SPIR-V module.", where);
            return false;
        }

        // Every block the header promises must be inside the file. Without this
        // a short file walks the reader past the end, and a header with a huge
        // count would ask for an allocation nothing can serve.
        const std::size_t padded_strings = round_up_to_word(header.string_bytes);
        const std::size_t expected = kShaderHeaderSize +
                                     (static_cast<std::size_t>(header.binding_count) *
                                      kShaderBindingRecordSize) +
                                     (static_cast<std::size_t>(header.param_count) *
                                      kShaderParamRecordSize) +
                                     padded_strings +
                                     (static_cast<std::size_t>(header.spirv_words) *
                                      sizeof(std::uint32_t));
        if (bytes.size() != expected) {
            ENGINE_LOG_ERROR("{}: its header describes {} bytes and the file holds {}.", where,
                             expected, bytes.size());
            return false;
        }

        std::size_t at = kShaderHeaderSize;
        const auto take = [&bytes, &at](void* into, std::size_t size) {
            if (size > 0) {
                std::memcpy(into, bytes.data() + at, size);
            }
            at += size;
        };

        std::vector<ShaderBindingRecord> binding_records(header.binding_count);
        take(binding_records.data(), binding_records.size() * kShaderBindingRecordSize);

        std::vector<ShaderParamRecord> param_records(header.param_count);
        take(param_records.data(), param_records.size() * kShaderParamRecordSize);

        std::string strings(header.string_bytes, '\0');
        take(strings.data(), strings.size());
        at += padded_strings - strings.size();

        out.bindings.clear();
        out.bindings.reserve(binding_records.size());
        for (const ShaderBindingRecord& record : binding_records) {
            if (record.kind > kDescriptorKindMax) {
                ENGINE_LOG_ERROR("{}: a binding names kind {} and this build knows {} of them.",
                                 where, record.kind, kDescriptorKindMax + 1);
                return false;
            }
            ShaderBinding binding;
            if (!take_name(strings, record.name_offset, record.name_length, binding.name, where)) {
                return false;
            }
            binding.set = record.set;
            binding.binding = record.binding;
            binding.count = record.count;
            binding.stages = record.stages;
            binding.block_size = record.block_size;
            binding.kind = static_cast<DescriptorKind>(record.kind);
            out.bindings.push_back(std::move(binding));
        }

        out.params.clear();
        out.params.reserve(param_records.size());
        for (const ShaderParamRecord& record : param_records) {
            if (record.type > kParamTypeMax) {
                ENGINE_LOG_ERROR("{}: a parameter names type {} and this build knows {} of them.",
                                 where, record.type, kParamTypeMax + 1);
                return false;
            }
            ShaderParam param;
            if (!take_name(strings, record.name_offset, record.name_length, param.name, where)) {
                return false;
            }
            param.set = record.set;
            param.binding = record.binding;
            param.offset = record.offset;
            param.size = record.size;
            param.type = static_cast<ParamType>(record.type);
            out.params.push_back(std::move(param));
        }

        out.spirv.resize(header.spirv_words);
        take(out.spirv.data(), out.spirv.size() * sizeof(std::uint32_t));

        out.push_constant_size = header.push_constant_size;
        out.stage = static_cast<ShaderStage>(header.stage);
        return true;
    }

} // namespace engine::assets
