#version 450

layout(location = 0) in vec3 vN;
layout(location = 1) in vec3 vC;
layout(location = 0) out vec4 outColor;

layout(push_constant) uniform PC {
    mat4 viewproj;
    vec4 lightDir;
} pc;

void main() {
    vec3 n = normalize(vN);
    vec3 l = normalize(pc.lightDir.xyz);
    float diff = max(dot(n, l), 0.0);
    float rim  = pow(1.0 - max(dot(n, vec3(0.0, 0.0, 1.0)), 0.0), 3.0) * 0.15;
    vec3 c = vC * (0.22 + 0.78 * diff) + rim;
    outColor = vec4(pow(c, vec3(1.0 / 2.2)), 1.0);
}
