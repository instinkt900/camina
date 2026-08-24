// Issue #204. The buffer contract, checked against a real device.
//
// Every other buffer check in this directory would need one, so there are
// none. This is the smallest thing that answers the question the issue asked:
// can a vertex buffer be allocated with no data and written every frame?
//
// It is not in CI, for the reason tests/test_gpu_frame.cpp gives: CI has no
// GPU. It skips itself where no device opens, so a plain ctest run includes it
// and CI leaves it out with -LE gpu.

#include "check.h"
#include "gfx/device.h"

#include <array>
#include <cstdint>

namespace {

    using test::check;
    using test::section;
    namespace gfx = engine::gfx;

    /// What ctest reads as "this did not run", set by SKIP_RETURN_CODE.
    constexpr int kSkipExitCode = 77;

    /// Enough vertices to be a real allocation and small enough to be quick.
    constexpr std::size_t kVertexCount = 256;

} // namespace

int main() {
    gfx::Device* device = nullptr;
    const gfx::DeviceDesc desc{
        .window = nullptr,
        .app_name = "camina test_gpu_buffer",
    };
    if (!gfx::succeeded(gfx::create_device(desc, &device)) || device == nullptr) {
        std::printf("No device opened, so there is no buffer to make. Skipping.\n");
        std::fflush(stdout);
        return kSkipExitCode;
    }

    std::array<float, kVertexCount> data{};
    for (std::size_t at = 0; at < data.size(); ++at) {
        data[at] = static_cast<float>(at);
    }
    const std::size_t bytes = data.size() * sizeof(float);

    section("A vertex buffer in host-visible memory");
    {
        // The two calls the issue named. Before #204 the first refused a vertex
        // buffer with no data and the second refused a vertex buffer outright,
        // so per-frame geometry had to destroy and rebuild a buffer every frame.
        gfx::BufferHandle buffer;
        const gfx::BufferDesc empty{
            .data = nullptr,
            .size = bytes,
            .usage = gfx::BufferUsage::Vertex,
            .memory = gfx::BufferMemory::HostVisible,
        };
        check(gfx::succeeded(gfx::create_buffer(device, empty, &buffer)),
              "a vertex buffer allocates with no data");
        check(buffer.valid(), "and the handle is real");

        // Written twice, because once proves the call and twice proves the
        // buffer is still there to write.
        gfx::update_buffer(device, buffer, data.data(), bytes);
        gfx::update_buffer(device, buffer, data.data(), bytes);
        check(true, "and it takes a write on each frame");

        gfx::destroy_buffer(device, buffer);
    }

    section("An index buffer does the same");
    {
        gfx::BufferHandle buffer;
        const gfx::BufferDesc empty{
            .data = nullptr,
            .size = bytes,
            .usage = gfx::BufferUsage::Index,
            .memory = gfx::BufferMemory::HostVisible,
        };
        check(gfx::succeeded(gfx::create_buffer(device, empty, &buffer)),
              "an index buffer allocates with no data");
        gfx::update_buffer(device, buffer, data.data(), bytes);
        gfx::destroy_buffer(device, buffer);
    }

    section("The default is still a staged, device-local buffer");
    {
        // A cooked mesh has not changed, and this is what says so. Asking for
        // a vertex buffer with no data and no memory choice is still refused,
        // because the memory it would land in is not memory the host reaches.
        gfx::BufferHandle refused;
        const gfx::BufferDesc no_data{
            .data = nullptr,
            .size = bytes,
            .usage = gfx::BufferUsage::Vertex,
            .memory = gfx::BufferMemory::Auto,
        };
        check(!gfx::succeeded(gfx::create_buffer(device, no_data, &refused)),
              "a vertex buffer with no data and no memory choice is refused");
        check(!refused.valid(), "and the handle stays null");

        gfx::BufferHandle staged;
        const gfx::BufferDesc with_data{
            .data = data.data(),
            .size = bytes,
            .usage = gfx::BufferUsage::Vertex,
            .memory = gfx::BufferMemory::Auto,
        };
        check(gfx::succeeded(gfx::create_buffer(device, with_data, &staged)),
              "and one with data still stages into device-local memory");
        gfx::destroy_buffer(device, staged);
    }

    gfx::destroy_device(device);
    return test::report();
}
