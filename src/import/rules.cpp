#include "import/rules.h"

#include "assets/manifest.h"
#include "assets/reference.h"
#include "assets/material.h"
#include "assets/mesh.h"
#include "assets/script.h"
#include "assets/shader.h"
#include "assets/texture.h"
#include "import/brdf.h"
#include "import/environment.h"
#include "import/mesh.h"
#include "import/shader.h"
#include "import/texture.h"

#include <string>

namespace engine::import {

    namespace {

        namespace as = engine::assets;

        /// Whether a file is a scene or a prefab, which name assets by path.
        [[nodiscard]] bool is_document_extension(std::string_view extension) {
            return extension == ".scene" || extension == ".prefab";
        }

        /// Whether a file is a Lua script.
        [[nodiscard]] bool is_script_extension(std::string_view extension) {
            return extension == as::kScriptExtension;
        }

        /// Whether a file is a font face the UI loads.
        [[nodiscard]] bool is_font_extension(std::string_view extension) {
            return extension == ".ttf";
        }

        /// Whether a file is a moth_ui layout.
        [[nodiscard]] bool is_layout_extension(std::string_view extension) {
            return extension == ".mothui";
        }

    } // namespace

    /// An extension in lower case, so a rule matches whatever a file shouts.
    [[nodiscard]] std::string lowered_extension(const std::filesystem::path& source) {
        const std::string extension = source.extension().string();
        std::string lower;
        lower.reserve(extension.size());
        for (const char c : extension) {
            lower.push_back(c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c);
        }
        return lower;
    }

    /**
     * The rule for this file, or nothing when it is not content.
     *
     * A rule is what makes a file content. Anything with a consumer earns
     * one, even when the rule only copies the bytes, and a file with no rule
     * is a build tool or a note. It gets no identity, no sidecar, and no
     * place in the cooked tree. See issue #178.
     *
     * The extension is lowered first. Half the predicates below lower it
     * again on their own and half never did, so `SKY.HDR` cooked and
     * `MAIN.SCENE` did not. That gap used to end in the copy path, which
     * put the file in the cooked tree unchanged. Now it ends in no rule at
     * all, and the file would leave the cooked tree without a word.
     */
    [[nodiscard]] std::optional<Rule> rule_for(const std::filesystem::path& source) {
        const std::string extension = lowered_extension(source);
        if (is_shader_extension(extension)) {
            return Rule::Shader;
        }
        // Before the image rule, because both read through stb_image and
        // an HDR panorama is not a texture a material samples.
        if (is_environment_extension(extension)) {
            return Rule::Environment;
        }
        if (is_brdf_extension(extension)) {
            return Rule::Brdf;
        }
        if (is_image_extension(extension)) {
            return Rule::Texture;
        }
        if (is_mesh_extension(extension)) {
            return Rule::Mesh;
        }
        if (is_document_extension(extension)) {
            return Rule::Document;
        }
        // The rule copies the bytes, so this changes no output. It is here
        // so that a script is content by declaration rather than by falling
        // past every other rule. See src/assets/script.h and issue #178.
        if (is_script_extension(extension)) {
            return Rule::Script;
        }
        if (is_font_extension(extension)) {
            return Rule::Font;
        }
        if (is_layout_extension(extension)) {
            return Rule::Layout;
        }
        return std::nullopt;
    }

    /// The name a rule adds to the source name, or nothing for a copy.
    [[nodiscard]] const char* cooked_suffix(Rule rule) {
        switch (rule) {
        case Rule::Shader:
            // cube.vert becomes cube.vert.0.shader, so cube.vert and cube.frag
            // stay two files rather than collapsing onto one name.
            return as::kShaderExtension;
        case Rule::Texture:
        case Rule::Environment:
        case Rule::Brdf:
            // A cubemap is a texture with six faces and the BRDF table is a
            // two channel one, so all three are the same cooked format and
            // they keep the same extension.
            return as::kTextureExtension;
        case Rule::Mesh:
            return as::kMeshExtension;
        case Rule::Document:
            // It keeps its own name. A scene is still a scene after its
            // references resolve, and the runtime opens it by that name.
        case Rule::Script:
            // The same. A cooked script is the source text, so the cooked
            // file is a .lua and a person can read it in the cooked tree.
        case Rule::Font:
        case Rule::Layout:
            // Both go through unchanged, and both are opened by the name the
            // source had. src/ui/font_factory.h names the face by path.
            break;
        }
        return "";
    }

    /**
     * The cooked path for one part of a source, under the same directory.
     *
     * A rule that writes one file uses part 0 and adds only the suffix. The
     * glTF rule numbers its parts, so `robot.gltf` gives `robot.gltf.0.mesh`
     * and `robot.gltf.1.mesh`. The number comes before the suffix so the
     * extension still says what the file is.
     */
    [[nodiscard]] std::filesystem::path cooked_name(const std::filesystem::path& relative,
                                                    Rule rule, std::uint32_t part) {
        std::filesystem::path named = relative;
        if (rule == Rule::Mesh || rule == Rule::Shader) {
            named += "." + std::to_string(part);
        }
        named += cooked_suffix(rule);
        return named;
    }


    assets::AssetRecord part_record(const std::filesystem::path& relative, Guid parent,
                                    std::string_view kind, std::string_view extension,
                                    std::uint32_t index) {
        std::filesystem::path cooked = relative;
        cooked += "." + std::to_string(index);
        cooked += extension;
        return assets::AssetRecord{ .guid = Guid::derive(parent, kind, index),
                                    .source = assets::manifest_path(relative),
                                    .name = assets::manifest_path(cooked) };
    }

} // namespace engine::import
