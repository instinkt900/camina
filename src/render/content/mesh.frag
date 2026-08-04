#version 450

// Cook-Torrance metallic-roughness, over the glTF material set the cooker has
// written since M4.4b. See DESIGN.md section 9 "Materials".
//
// The environment term is still two constant colors. IBL is M5.4, and until it
// lands a metal has almost nothing to reflect, so a metal reads dark here. That
// is the expected result and not a bug in this shader.

layout(location = 0) in vec3 in_world;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in vec4 in_tangent;
layout(location = 3) in vec2 in_uv;

layout(location = 0) out vec4 out_color;

// Must match kMaxLights in mesh_pass.cpp.
const uint kMaxLights = 8u;

struct Light {
    // xyz is the direction it points for a directional light, or where it is for
    // a point light. w is 0 for directional and 1 for point.
    vec4 position;
    // rgb is the color times the intensity. a is the range in meters, which a
    // directional light leaves at zero.
    vec4 color;
};

layout(set = 0, binding = 0) uniform Frame {
    mat4 view_projection;
    vec4 camera_position; // w is unused and keeps the block aligned.
    uvec4 light_count;    // x is how many lights are real. The rest is padding.
    Light lights[kMaxLights];
} frame;

// Every one of these is always a valid sampler. A material that names no
// texture for a slot binds a single white texel, so the shader needs no branch
// to read one. The factor beside it still applies.
//
// A base color and an emissive image are sRGB formats, so the hardware converts
// on the read and those values arrive linear. The rest are linear formats
// already, because the cooker read the color space from the sidecar. See
// DESIGN.md section 3.
layout(set = 1, binding = 0) uniform sampler2D base_color_map;
layout(set = 1, binding = 1) uniform sampler2D metallic_roughness_map;
layout(set = 1, binding = 2) uniform sampler2D normal_map;
layout(set = 1, binding = 3) uniform sampler2D occlusion_map;
layout(set = 1, binding = 4) uniform sampler2D emissive_map;

// Which slots the material really named. A slot it did not name reads the white
// texel above, which is right for a base color and wrong for a normal map, so
// the ones that need a different default test their bit.
//
// This is a branch on a uniform, which every lane takes the same way and which
// costs almost nothing. The second half of M5.1 turns these into defines and
// compiles a variant for each set of them.
const uint kHasNormalMap = 1u << 2;
const uint kHasOcclusionMap = 1u << 3;

layout(set = 1, binding = 5) uniform Material {
    vec4 base_color_factor;
    vec4 emissive_factor; // w is unused and keeps the block aligned.
    float metallic_factor;
    float roughness_factor;
    float normal_scale;
    float occlusion_strength;
    float alpha_cutoff;
    uint alpha_mode;
    uint has_maps;
    uint padding;
} material;

// The environment, until M5.4 replaces it with a cooked one.
const vec3 kSkyColor = vec3(0.28, 0.34, 0.45);
const vec3 kGroundColor = vec3(0.16, 0.14, 0.12);

// A dielectric reflects about four percent head on, which is what glTF assumes
// for every material that does not say otherwise.
const vec3 kDielectricF0 = vec3(0.04);

const float kPi = 3.14159265359;

// The GGX normal distribution. How much of the surface faces the half vector.
float distribution_ggx(float n_dot_h, float roughness) {
    float a = roughness * roughness;
    float a2 = a * a;
    float d = ((n_dot_h * n_dot_h) * (a2 - 1.0)) + 1.0;
    return a2 / max(kPi * d * d, 1e-7);
}

// Smith geometry with the Schlick-GGX term, height correlated by the split
// form. How much of the surface shadows itself.
float geometry_smith(float n_dot_v, float n_dot_l, float roughness) {
    float r = roughness + 1.0;
    float k = (r * r) / 8.0;
    float gv = n_dot_v / ((n_dot_v * (1.0 - k)) + k);
    float gl = n_dot_l / ((n_dot_l * (1.0 - k)) + k);
    return gv * gl;
}

// Fresnel by Schlick. How reflective the surface is at this angle.
vec3 fresnel_schlick(float cos_theta, vec3 f0) {
    return f0 + ((1.0 - f0) * pow(clamp(1.0 - cos_theta, 0.0, 1.0), 5.0));
}

// Builds the shading normal from the vertex frame and the normal map.
vec3 shading_normal() {
    vec3 n = normalize(in_normal);
    if ((material.has_maps & kHasNormalMap) == 0u) {
        return n;
    }

    vec3 t = normalize(in_tangent.xyz - (n * dot(n, in_tangent.xyz)));
    // w is +1 or -1 and it says which way the bitangent points. Getting this
    // wrong mirrors the lighting on half the model.
    vec3 b = cross(n, t) * in_tangent.w;

    vec3 sampled = (texture(normal_map, in_uv).xyz * 2.0) - 1.0;
    sampled.xy *= material.normal_scale;
    return normalize(mat3(t, b, n) * sampled);
}

void main() {
    vec4 base = texture(base_color_map, in_uv) * material.base_color_factor;

    // Mask draws a texel or it does not, with nothing between. Blend needs the
    // pipeline to blend and needs the draws sorted, which is the second half of
    // M5.2.
    if (material.alpha_mode == 1u && base.a < material.alpha_cutoff) {
        discard;
    }

    // Metallic in blue and roughness in green, which is the packing glTF uses.
    vec2 mr = texture(metallic_roughness_map, in_uv).bg;
    float metallic = clamp(mr.x * material.metallic_factor, 0.0, 1.0);
    // Away from zero, because a perfectly smooth surface makes the distribution
    // term divide by nothing and the highlight becomes an infinite point.
    float roughness = clamp(mr.y * material.roughness_factor, 0.045, 1.0);

    vec3 n = shading_normal();
    vec3 v = normalize(frame.camera_position.xyz - in_world);
    float n_dot_v = max(dot(n, v), 1e-4);

    // A metal has no diffuse term and it tints its reflection with its own
    // color. A dielectric reflects white and keeps its color in the diffuse.
    vec3 albedo = base.rgb;
    vec3 f0 = mix(kDielectricF0, albedo, metallic);

    vec3 direct = vec3(0.0);
    for (uint i = 0u; i < min(frame.light_count.x, kMaxLights); ++i) {
        Light light = frame.lights[i];

        // l points from the surface towards the light, which is the opposite of
        // the way a directional light travels.
        vec3 l;
        float attenuation = 1.0;
        if (light.position.w < 0.5) {
            l = -light.position.xyz;
        } else {
            vec3 to_light = light.position.xyz - in_world;
            float distance = length(to_light);
            l = to_light / max(distance, 1e-4);

            // The inverse square, windowed so the light reaches zero at its
            // range rather than going on forever. Without the window every
            // light would touch every surface and nothing could cull one.
            float range = max(light.color.a, 1e-4);
            float window = clamp(1.0 - pow(distance / range, 4.0), 0.0, 1.0);
            attenuation = (window * window) / max(distance * distance, 1e-4);
        }

        float n_dot_l = max(dot(n, l), 0.0);
        if (n_dot_l <= 0.0 || attenuation <= 0.0) {
            continue;
        }

        vec3 h = normalize(v + l);
        float n_dot_h = max(dot(n, h), 0.0);

        vec3 f = fresnel_schlick(max(dot(h, v), 0.0), f0);
        float d = distribution_ggx(n_dot_h, roughness);
        float g = geometry_smith(n_dot_v, n_dot_l, roughness);

        vec3 specular = (d * g * f) / max(4.0 * n_dot_v * n_dot_l, 1e-7);
        vec3 diffuse = (vec3(1.0) - f) * (1.0 - metallic) * albedo / kPi;

        direct += (diffuse + specular) * light.color.rgb * n_dot_l * attenuation;
    }

    // The environment, which M5.4 replaces with a cooked one. A metal reflects
    // it and has no diffuse, so a metal is nearly black until then.
    float up = (dot(n, vec3(0.0, 1.0, 0.0)) * 0.5) + 0.5;
    vec3 environment = mix(kGroundColor, kSkyColor, up);
    vec3 ambient = environment * mix(albedo, albedo * f0, metallic);

    float occlusion = 1.0;
    if ((material.has_maps & kHasOcclusionMap) != 0u) {
        // Occlusion is in red, and the strength blends back towards no
        // occlusion rather than scaling it.
        float sampled = texture(occlusion_map, in_uv).r;
        occlusion = mix(1.0, sampled, material.occlusion_strength);
    }

    vec3 emissive = texture(emissive_map, in_uv).rgb * material.emissive_factor.rgb;

    // The working space is linear, and the swapchain converts at the final
    // write. Nothing clamps the top end yet, so a bright result clips. The ACES
    // tonemap is M5.6. See DESIGN.md section 3.
    vec3 color = ((ambient * occlusion) + direct) + emissive;
    out_color = vec4(color, base.a);
}
