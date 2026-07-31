#version 450
// Soft round sprite, premultiplied alpha.
//
// Premultiplied is what lets ONE pipeline do both blend modes: an additive
// particle outputs colour with zero alpha, so the destination is added to
// rather than mixed with. No second pipeline, no sorting between modes.

layout(location = 0) in vec2 vUV;
layout(location = 1) in vec3 vColor;
layout(location = 2) in float vAlpha;
layout(location = 3) flat in uint vBlend;

layout(set = 1, binding = 0) uniform sampler2D uAtlas;

layout(push_constant) uniform Atlas {
    vec4 grid;      // cols, rows, has_texture, unused
    vec4 pad1;
    vec4 pad2;
    vec4 pad3;
} A;

layout(location = 0) out vec4 outColor;

void main() {
    vec3 tint = vColor;
    float falloff;
    if (A.grid.z > 0.5) {
        vec4 t = texture(uAtlas, vUV);
        falloff = t.a;                        // the sprite decides its own shape
        tint *= t.rgb;
        if (falloff < 0.004) discard;
    } else {
        // procedural soft dot, in cell local coordinates
        vec2 cell = vec2(A.grid.x, A.grid.y);
        vec2 local = fract(vUV * cell) * 2.0 - 1.0;
        float r2 = dot(local, local);
        if (r2 > 1.0) discard;
        falloff = (1.0 - r2) * (1.0 - r2);
    }

    float a = clamp(vAlpha, 0.0, 1.0) * falloff;
    vec3 c = tint * a;
    c = c / (1.0 + c);                        // keep it inside the tonemapped range
    outColor = vec4(pow(c, vec3(1.0 / 2.2)), vBlend == 1u ? 0.0 : a);
}
