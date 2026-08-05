#version 450
// Flat colour for world-space lines. Premultiplied alpha out, like the UI pass.

layout(location = 0) out vec4 outColor;

layout(push_constant) uniform Lines {
    vec4 color;   // rgb, premultiplied by alpha already
} L;

void main() {
    outColor = vec4(L.color.rgb, 1.0);
}
