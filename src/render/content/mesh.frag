#version 450

layout(location = 0) in vec3 in_normal;
layout(location = 1) in vec2 in_uv;

layout(location = 0) out vec4 out_color;

// The base color of the material this submesh names. A submesh with no material,
// and one whose texture will not load, both bind a single white texel, so this
// sampler is always valid.
//
// The image is an sRGB format, so the hardware converts on the read and this
// value is already linear. See DESIGN.md section 3.
layout(set = 0, binding = 0) uniform sampler2D base_color;

// Two lights, not a material model. The base color is real now, and the shading
// over it is still a placeholder until M5 reads the metallic, the roughness, and
// the normal map that the cooked material already names.
//
// They are here so the shape reads. A flat texture lookup would show the color
// and nothing about the surface, which would hide exactly the mistakes a first
// look at a new importer has to catch: an inverted normal, a mirrored winding,
// or a mesh inside out.
const vec3 kKeyDirection = normalize(vec3(0.4, 0.8, 0.5));
const vec3 kKeyColor = vec3(1.0, 0.97, 0.92);
const vec3 kSkyColor = vec3(0.28, 0.34, 0.45);
const vec3 kGroundColor = vec3(0.16, 0.14, 0.12);

void main() {
    vec3 normal = normalize(in_normal);

    // A hemisphere term, so a surface facing away from the key light is lit by
    // the sky rather than being black.
    float up = (dot(normal, vec3(0.0, 1.0, 0.0)) * 0.5) + 0.5;
    vec3 ambient = mix(kGroundColor, kSkyColor, up);

    float key = max(dot(normal, kKeyDirection), 0.0);

    vec3 surface = texture(base_color, in_uv).rgb;

    // The working space is linear, and the swapchain converts at the final
    // write. See DESIGN.md section 3.
    out_color = vec4(surface * (ambient + (kKeyColor * key)), 1.0);
}
