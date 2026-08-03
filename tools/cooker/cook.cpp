#include "cook.h"

#include "assets/manifest.h"
#include "assets/meta.h"
#include "core/log.h"

#include <algorithm>
#include <cstdlib>
#include <system_error>
#include <vector>

namespace cooker {

    namespace {

        namespace as = engine::assets;

        /// What rule turns one source file into one cooked file.
        enum class Rule : std::uint8_t {
            Shader, ///< GLSL through glslc, out as SPIR-V.
            Copy,   ///< No rule yet. The bytes go through unchanged.
        };

        [[nodiscard]] Rule rule_for(const std::filesystem::path& source) {
            const std::string extension = source.extension().string();
            if (extension == ".vert" || extension == ".frag" || extension == ".comp") {
                return Rule::Shader;
            }
            return Rule::Copy;
        }

        /// Wraps an argument so a shell treats it as one word.
        [[nodiscard]] std::string quoted(const std::string& text) {
            return "\"" + text + "\"";
        }

        /**
         * Runs glslc over one shader.
         *
         * This spawns a process rather than linking libshaderc. The build
         * already finds glslc for the rule this replaces, so it costs no new
         * dependency. The trade is the error text: glslc writes it to stderr
         * and the cooker passes it through rather than reading it. Link
         * libshaderc instead when that stops being good enough.
         */
        [[nodiscard]] bool cook_shader(const Options& options,
                                       const std::filesystem::path& source,
                                       const std::filesystem::path& destination) {
            // --target-env matches the Vulkan version the device asks for, and
            // -O is the same optimization the old CMake rule used. No -mfmt,
            // because the cooked file is now SPIR-V rather than a C header.
            const std::string command = quoted(options.glslc) + " --target-env=vulkan1.3 -O -o " +
                                        quoted(destination.string()) + " " +
                                        quoted(source.string());

            const int status = std::system(command.c_str());
            if (status != 0) {
                ENGINE_LOG_ERROR("{}: glslc returned {}.", source.string(), status);
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
            if (rule != Rule::Shader) {
                return relative;
            }
            // cube.vert becomes cube.vert.spv, so cube.vert and cube.frag stay
            // two files rather than collapsing onto one name.
            std::filesystem::path named = relative;
            named += ".spv";
            return named;
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
            if (!as::meta_for(source, meta)) {
                ++result.failed;
                continue;
            }

            as::ManifestEntry entry;
            entry.source = as::manifest_path(relative);
            entry.guid = meta.guid;
            entry.cooked = as::manifest_path(cooked_name(relative, rule));
            entry.inputs.push_back(entry.source);

            // Skip only when the identity also matches. A sidecar somebody
            // replaced gives the asset a new identity, and every reference to
            // it has to see the new one.
            const as::ManifestEntry* old = as::find_by_source(previous, entry.source);
            if (!options.force && old != nullptr && old->guid == entry.guid &&
                old->cooked == entry.cooked &&
                as::is_fresh(*old, options.content, options.out)) {
                next.entries.push_back(*old);
                ++result.skipped;
                continue;
            }

            const std::filesystem::path destination = options.out / entry.cooked;
            std::filesystem::create_directories(destination.parent_path(), error);

            const bool ok = rule == Rule::Shader ? cook_shader(options, source, destination)
                                                 : copy_through(source, destination);
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
