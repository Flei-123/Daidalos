#version 450
// Bright pass: what is allowed to glow, at quarter resolution.
//
// The threshold is a KNEE, not a cut. A hard `if (luma > t)` makes the bloom
// pop in and out as a surface crosses the threshold between frames - a neon
// sign at the edge of the limit flickers, and a slow camera move across a
// gradient shows a visible contour line where the glow starts. The quadratic
// knee below ramps the contribution over a band of width 2*knee around the
// threshold, which is the standard Call-of-Duty/Unity curve, so a pixel just
// under the limit contributes a little instead of nothing.

layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 outColor;

layout(set = 1, binding = 0) uniform sampler2D uSrc;

layout(push_constant) uniform Push {
    vec4 p0;   // x = threshold, y = knee, z = intensity, w = unused
    vec4 p1;   // x = 1/width of the SOURCE, y = 1/height of the source
    vec4 p2;
    vec4 p3;
    vec4 p4;
    vec4 p5;
} P;

void main() {
    // Four taps in a box around the texel centre. The destination is a quarter
    // the size of the source, so one destination pixel covers four source
    // pixels; a single bilinear tap would read two of them and drop the other
    // two, and a lone bright pixel could vanish entirely depending on which
    // half of the box it sat in.
    vec2 t = P.p1.xy;
    vec3 c = texture(uSrc, vUv + vec2(-t.x, -t.y)).rgb;
    c += texture(uSrc, vUv + vec2( t.x, -t.y)).rgb;
    c += texture(uSrc, vUv + vec2(-t.x,  t.y)).rgb;
    c += texture(uSrc, vUv + vec2( t.x,  t.y)).rgb;
    c *= 0.25;

    float threshold = P.p0.x;
    float knee = max(P.p0.y, 1e-4);

    // Perceptual luma, not max(r,g,b): a saturated blue neon at (0,0,1) has a
    // max of 1.0 and would bloom as hard as white, which reads as a blown out
    // frame rather than a glowing sign.
    float luma = dot(c, vec3(0.2126, 0.7152, 0.0722));

    float soft = luma - threshold + knee;
    soft = clamp(soft, 0.0, 2.0 * knee);
    soft = soft * soft / (4.0 * knee);
    // max() of the soft ramp and the hard overshoot: below the knee band only
    // the ramp contributes, above it the honest excess does.
    float contrib = max(soft, luma - threshold) / max(luma, 1e-4);

    outColor = vec4(c * contrib, 1.0);
}
