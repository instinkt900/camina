#version 450

// The cooked mesh vertex layout, from src/assets/mesh.h. The stride is 48
// bytes and the order here must match MeshVertex exactly.
layout(location = 0) in vec3 in_position;
layout(location = 1) in vec3 in_normal;
// The tangent sits at offset 24 in the vertex and nothing reads it yet. An
// attribute a shader does not consume is a validation warning, so the pipeline
// declares three of the four. M4.4b adds it back with the normal mapping that
// needs it. The stride stays 48 either way.
layout(location = 3) in vec2 in_uv;

layout(location = 0) out vec3 out_normal;
layout(location = 1) out vec2 out_uv;

// 128 bytes, which is the smallest push constant block Vulkan guarantees.
// Anything more would need a uniform buffer.
layout(push_constant) uniform Push {
    mat4 view_projection;
    mat4 model;
} push;

void main() {
    vec4 world = push.model * vec4(in_position, 1.0);
    gl_Position = push.view_projection * world;

    // The inverse transpose would be correct for a non-uniform scale. Nothing
    // in the sandbox scales a mesh unevenly, and a mat3 of the model matrix is
    // right for every uniform scale and rotation. M5 revisits this when the
    // material system decides what a normal means.
    out_normal = normalize(mat3(push.model) * in_normal);
    out_uv = in_uv;
}
