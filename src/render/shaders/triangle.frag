#version 450

// The interpolated color arrives in the linear working space. The swapchain uses
// an sRGB format, so the driver converts on the write. See DESIGN.md section 3.

layout(location = 0) in vec3 in_color;
layout(location = 0) out vec4 out_fragment;

void main() {
    out_fragment = vec4(in_color, 1.0);
}
