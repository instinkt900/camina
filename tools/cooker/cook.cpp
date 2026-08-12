#include "cook.h"

#include "assets/manifest.h"
#include "assets/mesh.h"
#include "assets/meta.h"
#include "assets/reference.h"
#include "assets/script.h"
#include "assets/shader.h"
#include "assets/texture.h"
#include "core/log.h"
#include "brdf.h"
#include "document.h"
#include "environment.h"
#include "mesh.h"
#include "shader.h"
#include "texture.h"

#include <algorithm>
#include <map>
#include <set>
#include <system_error>
#include <vector>

namespace cooker {

    namespace {

        namespace as = engine::assets;

        /// What rule turns one source file into one cooked file.
        enum class Rule : std::uint8_t {
            Shader,      ///< GLSL through shaderc, out as SPIR-V and its reflected layout.
            Texture,     ///< An image through stb, out as mip levels and BC7 blocks.
            Environment, ///< An HDR panorama through stb, out as a half float cubemap.
            Brdf,        ///< The split sum BRDF table, integrated from its sidecar alone.
            Mesh,        ///< glTF through cgltf, out as one cooked mesh for each mesh.
            Document,    ///< A scene or a prefab, with its asset references resolved.
            Script,      ///< Lua source text, copied. See src/assets/script.h.
            Copy,        ///< No rule yet. The bytes go through unchanged.
        };

        /// Whether a file is a scene or a prefab, which name assets by path.
        [[nodiscard]] bool is_document_extension(std::string_view extension) {
            return extension == ".scene" || extension == ".prefab";
        }

        /// Whether a file is a Lua script.
        [[nodiscard]] bool is_script_extension(std::string_view extension) {
            return extension == as::kScriptExtension;
        }

        [[nodiscard]] Rule rule_for(const std::filesystem::path& source) {
            const std::string extension = source.extension().string();
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
            return Rule::Copy;
        }

        /**
         * Whether a file is documentation rather than content.
         *
         * A content directory holds a note saying where a model came from and
         * what its license is. That note belongs next to the files it
         * describes, and it is not an asset. A file with no extension counts
         * too, which covers LICENSE and COPYING.
         */
        [[nodiscard]] bool is_documentation(const std::filesystem::path& relative) {
            const std::string extension = relative.extension().string();
            if (extension.empty()) {
                return true;
            }
            std::string lower;
            lower.reserve(extension.size());
            for (const char c : extension) {
                lower.push_back(c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c);
            }
            return lower == ".md" || lower == ".txt";
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
            case Rule::Copy:
                break;
            }
            return "";
        }

        [[nodiscard]] bool copy_through(const std::filesystem::path& source,
                                        const std::filesystem::path& destination) {
            std::error_code error;
            std::filesystem::copy_file(source, destination,
                                       std::filesystem::copy_options::overwrite_existing, error);
            if (error) {
                ENGINE_LOG_ERROR("{}: could not copy it. {}", source.string(), error.message());
                return false;
            }
            return true;
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

        /**
         * Compiles one GLSL source once for each variant its sidecar lists.
         *
         * A sidecar with no variant list gives one module with no defines, which
         * is what every shader got before permutations. The base form keeps the
         * identity of the source asset, so a reference to the shader itself still
         * resolves. Every other variant derives one.
         */
        [[nodiscard]] bool cook_shader_variants(const Options& options,
                                                const std::filesystem::path& source,
                                                const std::filesystem::path& relative,
                                                const as::AssetMeta& meta,
                                                std::vector<as::ManifestOutput>& outputs) {
            // One base form when the sidecar names none, so the list below is
            // never empty and the loop needs no special case.
            std::vector<as::ShaderVariant> variants = meta.shader.variants;
            if (variants.empty()) {
                variants.push_back(as::ShaderVariant{ .name = "base", .defines = {} });
            }

            // Part 0 keeps the asset's own identity, so it has to be the form
            // with nothing defined. A list that starts with a variant carrying
            // defines would give the plain shader a derived identity that
            // nothing refers to.
            if (!variants.front().defines.empty()) {
                ENGINE_LOG_ERROR("{}: the first variant defines {} and it must define nothing. "
                                 "It is the base form and it keeps the identity of the source.",
                                 relative.string(), variants.front().defines.front());
                return false;
            }

            for (std::size_t at = 0; at < variants.size(); ++at) {
                const as::ShaderVariant& variant = variants[at];
                const std::filesystem::path cooked =
                    cooked_name(relative, Rule::Shader, static_cast<std::uint32_t>(at));
                const std::filesystem::path destination = options.out / cooked;

                std::error_code error;
                std::filesystem::create_directories(destination.parent_path(), error);
                if (!cook_shader(source, destination, variant.defines)) {
                    ENGINE_LOG_ERROR("{}: variant {} did not compile.", relative.string(),
                                     variant.name.empty() ? "with no name" : variant.name);
                    return false;
                }

                outputs.push_back(as::ManifestOutput{
                    .cooked = as::manifest_path(cooked),
                    .guid = at == 0 ? meta.guid
                                    : engine::Guid::derive(meta.guid, as::kShaderPartKind,
                                                           static_cast<std::uint32_t>(at)) });
            }
            return true;
        }

        /**
         * Runs the one rule that matches this asset, and says what it wrote.
         *
         * Every rule but the glTF one writes a single file, and that file goes
         * by the source asset's own GUID. A glTF file writes one for each mesh
         * and one for each material, and each of those needs an identity of its
         * own, because a prefab has to name one mesh. That rule derives them
         * itself, because it alone knows how many parts of each kind there are.
         */
        [[nodiscard]] bool cook_one(const Options& options, Rule rule,
                                    const std::filesystem::path& relative,
                                    const as::AssetMeta& meta,
                                    std::vector<as::ManifestOutput>& outputs) {
            const std::filesystem::path source = options.content / relative;

            const auto single = [&](auto&& run) {
                const std::filesystem::path cooked = cooked_name(relative, rule, 0);
                const std::filesystem::path destination = options.out / cooked;
                std::error_code error;
                std::filesystem::create_directories(destination.parent_path(), error);
                if (!run(destination)) {
                    return false;
                }
                outputs.push_back(as::ManifestOutput{ .cooked = as::manifest_path(cooked),
                                                      .guid = meta.guid });
                return true;
            };

            switch (rule) {
            case Rule::Shader:
                return cook_shader_variants(options, source, relative, meta, outputs);
            case Rule::Texture:
                return single([&](const std::filesystem::path& to) {
                    return cook_texture(source, to, meta.texture);
                });
            case Rule::Environment:
                // Two outputs, so it cannot use single(). The cubemap keeps the
                // source identity and the irradiance derives one beside it.
                return cook_environment(source, options.out, relative, meta.guid,
                                        meta.environment, outputs);
            case Rule::Brdf:
                // The source file is read for nothing but its identity. Every
                // number the table needs is in the sidecar.
                return single([&](const std::filesystem::path& to) {
                    return cook_brdf(to, meta.brdf);
                });
            case Rule::Mesh:
                return cook_gltf(source, options.out, relative, meta.guid, outputs);
            case Rule::Document:
                return single([&](const std::filesystem::path& to) {
                    return cook_document(source, to, options.content);
                });
            case Rule::Script:
                // The bytes go through unchanged, the same as a copy. The rule
                // exists so that a script is content by declaration, and so
                // that issue #258 has a place to add a precompile step.
            case Rule::Copy:
                break;
            }
            return single(
                [&](const std::filesystem::path& to) { return copy_through(source, to); });
        }

        /// Every regular file under the tree, sorted, so two runs agree on the order.
        [[nodiscard]] bool gather(const std::filesystem::path& root,
                                  std::vector<std::filesystem::path>& out) {
            std::error_code error;
            const std::filesystem::recursive_directory_iterator walk(root, error);
            if (error) {
                ENGINE_LOG_ERROR("Could not read {}. {}", root.string(), error.message());
                return false;
            }

            for (const auto& item : walk) {
                if (!item.is_regular_file()) {
                    continue;
                }
                const std::filesystem::path relative =
                    std::filesystem::relative(item.path(), root, error);
                if (error) {
                    continue;
                }
                // A sidecar describes an asset. It is not one.
                if (relative.extension() == as::kMetaExtension) {
                    continue;
                }
                // Documentation is not an asset either. A content directory
                // holds a note saying where a model came from and what its
                // license is, and that note belongs next to the files it
                // describes. Cooking it would give it a GUID and copy it where
                // nothing reads it.
                if (is_documentation(relative)) {
                    continue;
                }
                out.push_back(relative);
            }

            std::ranges::sort(out);
            return true;
        }

        /// What every glTF in the tree names besides itself.
        struct Named {
            /// Extra input paths for each glTF, relative to the content root.
            std::map<std::filesystem::path, std::vector<std::filesystem::path>> inputs;
            /// Every file some glTF names as a buffer, so no rule cooks one.
            std::set<std::filesystem::path> buffers;
        };

        /**
         * Reads every glTF for the files it names, before anything is cooked.
         *
         * A named buffer is an input, so the manifest has to hash it, or
         * editing the geometry would look like it did nothing. It is also not
         * an asset: it is glTF payload with no meaning of its own, and the copy
         * rule would put the vertex data in the cooked tree a second time where
         * nothing reads it.
         *
         * An image a glTF names is not like this. That one is a real asset with
         * a sidecar of its own, and the texture rule cooks it. Its sidecar is
         * an input all the same, because a cooked material stores the identity
         * out of that file.
         */
        void scan_gltf(const Options& options,
                       const std::vector<std::filesystem::path>& sources, Named& out) {
            for (const std::filesystem::path& relative : sources) {
                if (rule_for(relative) != Rule::Mesh) {
                    continue;
                }
                GltfReferences named;
                // A file that will not parse names nothing here and fails in
                // the rule, where the message belongs.
                (void)gltf_references(options.content / relative, relative, named);

                std::vector<std::filesystem::path> inputs;
                for (const std::filesystem::path& path : named.buffers) {
                    out.buffers.insert(path);
                    inputs.push_back(path);
                }
                for (const std::filesystem::path& path : named.images) {
                    inputs.push_back(as::meta_path(path));
                }
                out.inputs.emplace(relative, std::move(inputs));
            }
        }

        /**
         * Reads every scene and prefab for the assets it names, before cooking.
         *
         * The sidecar of a named asset is an input, because the identity comes
         * out of that file. Replacing a sidecar gives the asset a new identity,
         * and a document that still held the old one would name nothing. The
         * asset itself is not an input: editing the pixels of a texture changes
         * the texture and not the scene that names it.
         */
        void scan_documents(const Options& options,
                            const std::vector<std::filesystem::path>& sources, Named& out) {
            for (const std::filesystem::path& relative : sources) {
                if (rule_for(relative) != Rule::Document) {
                    continue;
                }
                std::vector<std::filesystem::path> named;
                document_references(options.content / relative, named);

                std::vector<std::filesystem::path> inputs;
                inputs.reserve(named.size());
                for (const std::filesystem::path& path : named) {
                    inputs.push_back(as::meta_path(path));
                }
                out.inputs.emplace(relative, std::move(inputs));
            }
        }

        /// What happened to one source file.
        enum class Outcome : std::uint8_t {
            Cooked,  ///< The rule ran and wrote its outputs.
            Skipped, ///< The manifest already had it, unchanged.
            Failed,  ///< It did not cook. The reason is logged.
        };

        /// Builds the input list for one source, sidecar and glTF buffers included.
        void gather_inputs(const std::filesystem::path& relative, const Named& named,
                           as::ManifestEntry& entry) {
            entry.inputs.push_back(entry.source);
            // The sidecar is an input, not only a place to keep the identity.
            // It carries the import settings, so flipping a texture from sRGB
            // to linear has to cook that texture again. Without this the edit
            // would look like it did nothing.
            entry.inputs.push_back(as::manifest_path(as::meta_path(relative)));

            // A .gltf keeps its geometry in a .bin next to it, and it names the
            // images its materials use. Both are inputs as much as the .gltf is.
            if (const auto found = named.inputs.find(relative); found != named.inputs.end()) {
                for (const std::filesystem::path& path : found->second) {
                    entry.inputs.push_back(as::manifest_path(path));
                }
            }
        }

        /**
         * Whether the cooker can leave an old entry alone.
         *
         * Four things have to agree. The identity, because a replaced sidecar
         * gives the asset a new one and every reference has to see it. The
         * input list, because is_fresh hashes the inputs the old entry names,
         * so an entry an older cooker wrote would stay fresh forever against a
         * list this build no longer uses. The name of the first output, which
         * catches a rule whose naming changed. And the hash of every input.
         *
         * A rule that started writing another number of files needs no check of
         * its own. The count follows the source bytes, so the hash catches it.
         */
        [[nodiscard]] bool can_skip(const Options& options, const std::filesystem::path& relative,
                                    Rule rule, const as::ManifestEntry& entry,
                                    const as::ManifestEntry* old, bool same_cooker) {
            // A rule that started writing a new kind of output changes none of
            // the checks below. The manifest would stay fresh forever and the
            // new output would never appear, which a person meets as content
            // missing after an engine update with nothing said about it.
            if (options.force || old == nullptr || !same_cooker) {
                return false;
            }
            const std::string first = as::manifest_path(cooked_name(relative, rule, 0));
            return old->guid == entry.guid && old->inputs == entry.inputs &&
                   !old->outputs.empty() && old->outputs.front().cooked == first &&
                   as::is_fresh(*old, options.content, options.out);
        }

        /**
         * Puts back what the last cook wrote for a source that failed.
         *
         * A rule that fails writes no output, so the cooked file from before is
         * still there and still good. Dropping the entry would hide it, and the
         * next start of the program would then fail on an asset that is sitting
         * right there. A person editing a shader meets this on the first typo.
         */
        void carry_forward(const Options& options, const as::Manifest& previous,
                           const std::filesystem::path& relative, as::Manifest& next) {
            const as::ManifestEntry* kept =
                as::find_by_source(previous, as::manifest_path(relative));
            if (kept == nullptr) {
                return;
            }

            // Only when every file it names is still there. An entry pointing
            // at a file somebody deleted is worse than no entry at all: the
            // manifest then says the asset is available, and the read fails
            // later and further from the cause.
            for (const as::ManifestOutput& output : kept->outputs) {
                std::error_code error;
                if (!std::filesystem::is_regular_file(options.out / output.cooked, error)) {
                    ENGINE_LOG_WARN("{}: it did not cook, and {} is gone as well, so the "
                                    "cooked tree no longer holds it.",
                                    relative.generic_string(), output.cooked);
                    return;
                }
            }

            ENGINE_LOG_WARN("{}: it did not cook, so the cooked tree keeps the one from "
                            "before.",
                            relative.generic_string());
            next.entries.push_back(*kept);
        }

        /**
         * Checks that every identity a document names was really cooked.
         *
         * Deriving an identity answers for any index, so a reference to a part
         * that is not there gives a GUID that looks like every other one and
         * names nothing. Only the finished manifest can tell the two apart, so
         * this runs after the whole tree is cooked. It covers a document the
         * cooker skipped as well, because a model that lost a mesh breaks a
         * reference that nobody touched.
         *
         * A document that did not cook is left alone. It has been reported once
         * already, and it fails this check for the same reason, so checking it
         * again would log the same line twice and count one failure as two.
         */
        [[nodiscard]] std::size_t check_documents(
            const Options& options, const std::vector<std::filesystem::path>& sources,
            const std::set<std::filesystem::path>& unfinished, const as::Manifest& manifest) {
            std::size_t failed = 0;
            for (const std::filesystem::path& relative : sources) {
                if (rule_for(relative) != Rule::Document || unfinished.contains(relative)) {
                    continue;
                }
                if (!validate_references(options.content / relative, options.content, manifest)) {
                    ++failed;
                }
            }
            return failed;
        }

        /// Cooks one source file, or says why it did not.
        [[nodiscard]] Outcome cook_source(const Options& options,
                                          const std::filesystem::path& relative,
                                          const as::Manifest& previous, const Named& named,
                                          bool same_cooker, as::ManifestEntry& entry) {
            const std::filesystem::path source = options.content / relative;
            const Rule rule = rule_for(relative);

            // An image goes through image_meta(), which fills in the color
            // space guess for a sidecar it has to write. The glTF rule calls
            // the same function, so whichever rule reaches an image first
            // records the same guess.
            as::AssetMeta meta;
            const bool read = rule == Rule::Texture ? image_meta(source, meta)
                                                    : as::meta_for(source, meta);
            if (!read) {
                return Outcome::Failed;
            }

            entry.source = as::manifest_path(relative);
            entry.guid = meta.guid;
            gather_inputs(relative, named, entry);

            const as::ManifestEntry* old = as::find_by_source(previous, entry.source);
            if (can_skip(options, relative, rule, entry, old, same_cooker)) {
                entry = *old;
                return Outcome::Skipped;
            }

            if (!cook_one(options, rule, relative, meta, entry.outputs)) {
                return Outcome::Failed;
            }

            if (!as::hash_inputs(options.content, entry.inputs, entry.hash)) {
                ENGINE_LOG_ERROR("{}: cooked, but an input would not read back.",
                                 source.string());
                return Outcome::Failed;
            }
            return Outcome::Cooked;
        }

    } // namespace

    /// Removes cooked files that nothing in the manifest names.
    void prune_orphan_outputs(const std::filesystem::path& out, const as::Manifest& manifest) {
        // Every output the manifest knows about, relative to the cooked root.
        std::set<std::filesystem::path> known;
        known.insert("manifest.json");
        for (const as::ManifestEntry& entry : manifest.entries) {
            for (const as::ManifestOutput& output : entry.outputs) {
                known.insert(as::manifest_path(output.cooked));
            }
        }

        std::set<std::filesystem::path> removed;
        auto iter = std::filesystem::recursive_directory_iterator(
            out, std::filesystem::directory_options::skip_permission_denied);
        for (const auto& entry : iter) {
            if (!entry.is_regular_file()) {
                continue;
            }
            const std::filesystem::path relative = std::filesystem::relative(entry.path(), out);
            if (!known.contains(relative)) {
                std::error_code error;
                std::filesystem::remove(entry.path(), error);
                if (!error) {
                    removed.insert(relative);
                }
            }
        }

        if (!removed.empty()) {
            ENGINE_LOG_INFO("Pruned {} cooked files the new manifest no longer names.",
                            removed.size());
        }
    }

    bool cook_all(const Options& options, Result& result) {
        result = Result{};

        std::error_code error;
        if (!std::filesystem::is_directory(options.content, error)) {
            ENGINE_LOG_ERROR("{} is not a content directory.", options.content.string());
            return false;
        }

        std::filesystem::create_directories(options.out, error);
        if (error) {
            ENGINE_LOG_ERROR("Could not make {}. {}", options.out.string(), error.message());
            return false;
        }

        std::vector<std::filesystem::path> sources;
        if (!gather(options.content, sources)) {
            return false;
        }

        // The old manifest says what is already cooked. A missing one is a
        // first run, not a failure, so the return value does not matter here.
        as::Manifest previous;
        (void)as::load_manifest(options.out, previous);

        // A manifest an older cooker wrote may be missing an output this build
        // produces, and no per-entry check can see that. So the whole tree
        // cooks again when the version moves.
        const bool same_cooker = previous.cooker == as::kCookerVersion;
        if (!same_cooker && !previous.entries.empty()) {
            ENGINE_LOG_INFO("The cooked tree came from cooker {} and this is cooker {}, so "
                            "everything cooks again.",
                            previous.cooker, as::kCookerVersion);
        }

        Named named;
        scan_gltf(options, sources, named);
        scan_documents(options, sources, named);

        as::Manifest next;
        /// The sources that did not cook, so the check below leaves them alone.
        std::set<std::filesystem::path> unfinished;
        for (const std::filesystem::path& relative : sources) {
            // A glTF buffer is payload, not an asset. scan_gltf found it.
            if (named.buffers.contains(relative)) {
                continue;
            }

            as::ManifestEntry entry;
            switch (cook_source(options, relative, previous, named, same_cooker, entry)) {
            case Outcome::Cooked:
                next.entries.push_back(std::move(entry));
                ++result.cooked;
                break;
            case Outcome::Skipped:
                next.entries.push_back(std::move(entry));
                ++result.skipped;
                break;
            case Outcome::Failed:
                unfinished.insert(relative);
                // The entry goes back with the outputs it had, so the check
                // below still finds every identity a kept document names. That
                // document is in `unfinished`, so it is not checked again.
                carry_forward(options, previous, relative, next);
                ++result.failed;
                break;
            }
        }

        // Copying an asset together with its .meta sidecar gives two source
        // files one identity. A glTF with a copied sidecar also duplicates
        // every derived sub-asset GUID. This walk catches both, before the
        // manifest is written, so the person hears about it on the cook rather
        // than on the first hot reload after.
        {
            std::map<engine::Guid, std::string> seen;
            for (const as::ManifestEntry& entry : next.entries) {
                for (const as::ManifestOutput& output : entry.outputs) {
                    const auto [at, added] = seen.emplace(output.guid, entry.source);
                    if (!added) {
                        ENGINE_LOG_ERROR(
                            "{} and {} both carry identity {}. Copying a file "
                            "copied its .meta sidecar too, and the sidecar holds the identity. "
                            "Delete the sidecar of one of them, so the cooker gives it a new "
                            "identity of its own. Then cook again.",
                            entry.source, at->second, output.guid.to_text());
                        return false;
                    }
                }
            }
        }

        result.failed += check_documents(options, sources, unfinished, next);

        if (!as::save_manifest(options.out, next)) {
            return false;
        }

        prune_orphan_outputs(options.out, next);

        return result.failed == 0;
    }

} // namespace cooker
