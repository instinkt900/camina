#version 450

// One push constant holds the combined model-view-projection matrix. The
// projection comes from perspective_reverse_z in math/conventions.h, so the near
// plane maps to depth 1 and the far plane to depth 0.

layout(push_constant) uniform Push {
    mat4 mvp;
} push;

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec2 in_uv;

layout(location = 0) out vec2 out_uv;

void main() {
    gl_Position = push.mvp * vec4(in_position, 1.0);
    out_uv = in_uv;
}
