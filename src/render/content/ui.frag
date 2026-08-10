#version 450

layout(location = 0) in vec4 in_color;

layout(location = 0) out vec4 out_color;

// The colour arrives linear and interpolated. src/ui/renderer.cpp converts each
// vertex colour out of sRGB as it records it, because the swapchain is
// B8G8R8A8_SRGB and the hardware encodes linear to sRGB on write. Converting
// here instead would encode after interpolation, and a gradient between two
// colours would then follow the wrong curve between them.
//
// This stage has no texture. Images are issue #198 and text is #199, and both
// add a sampler here.
void main() {
    out_color = in_color;
}
