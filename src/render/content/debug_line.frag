#version 450

layout(location = 0) in vec3 in_color;

layout(location = 0) out vec4 out_color;

void main() {
    // Written linear, and the swapchain is an 8-bit sRGB format, so the
    // hardware encodes it as it lands. physics/world.cpp decoded the Box3D
    // color into linear on the way in, so the round trip puts the color Box3D
    // named on the screen. See DESIGN.md section 3.
    out_color = vec4(in_color, 1.0);
}
