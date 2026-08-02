#version 450

// The triangle comes from gl_VertexIndex, so this needs no vertex buffer and no
// vertex input state. M1.3 replaces it with a real mesh.
//
// Reverse-Z puts the near plane at 1 and the far plane at 0, per DESIGN.md
// section 3. This shader writes a fixed depth of 0.5, which passes either way.

layout(location = 0) out vec3 out_color;

vec2 positions[3] = vec2[](
    vec2( 0.0, -0.6),
    vec2( 0.6,  0.6),
    vec2(-0.6,  0.6)
);

// Written in the linear working space. The sRGB swapchain converts on the final
// write, so do not pre-convert here.
vec3 colors[3] = vec3[](
    vec3(1.0, 0.0, 0.0),
    vec3(0.0, 1.0, 0.0),
    vec3(0.0, 0.0, 1.0)
);

void main() {
    gl_Position = vec4(positions[gl_VertexIndex], 0.5, 1.0);
    out_color = colors[gl_VertexIndex];
}
