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

layout(location = 0) out vec4 outColor;

void main() {
    vec2 d = vUV * 2.0 - 1.0;
    float r2 = dot(d, d);
    if (r2 > 1.0) discard;
    float falloff = 1.0 - r2;
    falloff *= falloff;                       // soft edge without a texture

    float a = clamp(vAlpha, 0.0, 1.0) * falloff;
    vec3 c = vColor * a;
    c = c / (1.0 + c);                        // keep it inside the tonemapped range
    outColor = vec4(pow(c, vec3(1.0 / 2.2)), vBlend == 1u ? 0.0 : a);
}
