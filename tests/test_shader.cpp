// M5.1 tests for the cooked shader format.
//
// The format carries a descriptor layout that the runtime builds a pipeline
// layout from. A reader that accepts a file it should refuse hands the driver a
// layout that does not match the module, and the message the driver gives back
// names nothing useful. So most of the tests here drive the refusal paths.
//
// The reflection itself needs glslc, so it is tested in test_cooker.cpp. This
// file tests only what the two sides agree on.

#include "assets/shader.h"
#include "check.h"

#include <cstddef>
#include <cstring>
#include <vector>

namespace {

    namespace as = engine::assets;

    /// A shader with two bindings, a parameter block, and a short module.
    [[nodiscard]] as::Shader make_shader() {
        as::Shader shader;
        shader.stage = as::ShaderStage::Fragment;
        shader.push_constant_size = 128;
        shader.spirv = { 0x07230203U, 0x00010600U, 0U, 42U };

        as::ShaderBinding base_color;
        base_color.name = "base_color";
        base_color.set = 0;
        base_color.binding = 0;
        base_color.count = 1;
        base_color.stages = as::kStageBitFragment;
        base_color.kind = as::DescriptorKind::CombinedImageSampler;
        shader.bindings.push_back(base_color);

        as::ShaderBinding params;
        params.name = "material";
        params.set = 0;
        params.binding = 1;
        params.count = 1;
        params.stages = as::kStageBitVertex | as::kStageBitFragment;
        params.block_size = 32;
        params.kind = as::DescriptorKind::UniformBuffer;
        shader.bindings.push_back(params);

        shader.params.push_back(
            as::ShaderParam{ .name = "base_color_factor",
                             .set = 0,
                             .binding = 1,
                             .offset = 0,
                             .size = 16,
                             .type = as::ParamType::Vec4 });
        shader.params.push_back(as::ShaderParam{ .name = "roughness_factor",
                                                 .set = 0,
                                                 .binding = 1,
                                                 .offset = 16,
                                                 .size = 4,
                                                 .type = as::ParamType::Float });
        return shader;
    }

    void test_a_shader_round_trips() {
        const as::Shader written = make_shader();
        const std::vector<std::byte> bytes = as::write_shader(written);

        as::Shader read;
        test::check(as::read_shader(bytes, read, "round trip"), "a cooked shader reads back");
        test::check(read.stage == written.stage, "the stage survives");
        test::check(read.push_constant_size == written.push_constant_size,
                    "the push constant size survives");
        test::check(read.spirv == written.spirv, "the module survives");

        test::check(read.bindings.size() == 2, "both bindings survive");
        if (read.bindings.size() == 2) {
            test::check(read.bindings[0].name == "base_color", "a binding keeps its name");
            test::check(read.bindings[0].kind == as::DescriptorKind::CombinedImageSampler,
                        "a binding keeps its kind");
            test::check(read.bindings[1].stages ==
                            (as::kStageBitVertex | as::kStageBitFragment),
                        "a binding read by both stages keeps both bits");
            test::check(read.bindings[1].block_size == 32, "a uniform block keeps its size");
        }

        test::check(read.params.size() == 2, "both parameters survive");
        if (read.params.size() == 2) {
            test::check(read.params[0].name == "base_color_factor", "a parameter keeps its name");
            test::check(read.params[0].type == as::ParamType::Vec4, "a parameter keeps its type");
            test::check(read.params[1].offset == 16, "a parameter keeps its offset");
        }
    }

    void test_a_shader_with_nothing_bound_round_trips() {
        as::Shader written;
        written.stage = as::ShaderStage::Vertex;
        written.spirv = { 0x07230203U, 1U };

        const std::vector<std::byte> bytes = as::write_shader(written);
        as::Shader read;
        test::check(as::read_shader(bytes, read, "empty"), "a shader that binds nothing reads");
        test::check(read.bindings.empty(), "it has no bindings");
        test::check(read.params.empty(), "it has no parameters");
        test::check(read.spirv == written.spirv, "its module survives");
    }

    void test_two_bindings_with_one_name_share_the_string() {
        as::Shader written;
        written.stage = as::ShaderStage::Fragment;
        written.spirv = { 1U };
        for (std::uint32_t i = 0; i < 2; ++i) {
            as::ShaderBinding binding;
            binding.name = "same_name";
            binding.binding = i;
            binding.stages = as::kStageBitFragment;
            written.bindings.push_back(binding);
        }

        const std::vector<std::byte> bytes = as::write_shader(written);
        as::ShaderHeader header{};
        std::memcpy(&header, bytes.data(), sizeof(header));
        // One copy of the name, not two. A shader that declares the same name in
        // several places is common, and the file has no reason to repeat it.
        test::check(header.string_bytes == 9, "one name is stored once");

        as::Shader read;
        test::check(as::read_shader(bytes, read, "shared"), "it still reads back");
        test::check(read.bindings.size() == 2 && read.bindings[0].name == "same_name" &&
                        read.bindings[1].name == "same_name",
                    "both bindings resolve to that name");
    }

    void test_a_file_that_is_not_a_shader_is_refused() {
        std::vector<std::byte> bytes = as::write_shader(make_shader());
        const std::uint32_t wrong = as::kShaderMagic + 1;
        std::memcpy(bytes.data(), &wrong, sizeof(wrong));

        as::Shader read;
        test::check(!as::read_shader(bytes, read, "bad magic"), "a wrong magic is refused");
    }

    void test_another_version_is_refused() {
        std::vector<std::byte> bytes = as::write_shader(make_shader());
        const std::uint32_t wrong = as::kShaderVersion + 1;
        std::memcpy(bytes.data() + sizeof(std::uint32_t), &wrong, sizeof(wrong));

        as::Shader read;
        test::check(!as::read_shader(bytes, read, "bad version"), "another version is refused");
    }

    void test_a_short_file_is_refused() {
        const std::vector<std::byte> bytes = as::write_shader(make_shader());

        as::Shader read;
        test::check(!as::read_shader(std::span(bytes).first(bytes.size() - 4), read, "short"),
                    "a file that ends early is refused");
        test::check(!as::read_shader(std::span(bytes).first(8), read, "tiny"),
                    "a file shorter than the header is refused");
    }

    void test_a_file_with_something_extra_is_refused() {
        std::vector<std::byte> bytes = as::write_shader(make_shader());
        bytes.push_back(std::byte{ 0 });

        as::Shader read;
        test::check(!as::read_shader(bytes, read, "long"), "a file that runs long is refused");
    }

    void test_a_module_of_no_words_is_refused() {
        as::Shader written;
        written.stage = as::ShaderStage::Vertex;

        const std::vector<std::byte> bytes = as::write_shader(written);
        as::Shader read;
        // The reader would otherwise pass an empty module to the driver, which
        // reports a failure that names neither the file nor the reason.
        test::check(!as::read_shader(bytes, read, "no module"),
                    "a shader carrying no module is refused");
    }

    void test_a_kind_this_build_does_not_know_is_refused() {
        std::vector<std::byte> bytes = as::write_shader(make_shader());
        const std::uint32_t wrong = as::kDescriptorKindMax + 1;
        // The first binding record follows the header, and kind is its third
        // field.
        std::memcpy(bytes.data() + as::kShaderHeaderSize + (2 * sizeof(std::uint32_t)), &wrong,
                    sizeof(wrong));

        as::Shader read;
        test::check(!as::read_shader(bytes, read, "bad kind"),
                    "a descriptor kind this build has no binding for is refused");
    }

    void test_a_name_past_the_end_of_the_strings_is_refused() {
        std::vector<std::byte> bytes = as::write_shader(make_shader());
        const std::uint32_t wrong = 4096;
        // name_offset is the seventh field of the first binding record.
        std::memcpy(bytes.data() + as::kShaderHeaderSize + (6 * sizeof(std::uint32_t)), &wrong,
                    sizeof(wrong));

        as::Shader read;
        test::check(!as::read_shader(bytes, read, "bad name"),
                    "a name that runs past the string block is refused");
    }

} // namespace

int main() {
    test::section("the cooked shader format");
    test_a_shader_round_trips();
    test_a_shader_with_nothing_bound_round_trips();
    test_two_bindings_with_one_name_share_the_string();
    test::section("a file this build must refuse");
    test_a_file_that_is_not_a_shader_is_refused();
    test_another_version_is_refused();
    test_a_short_file_is_refused();
    test_a_file_with_something_extra_is_refused();
    test_a_module_of_no_words_is_refused();
    test_a_kind_this_build_does_not_know_is_refused();
    test_a_name_past_the_end_of_the_strings_is_refused();
    return test::report();
}
