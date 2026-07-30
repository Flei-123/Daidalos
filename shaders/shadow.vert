#version 450
// Depth only pass from the sun's point of view. Same vertex layout as
// mesh.vert so both pipelines share one vertex and instance buffer.

layout(location = 0) in vec3  inPos;
layout(location = 1) in vec3  inNrm;
layout(location = 2) in float inCap;
layout(location = 3) in vec2  inUV;

layout(location = 4) in vec3  iPos;
layout(location = 5) in vec4  iRot;
layout(location = 6) in vec3  iScale;
layout(location = 7) in vec3  iColor;
layout(location = 8) in vec3  iMat;
layout(location = 9) in uint  iFlags;

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
} F;

layout(push_constant) uniform Mat {
    vec4 base_color;
    vec4 emissive;
    vec4 scalars;
    vec4 extra;      // w = which cascade this pass is filling
} M;

vec3 rotq(vec4 q, vec3 v) {
    return v + 2.0 * cross(q.xyz, cross(q.xyz, v) + q.w * v);
}

void main() {
    vec3 local = inPos * iScale + vec3(0.0, inCap * iMat.x, 0.0);
    vec3 world = rotq(iRot, local) + iPos;
    gl_Position = F.lightviewproj[int(M.extra.w + 0.5)] * vec4(world, 1.0);
}
