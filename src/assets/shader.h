#pragma once

/**
 * @file
 * @brief The cooked shader format, shared by the cooker and the runtime.
 *
 * A cooked shader is the SPIR-V module and a description of what that module
 * reads. The cooker runs glslc, reflects the result with SPIRV-Reflect, and
 * writes both into one file. The runtime reads the description and builds the
 * descriptor set layout from it.
 *
 * The layout used to live in hand-written C++, beside a shader that declared the
 * same thing in GLSL. Nothing compared the two, so a mismatch appeared as a
 * validation error or a wrong texture, far from the line that caused it. See
 * DESIGN.md section 9 "Shader pipeline".
 *
 * The module and its description travel in one file on purpose. Two files would
 * need two manifest entries and a derived GUID for the second, and they could
 * drift apart. One file cannot describe a module it does not carry.
 *
 * `tools/cooker/shader.cpp` writes this, and nothing else writes it. A change
 * here is a format change, so it moves the version below and the cooker version
 * in `manifest.h`.
 */

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace engine::assets {

    /// @brief The name a cooked shader file carries after the source name.
    inline constexpr const char* kShaderExtension = ".shader";

    /**
     * @brief The first four bytes of a cooked shader file.
     *
     * The value spells "CSHD" when a person opens the file in a hex viewer.
     */
    inline constexpr std::uint32_t kShaderMagic = 0x44485343U;

    /// @brief The format version this build writes and reads.
    inline constexpr std::uint32_t kShaderVersion = 1;

    /// @brief Which stage a cooked shader belongs to.
    enum class ShaderStage : std::uint32_t {
        Vertex = 0, ///< The vertex stage, from a `.vert` source.
        Fragment,   ///< The fragment stage, from a `.frag` source.
        Compute,    ///< The compute stage, from a `.comp` source.
    };

    /// @brief The largest ::ShaderStage value, so a reader can reject the rest.
    inline constexpr std::uint32_t kShaderStageMax = static_cast<std::uint32_t>(ShaderStage::Compute);

    /// @brief What kind of resource one descriptor binding holds.
    enum class DescriptorKind : std::uint32_t {
        CombinedImageSampler = 0, ///< A texture and its sampler together.
        UniformBuffer,            ///< A read-only block of parameters.
        StorageBuffer,            ///< A read-write block.
    };

    /// @brief The largest ::DescriptorKind value, so a reader can reject the rest.
    inline constexpr std::uint32_t kDescriptorKindMax =
        static_cast<std::uint32_t>(DescriptorKind::StorageBuffer);

    /// @brief The stage bit for the vertex stage, used in ShaderBinding::stages.
    inline constexpr std::uint32_t kStageBitVertex = 1U << 0U;
    /// @brief The stage bit for the fragment stage, used in ShaderBinding::stages.
    inline constexpr std::uint32_t kStageBitFragment = 1U << 1U;
    /// @brief The stage bit for the compute stage, used in ShaderBinding::stages.
    inline constexpr std::uint32_t kStageBitCompute = 1U << 2U;

    /**
     * @brief The element type of one member of a parameter block.
     *
     * These are the types a material parameter uses. A type SPIRV-Reflect
     * reports that is not one of these becomes ::ParamType::Unknown, which the
     * inspector shows as read-only rather than guessing at it.
     */
    enum class ParamType : std::uint32_t {
        Float = 0, ///< One 32-bit float.
        Vec2,      ///< Two floats.
        Vec3,      ///< Three floats.
        Vec4,      ///< Four floats.
        Int,       ///< One signed 32-bit integer.
        UInt,      ///< One unsigned 32-bit integer.
        Mat4,      ///< A four by four matrix of floats.
        Unknown,   ///< Something this build has no editor for.
    };

    /// @brief The largest ::ParamType value, so a reader can reject the rest.
    inline constexpr std::uint32_t kParamTypeMax = static_cast<std::uint32_t>(ParamType::Unknown);

    /**
     * @brief One descriptor a cooked shader reads, as stored in the file.
     *
     * Every member is four bytes wide, so the layout is the same on both
     * platforms and the struct needs no packing attribute. The name is an offset
     * and a length into the string block that follows the arrays.
     */
    struct ShaderBindingRecord {
        std::uint32_t set = 0;         ///< The `layout(set = N)` in the shader.
        std::uint32_t binding = 0;     ///< The `layout(binding = N)` in the shader.
        std::uint32_t kind = 0;        ///< A ::DescriptorKind value.
        std::uint32_t count = 1;       ///< Array length, or 1 for a plain declaration.
        std::uint32_t stages = 0;      ///< Which stages read it, as ::kStageBitVertex and friends.
        std::uint32_t block_size = 0;  ///< Bytes of a uniform block, or 0 for a texture.
        std::uint32_t name_offset = 0; ///< Byte offset of the name in the string block.
        std::uint32_t name_length = 0; ///< Bytes of the name, with no terminator.
    };

    /**
     * @brief One member of a parameter block, as stored in the file.
     *
     * The set and the binding say which block it belongs to, because a shader
     * may read more than one.
     */
    struct ShaderParamRecord {
        std::uint32_t set = 0;         ///< The set of the block that holds it.
        std::uint32_t binding = 0;     ///< The binding of the block that holds it.
        std::uint32_t offset = 0;      ///< Byte offset inside the block.
        std::uint32_t size = 0;        ///< Bytes this member occupies.
        std::uint32_t type = 0;        ///< A ::ParamType value.
        std::uint32_t name_offset = 0; ///< Byte offset of the name in the string block.
        std::uint32_t name_length = 0; ///< Bytes of the name, with no terminator.
        std::uint32_t reserved = 0;    ///< Keeps the record a multiple of 8 bytes.
    };

    /**
     * @brief The fixed part at the front of a cooked shader file.
     *
     * The counts say how long each block that follows is. The blocks come in the
     * order the members are declared here: the bindings, the parameters, the
     * string block, and then the SPIR-V words.
     */
    struct ShaderHeader {
        std::uint32_t magic = kShaderMagic;     ///< ::kShaderMagic. Checked first.
        std::uint32_t version = kShaderVersion; ///< ::kShaderVersion when written.
        std::uint32_t stage = 0;                ///< A ::ShaderStage value.
        std::uint32_t binding_count = 0;        ///< How many ShaderBindingRecord entries follow.
        std::uint32_t param_count = 0;          ///< How many ShaderParamRecord entries follow.
        std::uint32_t string_bytes = 0;         ///< Bytes of the string block, before padding.
        std::uint32_t spirv_words = 0;          ///< How many 32-bit words the module holds.
        std::uint32_t push_constant_size = 0;   ///< Bytes of push constants this stage reads.
    };

    /// @brief How many bytes the fixed header holds.
    inline constexpr std::size_t kShaderHeaderSize = 32;
    static_assert(sizeof(ShaderHeader) == kShaderHeaderSize,
                  "The header starts the file and it is written and read as raw "
                  "bytes, so its size is part of the file format.");

    /// @brief How many bytes one binding record holds.
    inline constexpr std::size_t kShaderBindingRecordSize = 32;
    static_assert(sizeof(ShaderBindingRecord) == kShaderBindingRecordSize,
                  "A binding record goes to disk as raw bytes, so its size is "
                  "part of the file format.");

    /// @brief How many bytes one parameter record holds.
    inline constexpr std::size_t kShaderParamRecordSize = 32;
    static_assert(sizeof(ShaderParamRecord) == kShaderParamRecordSize,
                  "A parameter record goes to disk as raw bytes, so its size is "
                  "part of the file format.");

    /// @brief One descriptor a shader reads, with its name resolved.
    struct ShaderBinding {
        std::string name;                                    ///< The identifier in the GLSL source.
        std::uint32_t set = 0;                               ///< The set it belongs to.
        std::uint32_t binding = 0;                           ///< The binding inside that set.
        std::uint32_t count = 1;                             ///< Array length, or 1.
        std::uint32_t stages = 0;                            ///< Which stages read it.
        std::uint32_t block_size = 0;                        ///< Bytes of a uniform block, or 0.
        DescriptorKind kind = DescriptorKind::UniformBuffer; ///< What the binding holds.
    };

    /// @brief One member of a parameter block, with its name resolved.
    struct ShaderParam {
        std::string name;                    ///< The identifier in the GLSL source.
        std::uint32_t set = 0;               ///< The set of the block that holds it.
        std::uint32_t binding = 0;           ///< The binding of that block.
        std::uint32_t offset = 0;            ///< Byte offset inside the block.
        std::uint32_t size = 0;              ///< Bytes this member occupies.
        ParamType type = ParamType::Unknown; ///< The element type.
    };

    /**
     * @brief A cooked shader, read into memory.
     *
     * This is what `render::` builds a pipeline from. The words are the module,
     * and the bindings are the layout the module needs.
     */
    struct Shader {
        std::vector<std::uint32_t> spirv;        ///< The module, as 32-bit words.
        std::vector<ShaderBinding> bindings;     ///< Every descriptor the module reads.
        std::vector<ShaderParam> params;         ///< Every member of every uniform block.
        std::uint32_t push_constant_size = 0;    ///< Bytes of push constants the stage reads.
        ShaderStage stage = ShaderStage::Vertex; ///< Which stage the module belongs to.
    };

    /**
     * @brief Reads a cooked shader.
     *
     * This checks the magic, the version, and that every block the header
     * promises is inside the file. A short file would otherwise make the reader
     * walk past the end, and a module the driver rejects reports an error that
     * names nothing useful.
     *
     * @param bytes The whole file, as read from disk.
     * @param out The shader to fill. It copies, so @p bytes may go away after.
     * @param where A name for the log, usually the asset path.
     * @return True when the file is a cooked shader this build understands.
     */
    [[nodiscard]] bool read_shader(std::span<const std::byte> bytes, Shader& out,
                                   std::string_view where);

    /**
     * @brief Writes a shader into the bytes a cooked file holds.
     *
     * The cooker calls this. It is here rather than in the cooker so that one
     * file decides the layout, and the reader beside it can be tested against
     * the writer.
     *
     * @param shader The shader to write.
     * @return The whole file, ready to go to disk.
     */
    [[nodiscard]] std::vector<std::byte> write_shader(const Shader& shader);

    /**
     * @brief A short name for a ::DescriptorKind.
     * @param kind The value to name.
     * @return A static string. The caller must not free it.
     */
    [[nodiscard]] const char* descriptor_kind_name(DescriptorKind kind);

    /**
     * @brief A short name for a ::ParamType.
     * @param type The value to name.
     * @return A static string. The caller must not free it.
     */
    [[nodiscard]] const char* param_type_name(ParamType type);

} // namespace engine::assets
