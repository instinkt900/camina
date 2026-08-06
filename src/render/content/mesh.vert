#version 450

// The cooked mesh vertex layout, from src/assets/mesh.h. The stride is 48
// bytes and the order here must match MeshVertex exactly.
layout(location = 0) in vec3 in_position;
layout(location = 1) in vec3 in_normal;
// The tangent has been in the cooked mesh since M4.4a and nothing read it until
// now. Normal mapping needs it, and w carries the handedness that says which
// way the bitangent points.
layout(location = 2) in vec4 in_tangent;
layout(location = 3) in vec2 in_uv;

layout(location = 0) out vec3 out_world;
layout(location = 1) out vec3 out_normal;
layout(location = 2) out vec4 out_tangent;
layout(location = 3) out vec2 out_uv;

// Set 0 changes once for each frame and set 1 changes for each material, so the
// lower number is the one that changes less often.
layout(set = 0, binding = 0) uniform Frame {
    mat4 view_projection;
    vec4 camera_position; // w is unused and keeps the block aligned.
} frame;

// The model matrix alone. The view projection moved into the frame block above,
// because the shading needs the camera position and 128 bytes is the smallest
// push constant block Vulkan guarantees.
layout(push_constant) uniform Push {
    mat4 model;
} push;

void main() {
    vec4 world = push.model * vec4(in_position, 1.0);
    gl_Position = frame.view_projection * world;
    out_world = world.xyz;

    // The inverse transpose of the model matrix correctly transforms normals
    // under non-uniform scale. mat3(model) is right only for rotation and
    // uniform scale.
    mat3 to_world = mat3(transpose(inverse(push.model)));
    out_normal = normalize(to_world * in_normal);
    out_tangent = vec4(normalize(to_world * in_tangent.xyz), in_tangent.w);
    out_uv = in_uv;
}
