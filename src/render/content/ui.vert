#version 450

// A moth_ui quad, already in screen space. src/ui/renderer.cpp applies the
// node transform on the CPU as it records each corner, so this stage has no
// matrix at all. That is what lets a whole layout collapse into one draw:
// a transform change costs nothing here, so it never breaks a batch.
layout(location = 0) in vec2 in_position;
layout(location = 1) in vec4 in_color;

layout(location = 0) out vec4 out_color;

layout(push_constant) uniform Push {
    // One over the logical size, so this stage needs one multiply rather than
    // a divide for each vertex.
    vec2 inv_logical_size;
} push;

void main() {
    out_color = in_color;

    // Screen space is pixels with the origin at the top left and y down.
    // Vulkan puts NDC -1 at the top, so the two already agree and nothing
    // flips. Compare tonemap.vert, which relies on the same thing.
    gl_Position = vec4((in_position * push.inv_logical_size * 2.0) - 1.0, 0.0, 1.0);
}
