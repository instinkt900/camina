#include "platform/paths.h"

#include "core/log.h"

#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_stdinc.h>

namespace engine::platform {

    namespace {

        /// The directory name the cooker writes under, next to the executable.
        constexpr const char* kContentDirectory = "content";

        /// The organization SDL puts above the application name. It is the
        /// project name rather than the namespace. See DESIGN.md section 2.1.
        constexpr const char* kOrganization = "camina";

    } // namespace

    std::filesystem::path executable_directory() {
        // SDL owns this string and frees it when it shuts down, so this does
        // not free it. It also ends with a separator, which the path type
        // turns into an empty final component. Asking for the parent removes
        // that, so the result joins cleanly with a name.
        const char* base = SDL_GetBasePath();
        if (base == nullptr) {
            ENGINE_LOG_WARN("SDL will not say where the executable is. Using the working "
                            "directory instead.");
            return {};
        }
        return std::filesystem::path{ base }.parent_path();
    }

    std::filesystem::path cooked_content_root() {
        return executable_directory() / kContentDirectory;
    }

    std::filesystem::path preferences_directory(const char* application) {
        // Unlike SDL_GetBasePath, this string belongs to the caller. SDL also
        // creates the directory here, so a caller that gets a path can write
        // to it.
        char* pref = SDL_GetPrefPath(kOrganization, application);
        if (pref == nullptr) {
            ENGINE_LOG_WARN("SDL will not say where the settings of this user go, so nothing "
                            "will be saved.");
            return {};
        }
        // The same trailing separator executable_directory() removes.
        std::filesystem::path path = std::filesystem::path{ pref }.parent_path();
        SDL_free(pref);
        return path;
    }

} // namespace engine::platform
