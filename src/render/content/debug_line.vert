#version 450

// The physics wireframe. Each vertex is already in world space, because
// physics::World::debug_lines transforms the cached shape wireframes as it
// collects them. So this stage is one matrix multiply and nothing else.
layout(location = 0) in vec3 in_position;
// Linear, and it stays linear all the way to the swapchain write. See the
// note in debug_line.frag.
layout(location = 1) in vec3 in_color;

layout(location = 0) out vec3 out_color;

layout(push_constant) uniform Push {
    // Clip from world. A push constant rather than the frame block, because
    // this pass draws after the tonemap and binds no descriptor set at all.
    mat4 clip_from_world;
} push;

void main() {
    gl_Position = push.clip_from_world * vec4(in_position, 1.0);
    out_color = in_color;
}
