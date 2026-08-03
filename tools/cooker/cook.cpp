#include "cook.h"

#include "assets/manifest.h"
#include "assets/mesh.h"
#include "assets/meta.h"
#include "assets/texture.h"
#include "core/log.h"
#include "mesh.h"
#include "texture.h"

#include <algorithm>
#include <cstdlib>
#include <map>
#include <set>
#include <system_error>
#include <vector>

namespace cooker {

    namespace {

        namespace as = engine::assets;

        /// What rule turns one source file into one cooked file.
        enum class Rule : std::uint8_t {
            Shader,  ///< GLSL through glslc, out as SPIR-V.
            Texture, ///< An image through stb, out as mip levels and BC7 blocks.
            Mesh,    ///< glTF through cgltf, out as one cooked mesh for each mesh.
            Copy,    ///< No rule yet. The bytes go through unchanged.
        };

        [[nodiscard]] Rule rule_for(const std::filesystem::path& source) {
            const std::string extension = source.extension().string();
            if (extension == ".vert" || extension == ".frag" || extension == ".comp") {
                return Rule::Shader;
            }
            if (is_image_extension(extension)) {
                return Rule::Texture;
            }
            if (is_mesh_extension(extension)) {
                return Rule::Mesh;
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
                // cube.vert becomes cube.vert.spv, so cube.vert and cube.frag
                // stay two files rather than collapsing onto one name.
                return ".spv";
            case Rule::Texture:
                return as::kTextureExtension;
            case Rule::Mesh:
                return as::kMeshExtension;
            case Rule::Copy:
                break;
            }
            return "";
        }

        /**
         * What kind of part a sub-asset is, for Guid::derive.
         *
         * The word is part of the identity, so changing it changes every GUID
         * derived with it, and every reference to those breaks. Treat it the
         * same as a file format version.
         */
        constexpr const char* kMeshPartKind = "mesh";

        /// Wraps an argument so a shell treats it as one word.
        [[nodiscard]] std::string quoted(const std::string& text) {
            return "\"" + text + "\"";
        }

        /// A path with the separators this platform's shell expects.
        [[nodiscard]] std::string native(const std::filesystem::path& path) {
            std::filesystem::path copy = path;
            copy.make_preferred();
            return copy.string();
        }

        /**
         * Whether a string is safe to put inside double quotes.
         *
         * Double quotes are not enough on their own. A POSIX shell still
         * expands `$name`, `$(command)`, and a backtick inside them, so an
         * asset named `a$(id).vert` would run a command during a cook. cmd.exe
         * expands `%NAME%` in the same way.
         *
         * This refuses those characters rather than escaping them. No asset
         * needs one in its name, the rule is easy to read, and an escape that
         * is subtly wrong on one of the two shells is worse than a refusal.
         * Issue #43 removes the question by spawning the process directly.
         */
        [[nodiscard]] bool shell_safe(const std::string& text, const char* what) {
#if defined(_WIN32)
            // A backslash is an ordinary separator here, and cmd does not
            // treat it as an escape inside quotes.
            constexpr std::string_view kUnsafe = "\"%\r\n";
#else
            constexpr std::string_view kUnsafe = "\"$`\\\r\n";
#endif
            const std::size_t bad = text.find_first_of(kUnsafe);
            if (bad == std::string::npos) {
                return true;
            }
            ENGINE_LOG_ERROR("{} holds '{}', which a shell would read as a command rather "
                             "than as part of the name: {}",
                             what, text.at(bad), text);
            return false;
        }

        /**
         * Builds the command line for one glslc run.
         *
         * Two things here are Windows only, and both are about cmd.exe rather
         * than about glslc.
         *
         * cmd reads a forward slash at the start of a word as a switch, so the
         * program path needs backslashes. CMake hands out forward slashes on
         * both platforms, so this cannot rely on what the caller passed.
         *
         * cmd also removes the first and the last quote of a command that
         * starts with one. That splits a quoted program path in half, and the
         * error it gives back names no file. Wrapping the whole command in one
         * more pair is what cmd documents as the way to keep the inner quotes.
         */
        [[nodiscard]] bool shader_command(const Options& options,
                                          const std::filesystem::path& source,
                                          const std::filesystem::path& destination,
                                          std::string& out) {
            const std::string tool = native(options.glslc);
            const std::string in = native(source);
            const std::string to = native(destination);
            if (!shell_safe(tool, "The path to glslc") || !shell_safe(in, "A source path") ||
                !shell_safe(to, "A cooked path")) {
                return false;
            }

            // --target-env matches the Vulkan version the device asks for, and
            // -O is the same optimization the old CMake rule used. No -mfmt,
            // because the cooked file is now SPIR-V rather than a C header.
            out = quoted(tool) + " --target-env=vulkan1.3 -O -o " + quoted(to) + " " +
                  quoted(in);
#if defined(_WIN32)
            out = "\"" + out + "\"";
#endif
            return true;
        }

        /**
         * Runs glslc over one shader.
         *
         * This spawns a process rather than linking libshaderc, because the
         * build already finds glslc and Conan 2 has no per-target requirement.
         * Issue #43 holds the reasons to change that and the trade it makes.
         */
        [[nodiscard]] bool cook_shader(const Options& options,
                                       const std::filesystem::path& source,
                                       const std::filesystem::path& destination) {
            std::string command;
            if (!shader_command(options, source, destination, command)) {
                return false;
            }

            // std::system gives back a wait status rather than an exit code,
            // and the two differ on POSIX, where exit code 1 arrives as 256.
            // So this reports that glslc failed and leaves the number out.
            // glslc already wrote what is wrong, with a line number.
            if (std::system(command.c_str()) != 0) {
                ENGINE_LOG_ERROR("{}: glslc did not compile it. Its messages are above.",
                                 source.string());
                return false;
            }
            return true;
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
            if (rule == Rule::Mesh) {
                named += "." + std::to_string(part);
            }
            named += cooked_suffix(rule);
            return named;
        }

        /**
         * Runs the one rule that matches this asset, and says what it wrote.
         *
         * Every rule but the glTF one writes a single file, and that file goes
         * by the source asset's own GUID. A glTF file writes one for each mesh,
         * and each of those needs an identity of its own, because a prefab has
         * to name one mesh. Guid::derive works those out from the source GUID
         * with nothing stored, so they are the same on every machine.
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
                return single([&](const std::filesystem::path& to) {
                    return cook_shader(options, source, to);
                });
            case Rule::Texture:
                return single([&](const std::filesystem::path& to) {
                    return cook_texture(source, to, meta.texture);
                });
            case Rule::Mesh: {
                std::vector<std::filesystem::path> cooked;
                if (!cook_gltf(source, options.out, relative, cooked)) {
                    return false;
                }
                for (std::uint32_t part = 0; part < cooked.size(); ++part) {
                    outputs.push_back(as::ManifestOutput{
                        .cooked = as::manifest_path(cooked[part]),
                        .guid = engine::Guid::derive(meta.guid, kMeshPartKind, part) });
                }
                return true;
            }
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
            /// Source path to the files that glTF names, for the input list.
            std::map<std::filesystem::path, std::vector<std::filesystem::path>> inputs;
            /// Every file some glTF names as a buffer, so no rule cooks one.
            std::set<std::filesystem::path> buffers;
        };

        /**
         * Reads every glTF for the files it names, before anything is cooked.
         *
         * This answers two questions at once. A named buffer is an input, so
         * the manifest has to hash it, or editing the geometry would look like
         * it did nothing. A named buffer is also not an asset: it is glTF
         * payload with no meaning of its own, and the copy rule would put the
         * vertex data in the cooked tree a second time where nothing reads it.
         *
         * A texture a glTF names is not like this. That one is a real asset
         * with a sidecar of its own, and the texture rule cooks it.
         */
        void scan_gltf(const Options& options,
                       const std::vector<std::filesystem::path>& sources, Named& out) {
            for (const std::filesystem::path& relative : sources) {
                if (rule_for(relative) != Rule::Mesh) {
                    continue;
                }
                std::vector<std::filesystem::path> named;
                // A file that will not parse names nothing here and fails in
                // the rule, where the message belongs.
                (void)gltf_extra_inputs(options.content / relative, relative, named);
                for (const std::filesystem::path& path : named) {
                    out.buffers.insert(path);
                }
                out.inputs.emplace(relative, std::move(named));
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

            // A .gltf keeps its geometry in a .bin next to it, and that file is
            // an input as much as the .gltf is.
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
                                    const as::ManifestEntry* old) {
            if (options.force || old == nullptr) {
                return false;
            }
            const std::string first = as::manifest_path(cooked_name(relative, rule, 0));
            return old->guid == entry.guid && old->inputs == entry.inputs &&
                   !old->outputs.empty() && old->outputs.front().cooked == first &&
                   as::is_fresh(*old, options.content, options.out);
        }

        /// Cooks one source file, or says why it did not.
        [[nodiscard]] Outcome cook_source(const Options& options,
                                          const std::filesystem::path& relative,
                                          const as::Manifest& previous, const Named& named,
                                          as::ManifestEntry& entry) {
            const std::filesystem::path source = options.content / relative;
            const Rule rule = rule_for(relative);

            as::AssetMeta meta;
            bool created = false;
            if (!as::meta_for(source, meta, &created)) {
                return Outcome::Failed;
            }

            // A new sidecar carries the defaults, and a texture wants a better
            // starting guess than "sRGB" for every file. So the guess goes in
            // once, when the file is written, and after that the file decides.
            // A wrong guess is one edit to fix and it never comes back.
            if (created && rule == Rule::Texture) {
                meta.texture.color_space = guess_color_space(source);
                if (!as::save_meta(source, meta)) {
                    return Outcome::Failed;
                }
                ENGINE_LOG_INFO("{}: reading it as {}. Edit the sidecar to change that.",
                                source.string(), as::to_text(meta.texture.color_space));
            }

            entry.source = as::manifest_path(relative);
            entry.guid = meta.guid;
            gather_inputs(relative, named, entry);

            const as::ManifestEntry* old = as::find_by_source(previous, entry.source);
            if (can_skip(options, relative, rule, entry, old)) {
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

        Named named;
        scan_gltf(options, sources, named);

        as::Manifest next;
        for (const std::filesystem::path& relative : sources) {
            // A glTF buffer is payload, not an asset. scan_gltf found it.
            if (named.buffers.contains(relative)) {
                continue;
            }

            as::ManifestEntry entry;
            switch (cook_source(options, relative, previous, named, entry)) {
            case Outcome::Cooked:
                next.entries.push_back(std::move(entry));
                ++result.cooked;
                break;
            case Outcome::Skipped:
                next.entries.push_back(std::move(entry));
                ++result.skipped;
                break;
            case Outcome::Failed:
                ++result.failed;
                break;
            }
        }

        if (!as::save_manifest(options.out, next)) {
            return false;
        }

        return result.failed == 0;
    }

} // namespace cooker
