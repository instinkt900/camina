#version 450

// The cooked mesh vertex layout, from src/assets/mesh.h. This pass reads
// position alone, and the other three attributes are still part of the stride
// because the cooked stream is interleaved. Issue #87 records what that costs
// and whether a position-only stream is worth a mesh format version.
layout(location = 0) in vec3 in_position;

// Both matrices ride in the push constant block, which is 128 bytes and so
// exactly the smallest size Vulkan guarantees. That leaves the pass with no
// descriptor set at all, which is most of the reason it is cheap.
//
// The light matrix repeats for every draw, which costs 64 bytes each. The
// cascades in #135 need an array of them and will not fit here, so that is
// where this becomes a uniform block.
layout(push_constant) uniform Push {
    mat4 light_view_projection;
    mat4 model;
} push;

// No fragment stage. The depth write is fixed-function work and nothing
// consumes a color, so the pipeline declares no fragment module at all. See
// gfx::GraphicsPipelineDesc::depth_only.
void main() {
    gl_Position = push.light_view_projection * push.model * vec4(in_position, 1.0);
}
