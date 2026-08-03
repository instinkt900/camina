#version 450

// The texture uses an sRGB format, so the sampler returns linear values. The
// swapchain converts back on the final write. See DESIGN.md section 3.

layout(set = 0, binding = 0) uniform sampler2D albedo;

layout(location = 0) in vec2 in_uv;
layout(location = 0) out vec4 out_fragment;

void main() {
    out_fragment = texture(albedo, in_uv);
}
