#version 450
// Screen space UI: pixels in, clip space out. The viewport size arrives in the
// push constants, so resizing needs no descriptor update and no pipeline change.

layout(location = 0) in vec2 inPos;
layout(location = 1) in vec2 inUV;
layout(location = 2) in vec4 inColor;

layout(push_constant) uniform UI {
    vec4 screen;     // width, height, unused, unused
    vec4 pad1;
    vec4 pad2;
    vec4 pad3;
} U;

layout(location = 0) out vec2 vUV;
layout(location = 1) out vec4 vColor;

void main() {
    vUV = inUV;
    vColor = inColor;
    vec2 ndc = vec2(inPos.x / U.screen.x * 2.0 - 1.0, inPos.y / U.screen.y * 2.0 - 1.0);
    gl_Position = vec4(ndc, 0.0, 1.0);
}
