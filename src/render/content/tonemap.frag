#version 450

layout(location = 0) in vec2 in_uv;

layout(location = 0) out vec4 out_color;

// What the mesh pass rendered, in half float. A value here can be far above 1,
// which is the whole reason this pass exists.
layout(set = 0, binding = 0) uniform sampler2D scene_color;

layout(push_constant) uniform Push {
    // A linear scale on the scene before the curve. One is neutral.
    float exposure;
} push;

// The ACES fit from Stephen Hill, which is two colour space changes around one
// rational curve. The cheaper single-function fit from Krzysztof Narkowicz was
// the other candidate, and it loses saturation in the highlights. An interior
// of coloured walls is the case that shows that, so the more faithful fit is
// worth two matrix products in a pass that runs once for each pixel.
//
// Both matrices are written as columns, because that is what the GLSL mat3
// constructor takes. The published form lists rows, so these read as the
// transpose of it and are not.
const mat3 kAcesInput = mat3(0.59719, 0.07600, 0.02840,  //
                             0.35458, 0.90834, 0.13383,  //
                             0.04823, 0.01566, 0.83777);

const mat3 kAcesOutput = mat3(1.60475, -0.10208, -0.00327,  //
                              -0.53108, 1.10813, -0.07276,  //
                              -0.07367, -0.00605, 1.07602);

// The reference rendering transform and the output transform together, fit to
// one rational function for each channel.
vec3 rrt_and_odt_fit(vec3 v) {
    vec3 a = (v * (v + 0.0245786)) - 0.000090537;
    vec3 b = (v * ((0.983729 * v) + 0.4329510)) + 0.238081;
    return a / b;
}

vec3 tonemap_aces(vec3 color) {
    color = kAcesInput * color;
    color = rrt_and_odt_fit(color);
    color = kAcesOutput * color;
    // The output matrix can leave a channel a little outside the range, and it
    // can push one slightly negative. Neither is a colour a display can show.
    return clamp(color, 0.0, 1.0);
}

void main() {
    vec3 color = texture(scene_color, in_uv).rgb;

    // Exposure before the curve, because the curve is not a straight line.
    // Scaling after it would move an already compressed value and throw away
    // the highlight roll off that is the point of the curve.
    color = tonemap_aces(color * push.exposure);

    // The swapchain is an sRGB format, so the hardware converts this from
    // linear as it lands. Nothing here encodes sRGB by hand, per DESIGN.md
    // section 3.
    out_color = vec4(color, 1.0);
}
