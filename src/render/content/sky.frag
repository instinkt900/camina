#version 450

layout(location = 0) in vec3 in_direction;

layout(location = 0) out vec4 out_color;

// The same cubemap mesh.frag reflects, which scene::Environment names. This
// pass shows it directly, so a surface no longer reflects a sky the viewer
// cannot see.
layout(set = 0, binding = 0) uniform samplerCube environment_map;

void main() {
    // Level 0, which is the sharp environment. Every level above it is the same
    // sky blurred for one roughness, and the sky is not the reflection of
    // anything, so none of them applies here.
    //
    // The direction is normalized here rather than in the vertex stage. The
    // interpolation of a normalized vector is not itself normalized, so
    // normalizing first and interpolating after gives a shorter vector towards
    // the middle of each edge.
    vec3 direction = normalize(in_direction);

    // Linear light, into the half float scene target. The tonemap pass applies
    // exposure and the curve to this the same way it does to lit geometry, so
    // the sky and the surfaces reflecting it are exposed together.
    out_color = vec4(textureLod(environment_map, direction, 0.0).rgb, 1.0);
}
