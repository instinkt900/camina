#include "import/cook.h"

#include "import/rules.h"

#include <fstream>

#include "assets/manifest.h"
#include "assets/mesh.h"
#include "assets/meta.h"
#include "assets/reference.h"
#include "assets/script.h"
#include "assets/shader.h"
#include "assets/texture.h"
#include "core/log.h"
#include "physics/components.h"
#include "scene/component_registry.h"
#include "script/components.h"
#include "import/brdf.h"
#include "import/document.h"
#include "import/layout.h"
#include "import/environment.h"
#include "import/mesh.h"
#include "import/shader.h"
#include "import/sound.h"
#include "import/texture.h"

#include <algorithm>
#include <map>
#include <optional>
#include <set>
#include <system_error>
#include <vector>

namespace engine::import {

    namespace {

        namespace as = engine::assets;

        /**
         * The component types this run reads a document against.
         *
         * A caller that names none gets the engine's own, which is right for a
         * tree of engine content and for a test. An application cooking a
         * game's content names the registry that game registered into.
         */
        [[nodiscard]] const engine::scene::ComponentRegistry& components_of(
            const Options& options) {
            static const engine::scene::ComponentRegistry kEngineOnly = engine_components();
            return options.components != nullptr ? *options.components : kEngineOnly;
        }

        /// A rule that changes nothing still goes through the writer, so an
        /// import in memory gets the bytes rather than nothing at all.
        [[nodiscard]] bool copy_through(const std::filesystem::path& source, Writer& writer,
                                        const std::filesystem::path& cooked) {
            std::ifstream file(source, std::ios::binary | std::ios::ate);
            if (!file) {
                ENGINE_LOG_ERROR("{}: could not open it to copy.", source.string());
                return false;
            }
            const auto size = static_cast<std::streamsize>(file.tellg());
            file.seekg(0);
            std::vector<std::byte> bytes(static_cast<std::size_t>(size));
            if (size > 0 && !file.read(reinterpret_cast<char*>(bytes.data()), size)) {
                ENGINE_LOG_ERROR("{}: the read failed part way through.", source.string());
                return false;
            }
            return writer.write(cooked, bytes);
        }

        /**
         * Compiles one GLSL source once for each variant its sidecar lists.
         *
         * A sidecar with no variant list gives one module with no defines, which
         * is what every shader got before permutations. The base form keeps the
         * identity of the source asset, so a reference to the shader itself still
         * resolves. Every other variant derives one.
         */
        [[nodiscard]] bool cook_shader_variants(Writer& writer,
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
                if (!cook_shader(source, writer, cooked, variant.defines)) {
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
        [[nodiscard]] bool cook_one(const Options& options, Writer& writer, Rule rule,
                                    const std::filesystem::path& relative,
                                    const as::AssetMeta& meta,
                                    std::vector<as::ManifestOutput>& outputs) {
            const std::filesystem::path source = options.content / relative;

            const auto single = [&](auto&& run) {
                const std::filesystem::path cooked = cooked_name(relative, rule, 0);
                if (!run(cooked)) {
                    return false;
                }
                outputs.push_back(as::ManifestOutput{ .cooked = as::manifest_path(cooked),
                                                      .guid = meta.guid });
                return true;
            };

            switch (rule) {
            case Rule::Shader:
                return cook_shader_variants(writer, source, relative, meta, outputs);
            case Rule::Texture:
                return single([&](const std::filesystem::path& to) {
                    return cook_texture(source, writer, to, meta.texture);
                });
            case Rule::Environment:
                // Two outputs, so it cannot use single(). The cubemap keeps the
                // source identity and the irradiance derives one beside it.
                return cook_environment(source, writer, relative, meta.guid,
                                        meta.environment, outputs);
            case Rule::Brdf:
                // The source file is read for nothing but its identity. Every
                // number the table needs is in the sidecar.
                return single([&](const std::filesystem::path& to) {
                    return cook_brdf(writer, to, meta.brdf);
                });
            case Rule::Mesh:
                return cook_gltf(source, writer, relative, meta.guid, outputs);
            case Rule::Sound:
                return single([&](const std::filesystem::path& to) {
                    return cook_sound(source, writer, to, meta.sound);
                });
            case Rule::Document:
                return single([&](const std::filesystem::path& to) {
                    return cook_document(source, writer, to, options.content,
                                         components_of(options));
                });
            case Rule::Layout:
                return single([&](const std::filesystem::path& to) {
                    return cook_layout(source, writer, to, relative, options.content);
                });
            case Rule::Script:
                // The bytes go through unchanged, the same as a copy. The rule
                // exists so that a script is content by declaration, and so
                // that issue #258 has a place to add a precompile step.
            case Rule::Font:
                // The same again. A face is opened by the name the source had.
                break;
            }
            return single([&](const std::filesystem::path& to) {
                return copy_through(source, writer, to);
            });
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
                // A file with no rule is not content. A generator script, a
                // README, and a license note all live in a content directory
                // and none of them is an asset. Cooking one gave it a GUID,
                // wrote a sidecar next to it in the source tree, and copied it
                // where nothing reads it. See issue #178.
                if (!rule_for(relative)) {
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
                document_references(options.content / relative, components_of(options), named);

                std::vector<std::filesystem::path> inputs;
                inputs.reserve(named.size());
                for (const std::filesystem::path& path : named) {
                    inputs.push_back(as::meta_path(path));
                }
                out.inputs.emplace(relative, std::move(inputs));
            }
        }

        /**
         * Reads every layout for the images it names, before cooking.
         *
         * The sidecar of a named image is an input, for the same reason a
         * document's is: the cooked layout stores the identity out of that
         * file. The image itself is not an input. Editing its pixels changes
         * the texture and not the layout that names it.
         */
        void scan_layouts(const Options& options,
                          const std::vector<std::filesystem::path>& sources, Named& out) {
            for (const std::filesystem::path& relative : sources) {
                if (rule_for(relative) != Rule::Layout) {
                    continue;
                }
                std::vector<std::filesystem::path> named;
                layout_references(options.content / relative, relative, named);

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
                if (!validate_references(options.content / relative, options.content, manifest,
                                         components_of(options))) {
                    ++failed;
                }
            }
            return failed;
        }

        /// Cooks one source file, or says why it did not.
        [[nodiscard]] Outcome cook_source(const Options& options, Writer& writer,
                                          const std::filesystem::path& relative,
                                          const as::Manifest& previous, const Named& named,
                                          bool same_cooker, as::ManifestEntry& entry) {
            const std::filesystem::path source = options.content / relative;
            // gather() drops every file with no rule, so this always holds.
            const std::optional<Rule> found = rule_for(relative);
            if (!found) {
                return Outcome::Failed;
            }
            const Rule rule = *found;

            // asset_meta() reads the sidecar and makes whatever guess the
            // rule makes when it has to write one. The editor's index calls the
            // same function, so a sidecar says the same thing whichever side
            // created it.
            as::AssetMeta meta;
            if (!asset_meta(source, meta)) {
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

            if (!cook_one(options, writer, rule, relative, meta, entry.outputs)) {
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

    bool import_one(const std::filesystem::path& content_root,
                    const std::filesystem::path& relative, Writer& writer,
                    const engine::scene::ComponentRegistry& types,
                    std::vector<as::ManifestOutput>& outputs) {
        const std::filesystem::path source = content_root / relative;

        const std::optional<Rule> found = rule_for(relative);
        if (!found) {
            ENGINE_LOG_ERROR("{}: nothing gives it a rule, so it is not an asset.",
                             source.string());
            return false;
        }

        // The same sidecar read cook_source() makes, guesses included.
        as::AssetMeta meta;
        if (!asset_meta(source, meta)) {
            return false;
        }

        // No cooked root. A rule reaches the file system through the writer
        // now, so an import needs no directory to write into.
        const Options options{
            .content = content_root, .out = {}, .force = false, .components = &types
        };
        return cook_one(options, writer, *found, relative, meta, outputs);
    }

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

    engine::scene::ComponentRegistry engine_components() {
        // The engine registers what it defines, and a game registers what it
        // defines on top. The same order the runtime uses, and for the same
        // reason: physics and script each own their types, so scene/ needs no
        // header from either one.
        engine::scene::ComponentRegistry types;
        engine::scene::register_builtin_components(types);
        engine::physics::register_components(types);
        engine::script::register_components(types);
        return types;
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

        // The cooker puts every rule's result on disk. The editor gives the
        // same rules a MemoryWriter instead, which is the whole of M13.3b.
        FileWriter writer(options.out);

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
        scan_layouts(options, sources, named);

        as::Manifest next;
        /// The sources that did not cook, so the check below leaves them alone.
        std::set<std::filesystem::path> unfinished;
        for (const std::filesystem::path& relative : sources) {
            // A glTF buffer is payload, not an asset. scan_gltf found it.
            if (named.buffers.contains(relative)) {
                continue;
            }

            as::ManifestEntry entry;
            switch (cook_source(options, writer, relative, previous, named, same_cooker, entry)) {
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

} // namespace engine::import
