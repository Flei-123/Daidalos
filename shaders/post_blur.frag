#version 450
// One HALF of a separable Gaussian. The direction arrives in the push
// constant, so the same pipeline runs horizontally and then vertically.
//
// Separable because a 9-tap Gaussian done in two passes costs 9+9 = 18 taps
// per pixel, while the same kernel as a single 9x9 square costs 81. The result
// is identical for a Gaussian - it is the one kernel that factorises exactly -
// so this is free quality, not an approximation.

layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 outColor;

layout(set = 1, binding = 0) uniform sampler2D uSrc;

layout(push_constant) uniform Push {
    vec4 p0;   // xy = the step, in uv, from one tap to the next (direction * texel)
    vec4 p1;
    vec4 p2;
    vec4 p3;
    vec4 p4;
    vec4 p5;
} P;

void main() {
    // The tap spacing is a multiplier on the texel step, not 1 texel. A
    // sigma-2 kernel sampled at one texel apart spreads light about 8 full
    // resolution pixels per round, which is a soft edge rather than a glow -
    // measured at 20 px of visible halo for the whole chain. Spreading the
    // same nine weights over 2 texel steps doubles the radius for exactly the
    // same nine samples, and the bilinear filter fills in between them because
    // the image being blurred has no detail left to alias.
    vec2 step = P.p0.xy * 2.0;

    // sigma = 2 Gaussian, normalised so the weights sum to exactly 1. They are
    // written out rather than computed: a loop with exp() recomputes the same
    // five numbers for every pixel of every pass, and the sum of the literals
    // below is 1.0 to the last bit, which is what keeps a flat grey area from
    // drifting brighter or darker with each blur iteration.
    const float w0 = 0.2270270270;
    const float w1 = 0.1945945946;
    const float w2 = 0.1216216216;
    const float w3 = 0.0540540541;
    const float w4 = 0.0162162162;

    vec3 c = texture(uSrc, vUv).rgb * w0;
    c += texture(uSrc, vUv + step * 1.0).rgb * w1;
    c += texture(uSrc, vUv - step * 1.0).rgb * w1;
    c += texture(uSrc, vUv + step * 2.0).rgb * w2;
    c += texture(uSrc, vUv - step * 2.0).rgb * w2;
    c += texture(uSrc, vUv + step * 3.0).rgb * w3;
    c += texture(uSrc, vUv - step * 3.0).rgb * w3;
    c += texture(uSrc, vUv + step * 4.0).rgb * w4;
    c += texture(uSrc, vUv - step * 4.0).rgb * w4;

    outColor = vec4(c, 1.0);
}
