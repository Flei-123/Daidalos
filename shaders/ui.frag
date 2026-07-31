#version 450
// One texture, one blend mode. Text and sprites are the same draw: glyphs are
// white with coverage in alpha, sprites carry their own colour, and the vertex
// colour tints both.

layout(location = 0) in vec2 vUV;
layout(location = 1) in vec4 vColor;

layout(set = 1, binding = 0) uniform sampler2D uTex;

layout(location = 0) out vec4 outColor;

void main() {
    vec4 t = texture(uTex, vUV);
    vec4 c = t * vColor;
    if (c.a < 0.002) discard;
    // premultiplied output, matching the particle pass and the blend state
    outColor = vec4(c.rgb * c.a, c.a);
}
