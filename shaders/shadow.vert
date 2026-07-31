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
layout(location = 10) in uvec4 inJoints;
layout(location = 11) in vec4  inWeights;
layout(location = 12) in uvec2 iSkin;      // x = first joint matrix, y = joint count

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

layout(push_constant) uniform Mat {
    vec4 base_color;
    vec4 emissive;
    vec4 scalars;
    vec4 extra;      // w = which cascade this pass is filling
} M;

layout(set = 0, binding = 2) readonly buffer Joints { mat4 joint[]; } J;

// Linear blend skinning. Four influences, weights normalised by the exporter.
// A vertex with no weights leaves the matrix at identity, so skinned and rigid
// meshes share one pipeline.
mat4 skin_matrix() {
    if (iSkin.y == 0u) return mat4(1.0);
    float w = inWeights.x + inWeights.y + inWeights.z + inWeights.w;
    if (w <= 0.0001) return mat4(1.0);
    mat4 m = J.joint[iSkin.x + min(inJoints.x, iSkin.y - 1u)] * inWeights.x
           + J.joint[iSkin.x + min(inJoints.y, iSkin.y - 1u)] * inWeights.y
           + J.joint[iSkin.x + min(inJoints.z, iSkin.y - 1u)] * inWeights.z
           + J.joint[iSkin.x + min(inJoints.w, iSkin.y - 1u)] * inWeights.w;
    return m / w;
}

vec3 rotq(vec4 q, vec3 v) {
    return v + 2.0 * cross(q.xyz, cross(q.xyz, v) + q.w * v);
}

void main() {
    mat4 skin = skin_matrix();
    vec3 posed = (skin * vec4(inPos, 1.0)).xyz;
    vec3 local = posed * iScale + vec3(0.0, inCap * iMat.x, 0.0);
    vec3 world = rotq(iRot, local) + iPos;
    gl_Position = F.lightviewproj[int(M.extra.w + 0.5)] * vec4(world, 1.0);
}
