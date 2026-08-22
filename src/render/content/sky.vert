#version 450

layout(location = 0) out vec3 out_direction;

// The camera, as the two things needed to turn a pixel back into a world-space
// ray. The vertex stage is the only reader, so the fragment stage never
// declares this block.
layout(push_constant) uniform Push {
    // Clip space back to world space, which is the inverse of the matrix the
    // frame draws with.
    mat4 world_from_clip;
    // Where the camera is. w is unused and keeps the block aligned.
    vec4 camera_position;
} push;

void main() {
    // The same full-screen triangle tonemap.vert builds, and for the same
    // reason. See that file for why it is one triangle and not two.
    vec2 uv = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    vec2 ndc = (uv * 2.0) - 1.0;

    // Zero is the far plane under reverse-Z. The sky sits exactly there, so it
    // passes the depth test where the image still holds its clear and fails
    // against every fragment of geometry. That needs a test that lets an equal
    // value through, which gfx::GraphicsPipelineDesc::depth_equal asks for.
    gl_Position = vec4(ndc, 0.0, 1.0);

    // The ray is unprojected at the near plane, which is 1 under reverse-Z, and
    // never at the far plane. The projection is infinite, so the far plane is
    // where it puts infinity and unprojecting there divides by a w of zero.
    //
    // Interpolating the result across the triangle is exact rather than an
    // approximation. Unprojecting at one fixed depth is an affine map of the
    // screen position, because the w it divides by depends on that depth alone.
    vec4 near = push.world_from_clip * vec4(ndc, 1.0, 1.0);
    out_direction = (near.xyz / near.w) - push.camera_position.xyz;
}
