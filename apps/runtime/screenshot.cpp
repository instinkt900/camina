#include "screenshot.h"

#include "core/log.h"

// stb_image_write carries its own implementation and exactly one translation
// unit must ask for it. This is that unit. The header is third-party code we do
// not patch, so tools/cooker/CMakeLists.txt and this file both keep the warning
// set off the implementation. Rule 4.4 keeps it in the Conan cache.
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#include <stb_image_write.h>

#include <cstdint>
#include <cstdio>
#include <vector>

namespace runtime {

    namespace {

        /// Collects what stb writes, so this file owns the file handling.
        void collect(void* context, void* data, int size) {
            auto* out = static_cast<std::vector<unsigned char>*>(context);
            const auto* first = static_cast<const unsigned char*>(data);
            out->insert(out->end(), first, first + size);
        }

    } // namespace

    bool write_screenshot(engine::gfx::Device* device, const std::filesystem::path& path) {
        // Ask for the size first, so the buffer is exactly right.
        engine::gfx::Extent2D extent{};
        engine::gfx::Result result = engine::gfx::capture_frame(device, nullptr, 0, &extent);
        if (!engine::gfx::succeeded(result) || extent.width == 0 || extent.height == 0) {
            ENGINE_LOG_ERROR("There is no frame to capture.");
            return false;
        }

        constexpr std::size_t kBytesPerPixel = 4;
        std::vector<std::uint8_t> pixels(static_cast<std::size_t>(extent.width) * extent.height *
                                         kBytesPerPixel);
        result = engine::gfx::capture_frame(device, pixels.data(), pixels.size(), &extent);
        if (!engine::gfx::succeeded(result)) {
            return false;
        }

        std::vector<unsigned char> png;
        const int stride = static_cast<int>(extent.width * kBytesPerPixel);
        if (stbi_write_png_to_func(collect, &png, static_cast<int>(extent.width),
                                   static_cast<int>(extent.height),
                                   static_cast<int>(kBytesPerPixel), pixels.data(),
                                   stride) == 0) {
            ENGINE_LOG_ERROR("The screenshot would not encode.");
            return false;
        }

        // std::FILE rather than a stream, because stb hands over a byte buffer
        // and this only has to put it on disk.
        std::FILE* file = std::fopen(path.string().c_str(), "wb");
        if (file == nullptr) {
            ENGINE_LOG_ERROR("Could not open {} for writing.", path.string());
            return false;
        }
        const std::size_t written = std::fwrite(png.data(), 1, png.size(), file);
        const bool closed = std::fclose(file) == 0;
        if (written != png.size() || !closed) {
            ENGINE_LOG_ERROR("The screenshot did not finish writing to {}.", path.string());
            return false;
        }

        ENGINE_LOG_INFO("Wrote a {} by {} screenshot to {}.", extent.width, extent.height,
                        path.string());
        return true;
    }

} // namespace runtime
