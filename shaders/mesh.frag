#version 450
// Daidalos surface shader: glTF 2.0 metallic-roughness, one model for
// everything. Four maps (base colour, ORM, normal, emissive), Cook-Torrance
// GGX specular, hemisphere ambient, shadow map, fog, ACES.
//
// No node graph on purpose. A material is DATA - which is what makes the
// Blender -> engine round trip boring instead of a baking session.

layout(location = 0) in vec3 vNormal;
layout(location = 1) in vec3 vColor;
layout(location = 2) in vec3 vWorld;
layout(location = 3) in vec2 vMat;            // instance roughness, instance emissive
layout(location = 4) flat in uint vFlags;
layout(location = 5) in vec2 vUV;

layout(set = 0, binding = 0) uniform Frame {
    mat4 viewproj;
    mat4 invviewproj;
    mat4 lightviewproj[3];
    vec4 cascade_split;
    vec4 sun_dir;
    vec4 sun_color;
    vec4 sky_color;
    vec4 ground_color;
    vec4 fog_color;
    vec4 cam_pos;
    vec4 cam_right;
    vec4 cam_up;
} F;
layout(set = 0, binding = 1) uniform sampler2DArrayShadow uShadow;

layout(set = 1, binding = 0) uniform sampler2D uBaseColor;
layout(set = 1, binding = 1) uniform sampler2D uORM;        // occlusion / roughness / metallic
layout(set = 1, binding = 2) uniform sampler2D uNormal;
layout(set = 1, binding = 3) uniform sampler2D uEmissive;

layout(push_constant) uniform Mat {
    vec4 base_color;   // rgb, w = alpha cutoff
    vec4 emissive;     // rgb, w = flags
    vec4 scalars;      // metallic, roughness, normal strength, uv scale
    vec4 extra;        // occlusion strength, has_maps, has_normal_map, unused
} M;

layout(location = 0) out vec4 outColor;

const uint FLAG_CHECKER = 2u;
const float PI = 3.14159265;

vec3 tonemap_aces(vec3 x) {
    const float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}

// Cascade selection by view depth, then a 3x3 PCF tap. The cascade index is
// chosen from the distance to the camera rather than from the depth buffer, so
// it stays stable under a moving camera and costs one compare.
float shadow_factor(vec3 world, float ndl) {
    if (F.cam_pos.w < 0.5) return 1.0;
    float view_depth = length(F.cam_pos.xyz - world);
    int c = 0;
    if (view_depth > F.cascade_split.x) c = 1;
    if (view_depth > F.cascade_split.y) c = 2;

    vec4 lp = F.lightviewproj[c] * vec4(world, 1.0);
    vec3 p  = lp.xyz / lp.w;
    p.xy = p.xy * 0.5 + 0.5;
    if (p.x < 0.002 || p.x > 0.998 || p.y < 0.002 || p.y > 0.998 || p.z > 1.0 || p.z < 0.0) {
        // outside this cascade: try the widest one before giving up
        lp = F.lightviewproj[2] * vec4(world, 1.0);
        p = lp.xyz / lp.w;
        p.xy = p.xy * 0.5 + 0.5;
        c = 2;
        if (p.x < 0.002 || p.x > 0.998 || p.y < 0.002 || p.y > 0.998 || p.z > 1.0 || p.z < 0.0) return 1.0;
    }

    // bias grows with cascade size, otherwise the far cascade acnes
    float bias = mix(0.0035, 0.0007, ndl) * (1.0 + float(c) * 1.6);
    float texel = F.sun_color.w;
    float s = 0.0;
    for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x)
            s += texture(uShadow, vec4(p.xy + vec2(x, y) * texel, float(c), p.z - bias));
    return s / 9.0;
}

// Tangent frame from screen space derivatives. Saves a vertex attribute and,
// more importantly, saves every asset from needing baked tangents that must
// match the engine's convention exactly. (Schueler, ShaderX5.)
vec3 apply_normal_map(vec3 N, vec3 world, vec2 uv, float strength) {
    vec3 dp1 = dFdx(world), dp2 = dFdy(world);
    vec2 duv1 = dFdx(uv),   duv2 = dFdy(uv);
    vec3 dp2perp = cross(dp2, N), dp1perp = cross(N, dp1);
    vec3 T = dp2perp * duv1.x + dp1perp * duv2.x;
    vec3 B = dp2perp * duv1.y + dp1perp * duv2.y;
    float inv = inversesqrt(max(dot(T, T), dot(B, B)) + 1e-12);
    vec3 n = texture(uNormal, uv).xyz * 2.0 - 1.0;
    n.xy *= strength;
    return normalize(mat3(T * inv, B * inv, N) * n);
}

float D_GGX(float ndh, float a) {
    float a2 = a * a;
    float d = ndh * ndh * (a2 - 1.0) + 1.0;
    return a2 / max(PI * d * d, 1e-7);
}
float V_Smith(float ndv, float ndl, float a) {
    float k = a * 0.5;
    return 0.5 / max(mix(ndv, 1.0, k) * mix(ndl, 1.0, k) * 2.0, 1e-6);
}
vec3 F_Schlick(vec3 f0, float u) { return f0 + (1.0 - f0) * pow(1.0 - u, 5.0); }

void main() {
    vec2 uv = vUV;

    vec4 base = M.base_color;
    base.rgb *= vColor;
    if (M.extra.y > 0.5) base *= texture(uBaseColor, uv);
    if (M.base_color.w > 0.0 && base.a < M.base_color.w) discard;

    if ((vFlags & FLAG_CHECKER) != 0u) {
        vec2  c = vWorld.xz * 0.25;
        float f = max(fwidth(c.x), fwidth(c.y));
        float k = mod(floor(c.x) + floor(c.y), 2.0);
        base.rgb *= mix(mix(0.68, 1.0, k), 0.84, clamp(f * 2.2, 0.0, 1.0));
    }

    float metallic  = M.scalars.x;
    float roughness = M.scalars.y * clamp(vMat.x, 0.02, 4.0);
    float occlusion = 1.0;
    if (M.extra.y > 0.5) {
        vec3 orm = texture(uORM, uv).rgb;
        occlusion = mix(1.0, orm.r, M.extra.x);
        roughness *= orm.g;
        metallic  *= orm.b;
    }
    roughness = clamp(roughness, 0.045, 1.0);
    metallic  = clamp(metallic, 0.0, 1.0);

    vec3 N = normalize(vNormal);
    if (M.extra.z > 0.5) N = apply_normal_map(N, vWorld, uv, M.scalars.z);
    vec3 V = normalize(F.cam_pos.xyz - vWorld);
    vec3 L = normalize(F.sun_dir.xyz);
    vec3 H = normalize(L + V);

    float ndl = max(dot(N, L), 0.0);
    float ndv = max(dot(N, V), 1e-4);
    float ndh = max(dot(N, H), 0.0);
    float vdh = max(dot(V, H), 0.0);

    vec3 albedo = base.rgb;
    vec3 f0 = mix(vec3(0.04), albedo, metallic);
    vec3 diffuse_color = albedo * (1.0 - metallic);

    float sh = shadow_factor(vWorld, ndl);
    float a = roughness * roughness;
    vec3  spec = F_Schlick(f0, vdh) * D_GGX(ndh, a) * V_Smith(ndv, ndl, a);
    vec3  sun  = F.sun_color.rgb * F.sun_dir.w * ndl * sh;
    vec3  color = (diffuse_color / PI + spec) * sun * PI;

    // hemisphere ambient stands in for an environment probe: sky from above,
    // bounce from below, with a Fresnel weighted specular tint on top
    float up = N.y * 0.5 + 0.5;
    vec3 ambient_col = mix(F.ground_color.rgb, F.sky_color.rgb, up) * F.sky_color.w * occlusion;
    vec3 amb_spec = mix(F.ground_color.rgb, F.sky_color.rgb, clamp(reflect(-V, N).y * 0.5 + 0.5, 0.0, 1.0));
    color += diffuse_color * ambient_col;
    color += amb_spec * F.sky_color.w * F_Schlick(f0, ndv) * (1.0 - roughness) * occlusion;

    vec3 emissive = M.emissive.rgb;
    if (M.extra.y > 0.5) emissive *= texture(uEmissive, uv).rgb;
    color += emissive;
    color = mix(color, albedo, clamp(vMat.y, 0.0, 1.0));      // per instance emissive tint

    float density = F.ground_color.w;
    if (density > 0.0) {
        float d = length(F.cam_pos.xyz - vWorld) * density;
        color = mix(color, F.fog_color.rgb, clamp(1.0 - exp(-d * d), 0.0, 1.0));
    }

    // DAI_DEBUG_SHADOW=1 paints the raw shadow term: white = lit, black =
    // occluded. A uniformly white frame means the shadow map never got written.
    if (F.cam_pos.w > 1.5) { outColor = vec4(vec3(sh), 1.0); return; }

    color *= F.fog_color.w;
    outColor = vec4(pow(tonemap_aces(color), vec3(1.0 / 2.2)), 1.0);
}
