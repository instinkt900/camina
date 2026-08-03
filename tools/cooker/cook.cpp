#include "cook.h"

#include "assets/manifest.h"
#include "assets/meta.h"
#include "assets/texture.h"
#include "core/log.h"
#include "texture.h"

#include <algorithm>
#include <cstdlib>
#include <system_error>
#include <vector>

namespace cooker {

    namespace {

        namespace as = engine::assets;

        /// What rule turns one source file into one cooked file.
        enum class Rule : std::uint8_t {
            Shader,  ///< GLSL through glslc, out as SPIR-V.
            Texture, ///< An image through stb, out as mip levels and BC7 blocks.
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
            return Rule::Copy;
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
            case Rule::Copy:
                break;
            }
            return "";
        }

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

        /// The cooked path for a source path, under the same relative directory.
        [[nodiscard]] std::filesystem::path cooked_name(const std::filesystem::path& relative,
                                                        Rule rule) {
            std::filesystem::path named = relative;
            named += cooked_suffix(rule);
            return named;
        }

        /// Runs the one rule that matches this asset.
        [[nodiscard]] bool cook_one(const Options& options, Rule rule,
                                    const std::filesystem::path& source,
                                    const std::filesystem::path& destination,
                                    const as::AssetMeta& meta) {
            switch (rule) {
            case Rule::Shader:
                return cook_shader(options, source, destination);
            case Rule::Texture:
                return cook_texture(source, destination, meta.texture);
            case Rule::Copy:
                break;
            }
            return copy_through(source, destination);
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
                out.push_back(relative);
            }

            std::ranges::sort(out);
            return true;
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

        as::Manifest next;
        for (const std::filesystem::path& relative : sources) {
            const std::filesystem::path source = options.content / relative;
            const Rule rule = rule_for(relative);

            as::AssetMeta meta;
            bool created = false;
            if (!as::meta_for(source, meta, &created)) {
                ++result.failed;
                continue;
            }

            // A new sidecar carries the defaults, and a texture wants a better
            // starting guess than "sRGB" for every file. So the guess goes in
            // once, when the file is written, and after that the file decides.
            // A wrong guess is one edit to fix and it never comes back.
            if (created && rule == Rule::Texture) {
                meta.texture.color_space = guess_color_space(source);
                if (!as::save_meta(source, meta)) {
                    ++result.failed;
                    continue;
                }
                ENGINE_LOG_INFO("{}: reading it as {}. Edit the sidecar to change that.",
                                source.string(), as::to_text(meta.texture.color_space));
            }

            as::ManifestEntry entry;
            entry.source = as::manifest_path(relative);
            entry.guid = meta.guid;
            entry.cooked = as::manifest_path(cooked_name(relative, rule));
            entry.inputs.push_back(entry.source);
            // The sidecar is an input, not only a place to keep the identity.
            // It carries the import settings, so flipping a texture from sRGB
            // to linear has to cook that texture again. Without this the edit
            // would look like it did nothing.
            entry.inputs.push_back(as::manifest_path(as::meta_path(relative)));

            // Skip only when the identity also matches. A sidecar somebody
            // replaced gives the asset a new identity, and every reference to
            // it has to see the new one.
            //
            // The input list has to match as well. is_fresh() hashes the inputs
            // the old entry names, so an entry written by an older cooker would
            // stay fresh forever against a list this build no longer uses.
            const as::ManifestEntry* old = as::find_by_source(previous, entry.source);
            if (!options.force && old != nullptr && old->guid == entry.guid &&
                old->cooked == entry.cooked && old->inputs == entry.inputs &&
                as::is_fresh(*old, options.content, options.out)) {
                next.entries.push_back(*old);
                ++result.skipped;
                continue;
            }

            const std::filesystem::path destination = options.out / entry.cooked;
            std::filesystem::create_directories(destination.parent_path(), error);

            const bool ok = cook_one(options, rule, source, destination, meta);
            if (!ok) {
                ++result.failed;
                continue;
            }

            if (!as::hash_inputs(options.content, entry.inputs, entry.hash)) {
                ENGINE_LOG_ERROR("{}: cooked, but an input would not read back.",
                                 source.string());
                ++result.failed;
                continue;
            }

            next.entries.push_back(std::move(entry));
            ++result.cooked;
        }

        if (!as::save_manifest(options.out, next)) {
            return false;
        }

        return result.failed == 0;
    }

} // namespace cooker
