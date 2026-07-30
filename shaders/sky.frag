#version 450
// Procedural sky. The view ray is reconstructed from the inverse view
// projection, so looking up actually shows the zenith - a screen space
// gradient would not.

layout(location = 0) in vec2 vNdc;

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

layout(location = 0) out vec4 outColor;

void main() {
    vec4 far  = F.invviewproj * vec4(vNdc, 1.0, 1.0);
    vec4 near = F.invviewproj * vec4(vNdc, 0.0, 1.0);
    vec3 dir  = normalize(far.xyz / far.w - near.xyz / near.w);

    float up = dir.y;
    vec3 zenith  = F.sky_color.rgb;
    vec3 horizon = F.fog_color.rgb;
    vec3 below   = F.ground_color.rgb;

    vec3 c;
    if (up >= 0.0) c = mix(horizon, zenith, pow(clamp(up, 0.0, 1.0), 0.55));
    else           c = mix(horizon, below,  pow(clamp(-up, 0.0, 1.0), 0.35));

    // sun disc plus a soft glow, only if the sun is above the horizon
    float sd = max(dot(dir, normalize(F.sun_dir.xyz)), 0.0);
    c += F.sun_color.rgb * (pow(sd, 900.0) * 6.0 + pow(sd, 24.0) * 0.25) * F.sun_dir.w;

    c *= F.fog_color.w;
    const float A = 2.51, B = 0.03, C = 2.43, D = 0.59, E = 0.14;
    c = clamp((c * (A * c + B)) / (c * (C * c + D) + E), 0.0, 1.0);
    outColor = vec4(pow(c, vec3(1.0 / 2.2)), 1.0);
}
