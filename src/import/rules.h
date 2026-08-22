#pragma once

/**
 * @file
 * @brief What rule an asset takes, and what the rule calls its output.
 *
 * This was inside `cook.cpp` until M13.3. It moved out because the editor has
 * to work out what a source tree holds without cooking it, and a second copy of
 * these answers would drift from the cooker's copy the first time a rule
 * changed. A drift here is quiet: the editor would name an asset differently
 * from the runtime and the two pictures would stop matching for a reason
 * nothing reports. See `DESIGN.md` §10 M13.
 */

#include "assets/asset_source.h"
#include "assets/meta.h"
#include "core/guid.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace engine::import {

    /// @brief What rule turns one source file into one cooked file.
    enum class Rule : std::uint8_t {
        Shader,      ///< GLSL through shaderc, out as SPIR-V and its reflected layout.
        Texture,     ///< An image through stb, out as mip levels and BC7 blocks.
        Environment, ///< An HDR panorama through stb, out as a half float cubemap.
        Brdf,        ///< The split sum BRDF table, integrated from its sidecar alone.
        Mesh,        ///< glTF through cgltf, out as one cooked mesh for each mesh.
        Sound,       ///< A sound file, decoded to PCM or kept encoded to stream.
        Document,    ///< A scene or a prefab, with its asset references resolved.
        Script,      ///< Lua source text, copied. See src/assets/script.h.
        Font,        ///< A TrueType face, copied. src/ui/font.h opens it by name.
        Layout,      ///< A moth_ui layout, copied. Issue #211 makes it a real type.
    };

    /**
     * @brief An extension in lower case, so a rule matches whatever a file shouts.
     * @param source The source path.
     * @return The extension, dot included, in lower case.
     */
    [[nodiscard]] std::string lowered_extension(const std::filesystem::path& source);

    /**
     * @brief The rule for this file, or nothing when it is not content.
     *
     * A rule is what makes a file content. Anything with a consumer earns one,
     * even when the rule only copies the bytes, and a file with no rule is not
     * an asset and gets no identity.
     *
     * @param source The source path, relative to the content root.
     * @return The rule, or nothing.
     */
    [[nodiscard]] std::optional<Rule> rule_for(const std::filesystem::path& source);

    /**
     * @brief Reads the sidecar of a source asset, with the guess its rule makes.
     *
     * Two rules fill in a field when they write a new sidecar. The texture rule
     * guesses a color space from the file name, and the sound rule guesses
     * whether a file streams. Both guesses are written once, into a sidecar
     * that was not there, and the file decides from then on.
     *
     * **Every caller that may write a sidecar goes through this.** The cooker
     * and the editor both index a tree, and a guess made in one and not in the
     * other is a difference nothing reports: the file on disk is written by
     * whichever arrived first, and it is wrong forever after.
     *
     * @param source The source asset path. The file must exist.
     * @param out The metadata to fill.
     * @return True when @p out holds a valid GUID.
     */
    [[nodiscard]] bool asset_meta(const std::filesystem::path& source, assets::AssetMeta& out);

    /**
     * @brief The name a rule adds to the source name, or nothing for a copy.
     * @param rule The rule.
     * @return The extension, which is empty for a rule that copies.
     */
    [[nodiscard]] const char* cooked_suffix(Rule rule);

    /**
     * @brief The cooked path for one part of a source, under the same directory.
     *
     * A rule that writes one file uses part 0 and adds only the suffix. The
     * glTF rule numbers its parts, so `robot.gltf` gives `robot.gltf.0.mesh`
     * and `robot.gltf.1.mesh`. The number comes before the suffix so the
     * extension still says what the file is.
     *
     * @param relative The source path, relative to the content root.
     * @param rule The rule that cooks it.
     * @param part Which part of the source this is.
     * @return The cooked path, relative to the cooked root.
     */
    [[nodiscard]] std::filesystem::path cooked_name(const std::filesystem::path& relative,
                                                    Rule rule, std::uint32_t part);

    /**
     * @brief The name and identity of one numbered part of a source asset.
     *
     * A glTF holds several meshes, several materials and a prefab, and each one
     * needs a name and an identity of its own. Both follow the same pattern:
     * the source path, a dot, the index, and the extension of the kind, with
     * the identity derived from the source identity under the kind word.
     *
     * **The rules and the editor's index call this one function**, so the two
     * cannot disagree about what a part is called or what it is.
     *
     * @param relative The source path, relative to the content root.
     * @param parent The identity of the source asset.
     * @param kind The kind word, from `src/assets/reference.h`.
     * @param extension The cooked extension of this kind of part.
     * @param index Which part of its kind this is.
     * @return The record, with the name in the form a manifest stores.
     */
    [[nodiscard]] assets::AssetRecord part_record(const std::filesystem::path& relative,
                                                  Guid parent, std::string_view kind,
                                                  std::string_view extension,
                                                  std::uint32_t index);

} // namespace engine::import
