#version 450

layout(location = 0) out vec2 out_uv;

// One triangle that covers the whole screen, built from the vertex index alone.
// There is no vertex buffer and no draw data at all, so the pass costs one
// vkCmdDraw of three vertices.
//
// A triangle rather than two of them. A quad puts a seam down the diagonal, and
// the hardware rasterizes the shared edge in both halves, which does the work
// twice along that line for no gain.
//
// Vertex 0 lands at the top left, 1 reaches past the right edge, and 2 reaches
// past the bottom. Vulkan puts NDC -1 at the top, and texel (0, 0) of the
// attachment is the top left as well, so the two agree and nothing flips.
void main() {
    out_uv = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    gl_Position = vec4((out_uv * 2.0) - 1.0, 0.0, 1.0);
}
