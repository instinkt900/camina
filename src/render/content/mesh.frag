#version 450

// Cook-Torrance metallic-roughness, over the glTF material set the cooker has
// written since M4.4b. See DESIGN.md section 9 "Materials".
//
// The environment lights a surface by the split sum approximation, over three
// things the cooker writes. The irradiance is nine coefficients in the frame
// block, the specular is the environment prefiltered by roughness across the
// mips of one cubemap, and the lookup table below is the integral of this BRDF
// over every environment there is. So the shader reads three textures and
// evaluates one polynomial where the integral belongs.

layout(location = 0) in vec3 in_world;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in vec4 in_tangent;
layout(location = 3) in vec2 in_uv;

layout(location = 0) out vec4 out_color;

struct Light {
    // xyz is the direction it points for a directional light, or where it is for
    // a point light. w is 0 for directional and 1 for point.
    vec4 position;
    // rgb is the color times the intensity. a is the range in meters, which a
    // directional light leaves at zero.
    vec4 color;
};

// Must match kIrradianceCoefficients in src/assets/irradiance.h.
const uint kIrradianceCoefficients = 9u;

// Must match kCascadeCount in render/shadow_pass.h. This is the size of the
// array, not the number in use. light_count.z carries that.
const uint kMaxCascades = 4u;

layout(set = 0, binding = 0) uniform Frame {
    mat4 view_projection;
    // One for each cascade: takes a world position into that cascade's clip
    // space. Identity when the scene has no directional light, and
    // shadow_factor() reads light_count.y rather than testing the matrix.
    mat4 light_view_projection[kMaxCascades];
    vec4 camera_position; // w is unused and keeps the block aligned.
    // Where each cascade ends, as a distance in front of the camera. Increasing,
    // and the last one is the whole shadow distance.
    vec4 cascade_splits;
    // The depth bias each cascade needs in its own clip space. A cascade that
    // covers more world has larger texels and a longer depth range, so one
    // number cannot serve them all.
    vec4 cascade_biases;
    // x is how many lights are real. y is 1 when a directional light casts a
    // shadow. z is how many cascades are in use. w is padding.
    uvec4 light_count;
    // x is the near plane the projection was built with and y is how far the
    // cluster slices reach. zw is the viewport in pixels. C++ owns all four,
    // and cluster_cull.comp reads the same first pair.
    vec4 cluster_view;
    // The irradiance of the environment, as a second order spherical harmonic.
    // rgb is one coefficient and w is padding, because std140 puts an array
    // element on a sixteen byte boundary either way.
    vec4 irradiance[kIrradianceCoefficients];
} frame;

// Every light the camera can see, which `render::MeshPass` culls and writes.
//
// A storage buffer rather than an array in the block above. A uniform block
// declares how long an array inside it is, and that declared length was the
// eight-light ceiling. This one has no length in the declaration, so the count
// is whatever `light_count.x` says. See issue #98 and DESIGN.md section 9.
//
// It is `readonly` because nothing here writes it. That lets the driver treat it
// as a read and skips the write hazards it would otherwise have to assume.
layout(set = 0, binding = 4) readonly buffer Lights {
    Light lights[];
} light_buffer;

// The cluster grid. The compute cull divides the frustum into a tile grid over
// depth slices, tests every point light against each cell, and writes a short
// list of light indices for the fragment shader to loop over. A directional
// light goes in every cell because it has no position to test.
//
// The grid shape. Must match kClusterTileCountX and friends in mesh_pass.h,
// and cluster_cull.comp declares the same four. The near plane and the reach
// are not here, because they arrive in frame.cluster_view.
const uint kClusterTileCountX = 16;
const uint kClusterTileCountY = 12;
const uint kClusterSliceCount = 16;
const uint kMaxLightsPerCell = 64;
const uint kClusterCellCount = kClusterTileCountX * kClusterTileCountY * kClusterSliceCount;

layout(set = 0, binding = 5) readonly buffer ClusterGrid {
    uint cells[]; // flat: counts followed by light indices per cell
} cluster_grid;

// The environment every surface reflects, which `scene::Environment` names and
// `render::MeshPass` binds. A scene that names none binds six grey texels, so
// this is always a valid sampler and the shader needs no branch.
//
// Each mip level is the environment prefiltered for one roughness, so level 0
// is a mirror and the last level is the roughest surface there is. The chain
// is not a box filtered one, and reading it as if it were reads too sharp.
//
// It is a linear float format. There is no sRGB conversion on this read,
// because the cooker wrote radiance and not a color. See DESIGN.md section 3.
layout(set = 0, binding = 1) uniform samplerCube environment_map;

// The split sum lookup, which `tools/cooker/brdf.cpp` integrates once from
// `ibl.brdf` and every environment shares. Red is the scale on the reflectance
// at normal incidence and green is the bias. The horizontal axis is the cosine
// of the angle to the viewer and the vertical axis is roughness.
layout(set = 0, binding = 2) uniform sampler2D brdf_lut;

// What the directional light can see, from render::ShadowPass. A sampler2DShadow
// compares rather than returns the texel, so a linear filter gives four taps of
// percentage closer filtering for the cost of one read.
//
// The comparison is "greater or equal", which is what reverse-Z needs. Outside
// the map the sampler reads a border of zero, the far plane, so every fragment
// there is lit. See gfx::AddressMode::ClampToZeroBorder.
layout(set = 0, binding = 3) uniform sampler2DArrayShadow shadow_map;

/**
 * How much of the key light reaches this fragment. 1 is fully lit.
 *
 * The bias is in shadow map texels rather than world units, because the error
 * this corrects is a texel covering a slope. A surface nearly edge-on to the
 * light spans far more depth across one texel than a surface facing it, and the
 * normal-facing term is what tracks that.
 *
 * The shadow pass draws both faces, because a wall of the room is a single quad
 * with no back and culling either side would let the light through it. So this
 * bias carries the whole correction on its own.
 */
float shadow_factor(vec3 world, vec3 n, vec3 light_direction) {
    if (frame.light_count.y == 0u) {
        return 1.0;
    }

    // gl_FragCoord.w is one over the clip w that the projection produced, and
    // for this projection clip w is the distance in front of the camera. So this
    // is the view depth with nothing added to the frame block to carry it.
    float view_depth = 1.0 / gl_FragCoord.w;

    uint count = min(frame.light_count.z, kMaxCascades);
    uint cascade = count - 1u;
    for (uint i = 0u; i < count; ++i) {
        if (view_depth < frame.cascade_splits[i]) {
            cascade = i;
            break;
        }
    }

    vec4 light_clip = frame.light_view_projection[cascade] * vec4(world, 1.0);
    // An orthographic projection leaves w at 1, so this divide is a formality.
    // It stays because #135 may fit a cascade with a perspective volume.
    vec3 coord = light_clip.xyz / light_clip.w;

    // Clip space is -1 to 1 across, and a texture reads 0 to 1. Depth is
    // already 0 to 1 under Vulkan and must not be rescaled.
    coord.xy = (coord.xy * 0.5) + 0.5;

    // Behind the light's far plane. Nothing recorded a depth there, so treat it
    // as lit rather than reading a coordinate outside the map.
    if (coord.z <= 0.0) {
        return 1.0;
    }

    float n_dot_l = clamp(dot(n, light_direction), 0.0, 1.0);
    // Grows as the surface turns away from the light, up to eight times the
    // straight-on value. Beyond that a grazing surface is barely lit anyway, and
    // an unbounded slope term detaches the shadow where it is needed most.
    float slope = clamp(tan(acos(n_dot_l)), 0.0, 8.0);
    // The base comes from this cascade's own texel size, worked out on the CPU.
    // Reverse-Z means a lit surface needs a larger value, so the bias adds.
    float bias = frame.cascade_biases[cascade] * (1.0 + slope);

    return texture(shadow_map, vec4(coord.xy, float(cascade), coord.z + bias));
}

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

// A slot the material did not name reads the white texel above, which is right
// for a base color and wrong for a normal map. The two that need a different
// default are compiled in or out instead of tested at run time.
//
// HAS_NORMAL_MAP and HAS_OCCLUSION_MAP come from the variant list in
// `mesh.frag.meta`, and `render::MeshPass` picks the form that matches what a
// material named. `material.has_maps` still carries every bit, because the
// block is one shape for all four forms.
//
// The five samplers above stay declared in every form on purpose. A declaration
// inside an `#ifdef` would give each form a different descriptor set, and then
// one material set could not bind against another form. MeshPass checks that
// the forms agree rather than trusting this comment.

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

// Schlick again, with a ceiling that roughness lowers. A rough surface reflects
// less at a grazing angle than a smooth one, and the plain form above has no
// way to say so: it climbs to white at the edge whatever the roughness.
//
// This decides only how much light the ambient specular takes away from the
// ambient diffuse. The specular itself uses the lookup table, which carries the
// same effect properly.
vec3 fresnel_schlick_roughness(float cos_theta, vec3 f0, float roughness) {
    vec3 ceiling = max(vec3(1.0 - roughness), f0);
    return f0 + ((ceiling - f0) * pow(clamp(1.0 - cos_theta, 0.0, 1.0), 5.0));
}

// Rebuilds irradiance from the nine cooked coefficients.
//
// Every basis constant and the per band cosine convolution are folded in at
// cook time, so this sum is the whole of it and there is no table here to drift
// from the one the cooker used. `src/assets/irradiance.h` carries the same sum
// and the two must agree term for term.
vec3 environment_irradiance(vec3 n) {
    return frame.irradiance[0].rgb
        + (frame.irradiance[1].rgb * n.y)
        + (frame.irradiance[2].rgb * n.z)
        + (frame.irradiance[3].rgb * n.x)
        + (frame.irradiance[4].rgb * (n.x * n.y))
        + (frame.irradiance[5].rgb * (n.y * n.z))
        + (frame.irradiance[6].rgb * ((3.0 * n.z * n.z) - 1.0))
        + (frame.irradiance[7].rgb * (n.x * n.z))
        + (frame.irradiance[8].rgb * ((n.x * n.x) - (n.y * n.y)));
}

// Builds the shading normal from the vertex frame and the normal map.
vec3 shading_normal() {
    vec3 n = normalize(in_normal);
#ifndef HAS_NORMAL_MAP
    return n;
#else
    vec3 t = normalize(in_tangent.xyz - (n * dot(n, in_tangent.xyz)));
    // w is +1 or -1 and it says which way the bitangent points. Getting this
    // wrong mirrors the lighting on half the model.
    vec3 b = cross(n, t) * in_tangent.w;

    vec3 sampled = (texture(normal_map, in_uv).xyz * 2.0) - 1.0;
    sampled.xy *= material.normal_scale;
    return normalize(mat3(t, b, n) * sampled);
#endif
}

void main() {
    vec4 base = texture(base_color_map, in_uv) * material.base_color_factor;

    // Mask draws a texel or it does not, with nothing between. Blend needs no
    // test here, because the blend pipeline does the work and MeshPass sorts
    // the draws back to front before it sends them.
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
    // Set once the loop has seen a directional light, so the second one does not
    // read a map that was rendered for the first.
    bool shadowed_one = false;

    // The tile comes from the window position the rasterizer already worked
    // out. Multiplying the interpolated world position by the camera again
    // would answer the same question at the cost of a matrix, and it would
    // disagree by a tile at a boundary where the two rounded apart.
    vec2 tile_uv = gl_FragCoord.xy / frame.cluster_view.zw;
    uint tile_x = min(uint(tile_uv.x * float(kClusterTileCountX)), kClusterTileCountX - 1u);
    uint tile_y = min(uint(tile_uv.y * float(kClusterTileCountY)), kClusterTileCountY - 1u);

    // View distance, which the projection puts in 1.0 / gl_FragCoord.w. The
    // cull slices exponentially in this measure, so both sides agree on the
    // cell. A fragment past the reach clamps into the last slice, which the
    // cull extends to meet it.
    float z_near = frame.cluster_view.x;
    float cluster_far = frame.cluster_view.y;
    float view_depth = 1.0 / gl_FragCoord.w;
    float slice_f = log(view_depth / z_near) / log(cluster_far / z_near) * float(kClusterSliceCount);
    uint slice = uint(clamp(slice_f, 0.0, float(kClusterSliceCount) - 1.0));

    uint cell = (slice * kClusterTileCountY + tile_y) * kClusterTileCountX + tile_x;
    uint per_cell_count = cluster_grid.cells[cell];
    uint cell_index_base = kClusterCellCount + cell * kMaxLightsPerCell;

    // Loop over the lights the cull assigned to this cell. The cull dispatches
    // on every frame and writes a count for every cell, including a zero, so
    // this never reads what the frame before it wrote.
    uint light_count = min(per_cell_count, kMaxLightsPerCell);
    for (uint j = 0u; j < light_count; ++j) {
        uint i = cluster_grid.cells[cell_index_base + j];
        if (i >= frame.light_count.x) {
            continue;
        }
        Light light = light_buffer.lights[i];

        // l points from the surface towards the light, which is the opposite of
        // the way a directional light travels.
        vec3 l;
        float attenuation = 1.0;
        // Only the first directional light casts. One map exists, and #135 is
        // what gives a second caster somewhere to write.
        bool casts = false;
        if (light.position.w < 0.5) {
            l = -light.position.xyz;
            casts = !shadowed_one;
            shadowed_one = true;
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

        float visibility = casts ? shadow_factor(in_world, n, l) : 1.0;
        direct += (diffuse + specular) * light.color.rgb * n_dot_l * attenuation * visibility;
    }

    // The environment, by the split sum approximation.
    //
    // How much of the ambient light the specular takes. What is left goes to
    // the diffuse, so a smooth surface seen edge on keeps almost none of it.
    vec3 f_ambient = fresnel_schlick_roughness(n_dot_v, f0, roughness);

    // The coefficients carry the cosine convolution and nothing else, so the
    // Lambert divide belongs here. A metal has no diffuse term at all.
    vec3 diffuse_ambient = max(environment_irradiance(n), vec3(0.0)) * albedo / kPi;
    diffuse_ambient *= (vec3(1.0) - f_ambient) * (1.0 - metallic);

    // One mip level for each roughness the cooker filtered for. The first level
    // is the sharp environment and the last is the roughest, so roughness picks
    // the level directly.
    float levels = float(textureQueryLevels(environment_map)) - 1.0;
    vec3 prefiltered = textureLod(environment_map, reflect(-v, n), roughness * levels).rgb;

    // The other half of the split sum. It says what fraction of the reflection
    // survives this surface at this angle, as a scale on f0 and a bias beside
    // it, and it depends on no environment at all.
    vec2 scale_bias = texture(brdf_lut, vec2(n_dot_v, roughness)).rg;
    vec3 specular_ambient = prefiltered * ((f0 * scale_bias.x) + scale_bias.y);

    vec3 ambient = diffuse_ambient + specular_ambient;

    float occlusion = 1.0;
#ifdef HAS_OCCLUSION_MAP
    // Occlusion is in red, and the strength blends back towards no occlusion
    // rather than scaling it.
    occlusion = mix(1.0, texture(occlusion_map, in_uv).r, material.occlusion_strength);
#endif

    vec3 emissive = texture(emissive_map, in_uv).rgb * material.emissive_factor.rgb;

    // The working space is linear, and the swapchain converts at the final
    // write. Nothing clamps the top end yet, so a bright result clips. The ACES
    // tonemap is M5.6. See DESIGN.md section 3.
    vec3 color = ((ambient * occlusion) + direct) + emissive;
    out_color = vec4(color, base.a);
}
