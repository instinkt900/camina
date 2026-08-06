#version 450

layout(location = 0) in vec2 in_uv;

layout(location = 0) out vec4 out_color;

// What the mesh pass rendered, in half float. A value here can be far above 1,
// which is the whole reason this pass exists.
layout(set = 0, binding = 0) uniform sampler2D scene_color;

void main() {
    vec3 color = texture(scene_color, in_uv).rgb;

    // No curve yet. M5.6b puts the ACES fit here, and this half exists to show
    // that the extra target and the extra pass move no pixel. Issue #142.
    //
    // The swapchain is an sRGB format, so the hardware converts this from
    // linear as it lands. Nothing here encodes sRGB by hand, per DESIGN.md
    // section 3.
    out_color = vec4(color, 1.0);
}
