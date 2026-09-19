#version 450
// The composite: scene + bloom, then the four effects that sell a neon look,
// in ONE pass.
//
// One pass rather than four, because every separate fullscreen pass costs a
// full read and a full write of the frame - at 1280x720 that is 3.5 MB each
// way, and four of them would move 28 MB per frame to do arithmetic that fits
// in a handful of registers. The order below is the order light actually meets
// a camera: the lens bends the colours apart (aberration), the glow adds to
// what the lens delivered (bloom), the barrel darkens the corners (vignette),
// the sensor adds noise (grain), and the display's raster is last (scanlines).
// Flash sits with the bloom because it is light in the scene, not in the lens.

layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 outColor;

layout(set = 1, binding = 0) uniform sampler2D uScene;
layout(set = 2, binding = 0) uniform sampler2D uBloom;

layout(push_constant) uniform Push {
    vec4 p0;   // x = bloom_intensity, y = vignette, z = grain, w = aberration
    vec4 p1;   // x = scanlines, y = frame_index (as float), z = bloom_enabled, w = unused
    vec4 p2;   // xyz = flash colour, w = flash amount
    vec4 p3;   // xy = 1/width, 1/height of the destination
    vec4 p4;
    vec4 p5;
} P;

// Deterministic hash. The engine's whole contract is
// state(n+1) = step(state(n), input(n)) - see the README - so the grain may
// not read a clock. It reads the frame COUNTER the host passes in, which means
// the same frame index always produces the same noise, a replay grains
// identically, and tests/test_postfx.cpp [4] can assert exactly that.
// This is the fract/dot hash (Dave Hoskins' hash12), not the usual
// fract(sin(dot(...))): sin() of a large argument is where drivers disagree
// most, and a grain pattern that differs between two GPUs would break the
// "same frame_index gives the same picture" guarantee across machines - which
// is the half of determinism a single-machine test cannot catch.
float hash12(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

void main() {
    vec2 uv = vUv;
    // Centred, aspect-corrected radius. Without the aspect correction the
    // vignette is an ellipse that reaches the left and right edges long before
    // the top and bottom, which on 16:9 reads as a horizontal letterbox rather
    // than as a lens.
    vec2 texel = P.p3.xy;
    float aspect = texel.y / max(texel.x, 1e-6);
    vec2 d = uv - 0.5;
    vec2 da = vec2(d.x * aspect, d.y);
    float r = length(da) * 1.41421356;   // 1.0 at the corners

    // ---- chromatic aberration ------------------------------------------
    // Radial and growing outwards, because that is what a real lens does: the
    // centre of the frame is on the optical axis where the three wavelengths
    // land on the same spot, and the error grows with the distance from it. A
    // uniform channel offset would smear the middle of the screen, which is
    // the one place the eye is looking.
    vec3 scene;
    float ab = P.p0.w;
    if (ab > 0.0) {
        // r*r rather than r: a lens's transverse error is not linear, and the
        // square keeps the middle half of the frame visibly clean while the
        // corners still separate.
        vec2 off = d * ab * r * r;
        scene.r = texture(uScene, uv + off).r;
        scene.g = texture(uScene, uv).g;
        scene.b = texture(uScene, uv - off).b;
    } else {
        scene = texture(uScene, uv).rgb;
    }

    // ---- bloom ----------------------------------------------------------
    // Added, not mixed. Glow is light ARRIVING at the sensor on top of what is
    // already there; a lerp would darken the sign to brighten its halo.
    vec3 color = scene;
    if (P.p1.z > 0.5)
        color += texture(uBloom, uv).rgb * P.p0.x;

    // ---- flash ------------------------------------------------------------
    // The hit flash is additive for the same reason, and it is here rather
    // than after the vignette so that a flash at the edge of the screen is
    // darkened by the lens like everything else.
    color += P.p2.rgb * P.p2.w;

    // ---- vignette ---------------------------------------------------------
    // smoothstep from the middle outwards. The inner edge sits at 0.35 so the
    // centre third of the frame is untouched - a vignette that starts at the
    // centre is just a global exposure cut with extra maths.
    float v = P.p0.y;
    if (v > 0.0) {
        float fall = smoothstep(0.35, 1.25, r);
        color *= 1.0 - fall * v;
    }

    // ---- film grain -------------------------------------------------------
    // Signed noise, so grain brightens as often as it darkens and the average
    // brightness of the frame does not move. Scaled by (1 - luma) because film
    // grain is a property of the emulsion, not of the light: the highlights of
    // a real frame are comparatively clean, and noise applied flat makes a
    // white wall look like television static.
    float g = P.p0.z;
    if (g > 0.0) {
        // The frame counter goes in as an OFFSET in the hash's input rather
        // than as a multiplier, so consecutive frames give uncorrelated noise
        // instead of the same pattern sliding across the screen.
        vec2 seed = gl_FragCoord.xy + vec2(P.p1.y * 17.0, P.p1.y * 31.0);
        float n = hash12(seed) * 2.0 - 1.0;
        float luma = dot(color, vec3(0.2126, 0.7152, 0.0722));
        color += n * g * (1.0 - luma * 0.7);
    }

    // ---- scanlines --------------------------------------------------------
    // Every second ROW of the destination, from gl_FragCoord so the pattern is
    // locked to the pixel grid. Derived from uv it would shimmer as the window
    // is resized, because the line then falls between two pixels.
    float s = P.p1.x;
    if (s > 0.0) {
        float line = mod(floor(gl_FragCoord.y), 2.0);
        color *= 1.0 - s * line;
    }

    // The frame is read back by the tests and blitted to a window, both of
    // which expect a displayable value. Negative grain on a black pixel and an
    // additive bloom on a white one both leave the range.
    outColor = vec4(clamp(color, 0.0, 1.0), 1.0);
}
