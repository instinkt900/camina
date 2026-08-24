// Issue #60. The deferred free queue, checked against a real device.
//
// A reload used to wait for the whole device before it freed anything, because
// a frame the GPU has not finished may still read the buffer or the texture
// about to go. The queue frees behind the frames instead, so nothing stalls.
//
// The thing worth testing is the *delay*, not the free. A queue that freed at
// once would pass every check about the resource being gone in the end, and it
// would be the use-after-free the wait existed to stop. So each section reads
// gfx::retired_count between the frames.
//
// It is not in CI, for the reason tests/test_gpu_frame.cpp gives: CI has no
// GPU. It skips itself where no device opens, so a plain ctest run includes it
// and CI leaves it out with -LE gpu.

#include "check.h"
#include "gfx/device.h"

#include <cstddef>
#include <cstdio>


namespace {

    using test::check;
    using test::section;
    namespace gfx = engine::gfx;

    /// What ctest reads as "this did not run", set by SKIP_RETURN_CODE.
    constexpr int kSkipExitCode = 77;

    /// Small enough to be quick, large enough to be a real allocation.
    constexpr std::size_t kFloatCount = 64;

    /// Runs one frame that records nothing, so the counter moves.
    ///
    /// The queue is swept in begin_frame, after the fence of the slot it is
    /// about to reuse. A frame that draws nothing sweeps just as well as one
    /// that draws, which is what keeps this test free of a pipeline.
    ///
    /// @param device The device to drive.
    /// @return True when the frame opened and closed.
    [[nodiscard]] bool run_frame(gfx::Device* device) {
        gfx::FrameInfo frame;
        if (!gfx::succeeded(gfx::begin_frame(device, &frame))) {
            return false;
        }

        // begin_frame leaves both targets in ResourceState::Undefined, and
        // end_frame presents from ColorTarget. Nothing is drawn here, so this
        // is the whole of the transition a frame owes. Without it the
        // validation layer reports every frame, and a test that prints its own
        // errors cannot show anybody a real one.
        gfx::cmd_frame_barrier(frame.commands, gfx::FrameTarget::Color,
                               gfx::ResourceState::Undefined, gfx::ResourceState::ColorTarget);
        return gfx::succeeded(gfx::end_frame(device));
    }

    /// A host-visible vertex buffer with nothing in it.
    [[nodiscard]] gfx::BufferHandle make_buffer(gfx::Device* device) {
        gfx::BufferHandle buffer;
        const gfx::BufferDesc desc{
            .data = nullptr,
            .size = kFloatCount * sizeof(float),
            .usage = gfx::BufferUsage::Vertex,
            .memory = gfx::BufferMemory::HostVisible,
        };
        if (!gfx::succeeded(gfx::create_buffer(device, desc, &buffer))) {
            return gfx::BufferHandle{};
        }
        return buffer;
    }

} // namespace

int main() {
    gfx::Device* device = nullptr;
    const gfx::DeviceDesc desc{
        .window = nullptr,
        .app_name = "camina test_gpu_retire",
        .enable_validation = true,
    };
    if (!gfx::succeeded(gfx::create_device(desc, &device)) || device == nullptr) {
        std::printf("No device opened, so there is nothing to retire. Skipping.\n");
        std::fflush(stdout);
        return kSkipExitCode;
    }

    section("A retired buffer waits for the frames to move past it");
    {
        check(gfx::retired_count(device) == 0, "the queue starts empty");

        const gfx::BufferHandle buffer = make_buffer(device);
        check(buffer.valid(), "a buffer was created");

        gfx::retire_buffer(device, buffer);
        check(gfx::retired_count(device) == 1, "retiring it holds it rather than freeing it");

        // One frame is not enough. The frame that retired it may still be the
        // one in flight, so a queue that freed here would be the use-after-free
        // the wait used to stop.
        check(run_frame(device), "a frame runs");
        check(gfx::retired_count(device) == 1, "and one frame does not release it");

        check(run_frame(device), "a second frame runs");
        check(gfx::retired_count(device) == 0, "the second one does");
    }

    section("Every kind goes through the same queue");
    {
        const gfx::BufferHandle first = make_buffer(device);
        const gfx::BufferHandle second = make_buffer(device);
        check(first.valid() && second.valid(), "two more buffers were created");

        gfx::retire_buffer(device, first);
        gfx::retire_buffer(device, second);
        check(gfx::retired_count(device) == 2, "both are held");

        // A null handle is not a resource, so it must not take a slot in the
        // queue. Several callers retire a handle they may never have built.
        gfx::retire_buffer(device, gfx::BufferHandle{});
        gfx::retire_texture(device, gfx::TextureHandle{});
        gfx::retire_pipeline(device, gfx::PipelineHandle{});
        gfx::retire_descriptor_set(device, gfx::DescriptorSetHandle{});
        check(gfx::retired_count(device) == 2, "and a null handle takes no slot");

        check(run_frame(device) && run_frame(device), "two frames run");
        check(gfx::retired_count(device) == 0, "and both are released together");
    }

    section("A retire between two frames waits for its own two");
    {
        check(run_frame(device), "a frame runs first");

        const gfx::BufferHandle buffer = make_buffer(device);
        check(buffer.valid(), "a buffer was created after it");
        gfx::retire_buffer(device, buffer);

        // The counter this was recorded against is the one the frame above
        // raised, so the release is two frames from here and not two from the
        // start of the run. A queue keyed on the ring slot rather than on a
        // counter that only goes up would get this wrong.
        check(run_frame(device), "one frame after the retire");
        check(gfx::retired_count(device) == 1, "does not release it");
        check(run_frame(device), "and a second one");
        check(gfx::retired_count(device) == 0, "does");
    }

    section("A device that stops drawing still frees what it holds");
    {
        const gfx::BufferHandle buffer = make_buffer(device);
        check(buffer.valid(), "one last buffer was created");
        gfx::retire_buffer(device, buffer);
        check(gfx::retired_count(device) == 1, "and it is held");

        // No frame follows. destroy_device waits for the GPU and then empties
        // the queue, so an application that stops drawing leaks nothing. The
        // validation layer is on, and it reports a resource that outlives its
        // device.
    }

    gfx::destroy_device(device);
    return test::report();
}
