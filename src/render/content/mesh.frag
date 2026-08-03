#version 450

layout(location = 0) in vec3 in_normal;
layout(location = 1) in vec2 in_uv;

layout(location = 0) out vec4 out_color;

// A placeholder shade, not a material. M4.4b gives a submesh a material with a
// base color texture, and this file is what it replaces.
//
// The two lights are here so the shape reads. One flat colour would show a
// silhouette and nothing about the surface, which would hide exactly the
// mistakes a first look at a new importer has to catch: an inverted normal, a
// mirrored winding, or a mesh inside out.
const vec3 kKeyDirection = normalize(vec3(0.4, 0.8, 0.5));
const vec3 kKeyColor = vec3(1.0, 0.97, 0.92);
const vec3 kSkyColor = vec3(0.28, 0.34, 0.45);
const vec3 kGroundColor = vec3(0.16, 0.14, 0.12);
const vec3 kSurface = vec3(0.72, 0.72, 0.74);

void main() {
    vec3 normal = normalize(in_normal);

    // A hemisphere term, so a surface facing away from the key light is lit by
    // the sky rather than being black.
    float up = (dot(normal, vec3(0.0, 1.0, 0.0)) * 0.5) + 0.5;
    vec3 ambient = mix(kGroundColor, kSkyColor, up);

    float key = max(dot(normal, kKeyDirection), 0.0);

    // The working space is linear, and the swapchain converts at the final
    // write. See DESIGN.md section 3.
    out_color = vec4(kSurface * (ambient + (kKeyColor * key)), 1.0);
}
