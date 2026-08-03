#include "platform/paths.h"

#include "core/log.h"

#include <SDL3/SDL_filesystem.h>

namespace engine::platform {

    namespace {

        /// The directory name the cooker writes under, next to the executable.
        constexpr const char* kContentDirectory = "content";

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

} // namespace engine::platform
