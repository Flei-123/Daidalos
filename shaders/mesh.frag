#version 450
// Daidalos mesh fragment stage.
//
// Sun (Lambert + Blinn-Phong) + hemisphere ambient + shadow map + distance
// fog, then exposure, Reinhard tonemap and gamma. One material model on
// purpose: a general purpose look, not a film renderer.

layout(location = 0) in vec3 vNormal;
layout(location = 1) in vec3 vColor;
layout(location = 2) in vec3 vWorld;
layout(location = 3) in vec2 vMat;            // roughness, emissive
layout(location = 4) flat in uint vFlags;

layout(set = 0, binding = 0) uniform Frame {
    mat4 viewproj;
    mat4 invviewproj;
    mat4 lightviewproj;
    vec4 sun_dir;
    vec4 sun_color;
    vec4 sky_color;
    vec4 ground_color;
    vec4 fog_color;
    vec4 cam_pos;
} F;
layout(set = 0, binding = 1) uniform sampler2DShadow uShadow;

layout(location = 0) out vec4 outColor;

const uint FLAG_CHECKER = 2u;

// Narkowicz's ACES fit. Reinhard washes everything into the same grey mush -
// which is exactly what "the pictures look odd" looked like: no black, no
// white, no contrast anywhere in the frame.
vec3 tonemap_aces(vec3 x) {
    const float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}

float shadow_factor(vec3 world, float ndl) {
    if (F.cam_pos.w < 0.5) return 1.0;
    vec4 lp = F.lightviewproj * vec4(world, 1.0);
    vec3 p  = lp.xyz / lp.w;
    p.xy = p.xy * 0.5 + 0.5;
    if (p.x < 0.002 || p.x > 0.998 || p.y < 0.002 || p.y > 0.998 || p.z > 1.0 || p.z < 0.0) return 1.0;

    float bias = mix(0.0040, 0.0008, ndl);      // slope scaled
    float texel = F.sun_color.w;
    float s = 0.0;
    for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x)
            s += texture(uShadow, vec3(p.xy + vec2(x, y) * texel, p.z - bias));
    return s / 9.0;
}

void main() {
    vec3  n = normalize(vNormal);
    vec3  l = normalize(F.sun_dir.xyz);
    vec3  v = normalize(F.cam_pos.xyz - vWorld);
    float rough = clamp(vMat.x, 0.03, 1.0);

    vec3 albedo = vColor;
    if ((vFlags & FLAG_CHECKER) != 0u) {
        // distance aware checkerboard: without the fwidth fade this turns into
        // a moire mess a few tens of metres out and the whole frame looks noisy
        vec2  uv = vWorld.xz * 0.25;
        float f  = max(fwidth(uv.x), fwidth(uv.y));
        vec2  c  = floor(uv);
        float k  = mod(c.x + c.y, 2.0);
        albedo *= mix(mix(0.68, 1.0, k), 0.84, clamp(f * 2.2, 0.0, 1.0));
    }

    float ndl = max(dot(n, l), 0.0);
    float sh  = shadow_factor(vWorld, ndl);

    // DAI_DEBUG_SHADOW=1 paints the raw shadow term instead of the surface -
    // white = lit, black = occluded, and a uniformly white frame means the
    // shadow map never got written or the light matrix misses the scene.
    if (F.cam_pos.w > 1.5) { outColor = vec4(vec3(sh), 1.0); return; }

    float up = n.y * 0.5 + 0.5;
    vec3 ambient = mix(F.ground_color.rgb, F.sky_color.rgb, up) * F.sky_color.w;
    vec3 diffuse = F.sun_color.rgb * F.sun_dir.w * ndl * sh;

    vec3  h = normalize(l + v);
    float shininess = mix(180.0, 4.0, rough);
    float spec = pow(max(dot(n, h), 0.0), shininess) * (1.0 - rough) * 0.6 * sh * step(0.001, ndl);

    vec3 color = albedo * (ambient + diffuse) + F.sun_color.rgb * spec;
    color = mix(color, albedo, clamp(vMat.y, 0.0, 1.0));      // emissive

    // squared distance fog: stays out of the way up close, closes the horizon
    float density = F.ground_color.w;
    if (density > 0.0) {
        float d = length(F.cam_pos.xyz - vWorld) * density;
        color = mix(color, F.fog_color.rgb, clamp(1.0 - exp(-d * d), 0.0, 1.0));
    }

    color *= F.fog_color.w;             // exposure
    color = tonemap_aces(color);
    outColor = vec4(pow(color, vec3(1.0 / 2.2)), 1.0);
}
