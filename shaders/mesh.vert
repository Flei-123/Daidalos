#version 450

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inNrm;
// per instance
layout(location = 2) in vec3 iPos;
layout(location = 3) in vec4 iRot;
layout(location = 4) in vec3 iScale;
layout(location = 5) in vec3 iColor;

layout(push_constant) uniform PC {
    mat4 viewproj;
    vec4 lightDir;
} pc;

layout(location = 0) out vec3 vN;
layout(location = 1) out vec3 vC;

vec3 rotq(vec4 q, vec3 v) {
    return v + 2.0 * cross(q.xyz, cross(q.xyz, v) + q.w * v);
}

void main() {
    vec3 p = rotq(iRot, inPos * iScale) + iPos;
    gl_Position = pc.viewproj * vec4(p, 1.0);
    vN = rotq(iRot, inNrm);
    vC = iColor;
}
