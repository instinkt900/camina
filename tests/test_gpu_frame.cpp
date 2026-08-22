// Issue #190. The one check that renders a frame.
//
// Every other test in this directory runs on the CPU. That is a deliberate
// trade and README.md gives the reason: CI has no GPU, and a software
// rasterizer produces pixels that are those of no real driver. This check does
// not argue with that. It is not in CI. It is a target a person runs, and it
// exists because the engine otherwise has no check at all on the one thing it
// is for.
//
// #188 is what it answers. Every graphics pipeline declares dynamic cull mode,
// so the tonemap pass inherited back face culling from the mesh pass and its
// one full-screen triangle was culled. A scene of opaque geometry alone
// rendered pure black. The validation layer was happy, synchronization
// validation was happy, and every test passed, because no test drew anything.
//
// It drives the shipping binaries rather than assembling a frame of its own. A
// frame put together here would be a frame that nobody ships, and #188 lived in
// exactly the ground between the passes that such a test would have rebuilt.

#include "check.h"
#include "gfx/device.h"
#include "platform/paths.h"
#include "platform/process.h"

// The declarations only. engine::import already carries the one translation
// unit that holds the implementation, and it carries the warning suppressions
// that go with it. See src/import/stb_image_impl.cpp.
#include <stb_image.h>

#include <cstdio>
#include <filesystem>
#include <random>
#include <set>
#include <string>
#include <vector>

namespace {

    using test::check;
    using test::section;
    namespace fs = std::filesystem;

    /// What ctest reads as "this did not run", set by SKIP_RETURN_CODE.
    constexpr int kSkipExitCode = 77;

    /// Small, because the check reads every pixel and none of it needs to be
    /// large enough to look at.
    constexpr int kWidth = 640;
    constexpr int kHeight = 360;

    /// How many frames to render before the capture. Enough for the caches to
    /// fill and the first frame's uploads to land.
    constexpr const char* kFrames = "8";

    /// How many distinct colours a frame that drew a shaded scene must hold.
    constexpr std::size_t kShadedFloor = 32;

    /**
     * @brief What one capture came back as.
     */
    struct Frame {
        bool read = false;        ///< True when the file decoded.
        std::size_t pixels = 0;   ///< How many pixels it holds.
        std::size_t distinct = 0; ///< How many distinct colours it holds.
    };

    /// Whether a device opens on this machine at all.
    ///
    /// This is the skip condition, and it is asked of the engine rather than
    /// guessed from a binary's exit code. A machine with no GPU, no driver, or
    /// no display is a machine this check cannot run on, and that is not a
    /// failure of the branch under test.
    [[nodiscard]] bool device_opens() {
        engine::gfx::Device* device = nullptr;
        const engine::gfx::DeviceDesc desc{
            .window = nullptr,
            .app_name = "camina test_gpu_frame",
        };
        if (!engine::gfx::succeeded(engine::gfx::create_device(desc, &device)) ||
            device == nullptr) {
            return false;
        }
        engine::gfx::destroy_device(device);
        return true;
    }

    /// The name a program has on this platform.
    [[nodiscard]] fs::path program(std::string_view stem) {
#if defined(_WIN32)
        return engine::platform::executable_directory() / (std::string{ stem } + ".exe");
#else
        return engine::platform::executable_directory() / stem;
#endif
    }

    /// Reads a capture and counts what is in it.
    ///
    /// The count of distinct colours is the measure rather than a variance or a
    /// standard deviation. A black frame and a frame of one flat fill both come
    /// back as 1, and that is the whole of what #188 looked like.
    [[nodiscard]] Frame read_frame(const fs::path& file) {
        Frame frame;
        int width = 0;
        int height = 0;
        int channels = 0;
        unsigned char* pixels = stbi_load(file.string().c_str(), &width, &height, &channels, 3);
        if (pixels == nullptr || width <= 0 || height <= 0) {
            return frame;
        }

        std::set<unsigned int> colours;
        const std::size_t count = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
        for (std::size_t at = 0; at < count; ++at) {
            const unsigned int r = pixels[(at * 3) + 0];
            const unsigned int g = pixels[(at * 3) + 1];
            const unsigned int b = pixels[(at * 3) + 2];
            colours.insert((r << 16U) | (g << 8U) | b);
        }
        stbi_image_free(pixels);

        frame.read = true;
        frame.pixels = count;
        frame.distinct = colours.size();
        return frame;
    }

    /// Runs one binary offscreen and reads what it drew.
    [[nodiscard]] Frame capture(const fs::path& binary, const std::vector<std::string>& arguments,
                                const fs::path& out) {
        std::error_code ignored;
        fs::remove(out, ignored);

        std::vector<std::string> full = arguments;
        full.emplace_back("--offscreen");
        full.emplace_back("--resolution");
        full.emplace_back(std::to_string(kWidth) + "x" + std::to_string(kHeight));
        full.emplace_back("--screenshot");
        full.emplace_back(out.string());

        const engine::platform::ProcessResult result =
            engine::platform::run_process(binary, full);
        if (!result.ran || result.exit_code != 0) {
            std::printf("  note  %s did not run cleanly, exit %d\n",
                        binary.filename().string().c_str(), result.exit_code);
            std::fflush(stdout);
            return Frame{};
        }
        return read_frame(out);
    }

} // namespace

int main(int argc, char** argv) {
    // The source content tree, which CMake passes because a test binary has no
    // way to find the repository it was built from.
    const fs::path source = argc > 1 ? fs::path{ argv[1] } : fs::path{};

    if (!device_opens()) {
        std::printf("No device opened, so there is no frame to check. Skipping.\n");
        std::fflush(stdout);
        return kSkipExitCode;
    }

    // A name of its own for each run. Seven other tests here take a fixed one
    // and two concurrent runs of the same binary then delete each other's
    // fixtures, which is issue #293. This does not add an eighth.
    const fs::path scratch =
        fs::temp_directory_path() / ("camina_gpu_frame_" + std::to_string(std::random_device{}()));
    std::error_code ignored;
    fs::create_directories(scratch, ignored);

    section("The editor draws the scene alone");
    // The sharp check of the two. An editor capture carries no game UI and no
    // ImGui overlay, so every pixel of it came from the scene. A frame of one
    // colour therefore means the scene did not reach the picture, which is
    // exactly the shape of #188.
    const fs::path editor = program("editor");
    if (fs::exists(editor)) {
        const Frame frame = capture(editor, { "--content", source.string() },
                                    scratch / "editor.png");
        check(frame.read, "the editor wrote a capture");
        if (frame.read) {
            std::printf("  note  %zu distinct colours over %zu pixels\n", frame.distinct,
                        frame.pixels);
            std::fflush(stdout);
            check(frame.distinct > 1, "the editor frame is not one flat colour");
            // The floor is low on purpose. This catches a picture that
            // collapsed rather than pinning the shading to a number that every
            // driver would argue with. The scene measured 145 when this was
            // written, and a collapse gives one or two. A frame that drew its
            // geometry but lost the shading gives about one for each material,
            // which is seven here, so the floor is above that as well.
            check(frame.distinct >= kShadedFloor, "the editor frame holds a shaded scene");
        }
    } else {
        std::printf("  note  no editor binary, so the scene-alone check is skipped\n");
        std::fflush(stdout);
    }

    section("The runtime draws the scene it ships");
    // The weaker of the two, and worth having because it is the path that
    // ships. It cannot be as sharp: the game UI probe draws over the tonemapped
    // image, so this frame is varied whatever the scene did. Issue #200 takes
    // that probe away, and this check gets sharper for free when it does.
    const fs::path runtime = program("runtime");
    check(fs::exists(runtime), "the runtime binary is there");
    if (fs::exists(runtime)) {
        const fs::path cooked = engine::platform::cooked_content_root() / "test";
        check(fs::is_directory(cooked), "the cooked test content tree is there");
        const Frame frame = capture(
            runtime, { "--content", cooked.string(), "--frames", kFrames, "--no-watch" },
            scratch / "runtime.png");
        check(frame.read, "the runtime wrote a capture");
        if (frame.read) {
            std::printf("  note  %zu distinct colours over %zu pixels\n", frame.distinct,
                        frame.pixels);
            std::fflush(stdout);
            check(frame.distinct > 1, "the runtime frame is not one flat colour");
        }
    }

    test::remove_tree(scratch);
    return test::report();
}
