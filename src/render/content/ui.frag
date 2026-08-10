#version 450

layout(location = 0) in vec2 in_uv;
layout(location = 1) in vec4 in_color;

layout(location = 0) out vec4 out_color;

// The image the run draws. A run that drew rectangles, gradients or an outline
// binds a single white texel instead, because white is the identity of the
// multiply below. One sampler for both kinds keeps the set layout the same for
// every run, so there is one pipeline rather than two.
//
// A cooked texture that holds colour is an sRGB format, so the hardware decodes
// on read and this value is already linear. Text is issue #199 and it will
// arrive through this same binding.
layout(set = 0, binding = 0) uniform sampler2D u_image;

// The colour arrives linear and interpolated. src/ui/renderer.cpp converts each
// vertex colour out of sRGB as it records it, because the swapchain is
// B8G8R8A8_SRGB and the hardware encodes linear to sRGB on write. Converting
// here instead would encode after interpolation, and a gradient between two
// colours would then follow the wrong curve between them.
void main() {
    out_color = in_color * texture(u_image, in_uv);
}
