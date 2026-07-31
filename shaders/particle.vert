#version 450
// Camera facing billboards, six vertices per instance, no geometry buffer.
// The quad corners come from gl_VertexIndex and the camera basis from the
// frame uniforms, so a particle costs 40 bytes and no CPU transform work.

layout(location = 0) in vec3  iPos;
layout(location = 1) in float iSize;
layout(location = 2) in vec3  iColor;
layout(location = 3) in float iAlpha;
layout(location = 4) in float iRot;
layout(location = 5) in uint  iBlend;

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

layout(location = 0) out vec2 vUV;
layout(location = 1) out vec3 vColor;
layout(location = 2) out float vAlpha;
layout(location = 3) flat out uint vBlend;

void main() {
    vec2 c[6] = vec2[6](vec2(-1,-1), vec2(1,-1), vec2(1,1),
                        vec2(-1,-1), vec2(1,1), vec2(-1,1));
    vec2 q = c[gl_VertexIndex % 6];
    float s = sin(iRot), co = cos(iRot);
    vec2 r = vec2(q.x * co - q.y * s, q.x * s + q.y * co);

    vec3 world = iPos + (F.cam_right.xyz * r.x + F.cam_up.xyz * r.y) * (iSize * 0.5);
    gl_Position = F.viewproj * vec4(world, 1.0);

    vUV = q * 0.5 + 0.5;
    vColor = iColor;
    vAlpha = iAlpha;
    vBlend = iBlend;
}
