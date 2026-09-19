// Daidalos - post processing, measured.
//
// Every check here reads PIXELS BACK and counts something. "The bloom looks
// nice" is not a test; "a bright spot 14 px wide covers 3.1x more pixels above
// half brightness after the chain ran" is, and it fails when somebody breaks
// the blur.
//
//   DAI_SHADER_DIR=shaders ./build/test_postfx [outdir]
//
// What is being pinned down:
//   [1] off means OFF - the frame is bit identical to one rendered by a
//       renderer that was never told about post processing
//   [2] bloom WIDENS a bright spot, and does it more than nothing at all
//   [3] the vignette darkens the corners MORE than the middle
//   [4] the grain is a function of frame_index and nothing else - same index
//       twice gives the same picture, a different index a different one
//   [5] aberration separates the channels at the edge and leaves the centre
//   [6] flash and scanlines do what their names say
//   [7] resize rebuilds the chain instead of sampling a freed image

#include "daidalos.h"
#include "dai_render.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

static int g_fail = 0, g_pass = 0;
static const char *g_outdir = ".";

#define CHECK(cond, ...) do { \
    if (cond) { ++g_pass; } \
    else { ++g_fail; std::printf("  FAIL %s:%d  ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } \
} while (0)

struct Frame {
    std::vector<uint8_t> px;
    uint32_t w = 0, h = 0;
    const uint8_t *at(uint32_t x, uint32_t y) const { return &px[((size_t)y * w + x) * 4]; }
    float lum(uint32_t x, uint32_t y) const {
        const uint8_t *p = at(x, y);
        return (0.2126f * p[0] + 0.7152f * p[1] + 0.0722f * p[2]) / 255.0f;
    }
    // Mean luminance of a square window, which is what "the corner" and "the
    // middle" mean in the vignette check - a single pixel would be at the
    // mercy of one grain sample.
    float box_lum(int cx, int cy, int rad) const {
        double sum = 0; int n = 0;
        for (int y = cy - rad; y <= cy + rad; ++y)
            for (int x = cx - rad; x <= cx + rad; ++x) {
                if (x < 0 || y < 0 || x >= (int)w || y >= (int)h) continue;
                sum += lum((uint32_t)x, (uint32_t)y); ++n;
            }
        return n ? (float)(sum / n) : 0.0f;
    }
    // Counts pixels at or above a luminance. This is the bloom measurement:
    // a glow is light where there was none, so the number of lit pixels goes
    // UP even though no geometry moved.
    int count_above(float t) const {
        int n = 0;
        for (uint32_t y = 0; y < h; ++y)
            for (uint32_t x = 0; x < w; ++x) if (lum(x, y) >= t) ++n;
        return n;
    }
};

// A digest over every byte of the frame. Used for the two claims that are
// about EQUALITY rather than about a quantity: "off changed nothing" and "the
// same frame_index grains identically". FNV-1a because it is four lines and
// this is a test, not a security boundary.
static uint64_t digest(const Frame &f) {
    uint64_t h = 1469598103934665603ull;
    for (uint8_t b : f.px) { h ^= b; h *= 1099511628211ull; }
    return h;
}

static Frame grab(dai_renderer *r) {
    Frame f;
    f.w = dai_render_width(r); f.h = dai_render_height(r);
    f.px.resize((size_t)f.w * f.h * 4);
    dai_render_readback(r, f.px.data(), f.px.size());
    return f;
}

// The canonical scene for these tests: ONE emissive white sphere in the middle
// of a black frame, no sun, no sky, no shadows. Everything measured here is a
// property of the post chain, so the scene has to contribute as little as
// possible - a lit surface with a falloff would make "wider" ambiguous.
static void build_scene(dai_renderer *r) {
    dai_render_sky(r, 0);                       // flat clear, easier to threshold
    dai_render_clear_color(r, 0.0f, 0.0f, 0.0f);
    dai_render_fog(r, 0.0f, dai_vec3{ 0, 0, 0 });
    dai_render_camera(r, dai_vec3{ 0, 0, 6 }, dai_vec3{ 0, 0, 0 }, dai_vec3{ 0, 1, 0 },
                      55.0f, 0.1f, 100.0f);
    // No sun and no ambient: the sphere below is fully emissive, so it is its
    // own light source. Anything lit would put a falloff on the surface, and
    // "the spot got wider" would then be measuring the shading instead of the
    // bloom.
    dai_render_light(r, dai_vec3{ 0.0f, 0.0f, 1.0f });
    dai_render_ambient(r, dai_vec3{ 0, 0, 0 }, dai_vec3{ 0, 0, 0 }, 0.0f);
    dai_render_exposure(r, 1.0f);
}

static dai_render_instance sphere_at(float x, float y, float s) {
    dai_render_instance i{};
    i.mesh = DAI_MESH_SPHERE;
    i.position = dai_vec3{ x, y, 0.0f };
    i.rotation = dai_quat{ 0, 0, 0, 1 };
    i.scale = dai_vec3{ s, s, s };
    i.color = dai_vec3{ 1, 1, 1 };
    // Fully emissive: lifts the sphere out of the lighting entirely, which is
    // what makes it a clean white disc on black for every measurement here.
    i.emissive = 1.0f;
    return i;
}

static dai_postfx base_fx() {
    dai_postfx fx{};
    fx.enabled = 1;
    fx.bloom_threshold = 0.7f;
    fx.bloom_knee = 0.3f;
    fx.bloom_intensity = 0.0f;   // each test switches ON only what it measures
    fx.frame_index = 1;
    return fx;
}

int main(int argc, char **argv) {
    if (argc > 1) g_outdir = argv[1];

    char err[256] = { 0 };
    dai_render_desc d{};
    d.width = 640; d.height = 360;
    // MSAA off: the post chain runs on the resolved image either way, and a
    // resolve adds a source of difference between two frames that these
    // bit-identity checks would have to reason about.
    d.msaa = 1;
    d.shadow_size = -1;
    dai_renderer *r = dai_render_create(&d, err, sizeof(err));
    if (!r) {
        std::printf("test_postfx: no renderer (%s) - skipping\n", err);
        std::printf("0 passed, 0 failed\n");
        return 0;
    }
    std::printf("device: %s\n", dai_render_device_name(r));

    build_scene(r);
    dai_render_instance inst = sphere_at(0.0f, 0.0f, 0.6f);

    // ---------------------------------------------------------------- [1]
    // OFF IS OFF. The reference is taken before dai_render_postfx is ever
    // called, i.e. from a renderer in exactly the state every existing test in
    // this repository renders in. Then the chain is armed and DISARMED, and
    // the frame has to come back the same to the byte. This is the check that
    // protects the reference images - if it ever goes red, every screenshot
    // comparison in the tree is quietly measuring post processing.
    dai_render_frame(r, &inst, 1);
    Frame ref = grab(r);
    uint64_t ref_dig = digest(ref);

    {
        dai_postfx fx = base_fx();
        fx.bloom_intensity = 1.0f; fx.vignette = 0.5f; fx.grain = 0.1f;
        fx.enabled = 0;   // everything set, but switched off
        dai_render_postfx(r, &fx);
        dai_render_frame(r, &inst, 1);
        Frame f = grab(r);
        CHECK(digest(f) == ref_dig,
              "[1a] enabled=0 changed the frame (digest %llu vs %llu) - every "
              "reference image in the repo is now wrong",
              (unsigned long long)digest(f), (unsigned long long)ref_dig);
    }
    {
        dai_render_postfx(r, nullptr);   // NULL is documented to mean off
        dai_render_frame(r, &inst, 1);
        Frame f = grab(r);
        CHECK(digest(f) == ref_dig, "[1b] postfx(NULL) changed the frame");
    }

    // A frame WITH the chain on must differ - otherwise [1] is passing because
    // the chain does nothing at all, which would make the whole suite green
    // and meaningless.
    {
        dai_postfx fx = base_fx();
        fx.bloom_intensity = 1.0f;
        dai_render_postfx(r, &fx);
        dai_render_frame(r, &inst, 1);
        Frame f = grab(r);
        CHECK(digest(f) != ref_dig,
              "[1c] the chain is ON and the frame is unchanged - the post pass "
              "is not running, so [1a] proves nothing");
    }

    // ---------------------------------------------------------------- [2]
    // BLOOM WIDENS. Count the pixels that carry ANY light. The threshold is
    // deliberately just above black rather than "half brightness": a halo is
    // dim by nature - measured here it runs from about 26/255 at the edge of
    // the sphere down to 0 about 70 px out - and counting only bright pixels
    // would measure the sphere's own silhouette, which post processing does
    // not move.
    {
        const float kLit = 0.01f;   // ~3/255, above the encoder's noise floor

        dai_render_postfx(r, nullptr);
        dai_render_frame(r, &inst, 1);
        Frame off = grab(r);
        int lit_off = off.count_above(kLit);

        dai_postfx fx = base_fx();
        fx.bloom_intensity = 1.2f;
        dai_render_postfx(r, &fx);
        dai_render_frame(r, &inst, 1);
        Frame on = grab(r);
        int lit_on = on.count_above(kLit);

        std::printf("  bloom: %d lit pixels off -> %d on (x%.2f)\n",
                    lit_off, lit_on, lit_off ? (double)lit_on / lit_off : 0.0);
        CHECK(lit_on > lit_off * 3 / 2,
              "[2a] bloom did not widen the spot: %d lit pixels without, %d with "
              "(wanted at least 1.5x)", lit_off, lit_on);

        // ...and the widening has to be a HALO, not a global brightening: a
        // shader that simply added a constant would also raise the count. The
        // corner of the frame is far from the sphere and must stay black.
        CHECK(on.box_lum(8, 8, 6) < 0.004f,
              "[2b] bloom brightened the far corner to %.4f - that is a "
              "constant add, not a glow", on.box_lum(8, 8, 6));

        // The glow falls off with distance. Both probes sit OUTSIDE the
        // sphere, whose silhouette reaches about 68 px from the centre at this
        // camera distance, and the near one has to be brighter than the far.
        int cx = (int)on.w / 2, cy = (int)on.h / 2;
        // The sphere's silhouette reaches ~35 px above the centre at this
        // camera distance, so both probes sit clear of it: 42 px out is inside
        // the halo, 60 px out is near its end.
        float near_halo = on.box_lum(cx, cy - 42, 3);
        float far_halo  = on.box_lum(cx, cy - 60, 3);
        std::printf("  bloom falloff: near %.4f, far %.4f\n", near_halo, far_halo);
        CHECK(near_halo > far_halo,
              "[2c] the halo does not fall off with distance (near %.4f, far %.4f)",
              near_halo, far_halo);
        // The near probe must actually SEE something, or [2c] would pass on
        // two zeroes - which is exactly how this check first went green.
        CHECK(near_halo > 0.002f,
              "[2d] there is no halo outside the sphere at all (%.4f) - the "
              "bloom is not reaching the composite", near_halo);

        // Intensity has to be a dial, not a switch.
        fx.bloom_intensity = 0.4f;
        dai_render_postfx(r, &fx);
        dai_render_frame(r, &inst, 1);
        Frame weak = grab(r);
        float weak_halo = weak.box_lum(cx, cy - 42, 3);
        std::printf("  bloom intensity 0.4 -> %.4f, 1.2 -> %.4f\n", weak_halo, near_halo);
        CHECK(weak_halo < near_halo,
              "[2e] a third of the intensity gave the same halo (%.4f vs %.4f)",
              weak_halo, near_halo);
    }

    // ---------------------------------------------------------------- [3]
    // THE VIGNETTE DARKENS THE CORNERS MORE THAN THE MIDDLE. Measured against
    // the SAME scene without it, as a ratio, so the sphere's own brightness
    // cancels out of the comparison.
    {
        dai_render_postfx(r, nullptr);
        dai_render_frame(r, &inst, 1);
        Frame off = grab(r);

        dai_postfx fx = base_fx();
        fx.vignette = 0.8f;
        dai_render_postfx(r, &fx);
        dai_render_frame(r, &inst, 1);
        Frame on = grab(r);

        // A flat grey field would make this trivial, but the scene is a black
        // frame with a sphere in it, so the corners are already black and a
        // ratio there is 0/0. Use the flash to fill the frame with an even
        // colour FIRST - that is the one effect that writes every pixel.
        dai_postfx flat = base_fx();
        flat.flash_color[0] = flat.flash_color[1] = flat.flash_color[2] = 1.0f;
        flat.flash_amount = 0.6f;
        dai_render_postfx(r, &flat);
        dai_render_frame(r, &inst, 1);
        Frame flat_off = grab(r);

        flat.vignette = 0.8f;
        dai_render_postfx(r, &flat);
        dai_render_frame(r, &inst, 1);
        Frame flat_on = grab(r);

        int cx = (int)flat_on.w / 2, cy = (int)flat_on.h / 2;
        float mid_before = flat_off.box_lum(cx, cy, 10);
        float mid_after  = flat_on.box_lum(cx, cy, 10);
        float cor_before = flat_off.box_lum(12, 12, 10);
        float cor_after  = flat_on.box_lum(12, 12, 10);
        float mid_keep = mid_before > 0 ? mid_after / mid_before : 1.0f;
        float cor_keep = cor_before > 0 ? cor_after / cor_before : 1.0f;
        std::printf("  vignette: middle keeps %.3f of its light, corner keeps %.3f\n",
                    mid_keep, cor_keep);
        CHECK(cor_keep < mid_keep - 0.15f,
              "[3a] the corner is not darkened meaningfully more than the middle "
              "(corner keeps %.3f, middle %.3f)", cor_keep, mid_keep);
        // The centre third is documented as untouched (smoothstep starts at
        // 0.35), and a vignette that dims the middle is an exposure cut.
        CHECK(mid_keep > 0.97f,
              "[3b] the vignette dimmed the CENTRE to %.3f - it should start "
              "outside the middle third", mid_keep);
        (void)off; (void)on;
    }

    // ---------------------------------------------------------------- [4]
    // GRAIN IS DETERMINISTIC. This is the check that holds the engine's one
    // rule at the renderer's edge: the noise is a function of frame_index, so
    // the same index twice is the same picture and a replay grains identically.
    {
        dai_postfx fx = base_fx();
        fx.grain = 0.25f;

        fx.frame_index = 7;
        dai_render_postfx(r, &fx);
        dai_render_frame(r, &inst, 1);
        Frame a = grab(r);
        uint64_t da = digest(a);

        // A different frame in between, to prove the result depends on the
        // INDEX and not merely on "the previous call produced this".
        fx.frame_index = 99;
        dai_render_postfx(r, &fx);
        dai_render_frame(r, &inst, 1);
        Frame b = grab(r);
        uint64_t db = digest(b);

        fx.frame_index = 7;
        dai_render_postfx(r, &fx);
        dai_render_frame(r, &inst, 1);
        Frame a2 = grab(r);
        uint64_t da2 = digest(a2);

        CHECK(da == da2,
              "[4a] the same frame_index gave two different pictures (%llu vs %llu) "
              "- the grain is reading something that is not its input",
              (unsigned long long)da, (unsigned long long)da2);
        CHECK(da != db,
              "[4b] two different frame_index values gave the SAME picture - the "
              "counter is not reaching the shader, so the grain is frozen");

        // And it has to actually be noise: count how many pixels differ
        // between two frame indices. A handful would mean the hash is nearly
        // constant.
        int diff = 0;
        for (size_t i = 0; i < a.px.size(); i += 4)
            if (a.px[i] != b.px[i]) ++diff;
        double frac = (double)diff / (a.px.size() / 4);
        std::printf("  grain: %.1f%% of pixels differ between frame 7 and frame 99\n",
                    frac * 100.0);
        CHECK(frac > 0.5,
              "[4c] only %.1f%% of pixels changed between two frame indices - "
              "that is not film grain", frac * 100.0);
    }

    // ---------------------------------------------------------------- [5]
    // CHROMATIC ABERRATION separates the channels, radially. Measured on the
    // flat flash field so there is an edge everywhere, and compared between
    // the centre (on the optical axis, must stay clean) and the corner.
    {
        dai_postfx fx = base_fx();
        fx.flash_color[0] = fx.flash_color[1] = fx.flash_color[2] = 1.0f;
        fx.flash_amount = 0.5f;
        dai_render_postfx(r, &fx);
        dai_render_frame(r, &inst, 1);
        Frame off = grab(r);

        fx.aberration = 0.02f;
        dai_render_postfx(r, &fx);
        dai_render_frame(r, &inst, 1);
        Frame on = grab(r);

        // The sphere's edge is the only place with a colour gradient, so look
        // for the R/B split along it: walk a horizontal line through the
        // sphere and take the largest |R-B| found.
        auto max_split = [](const Frame &f, uint32_t y) {
            int worst = 0;
            for (uint32_t x = 0; x < f.w; ++x) {
                const uint8_t *p = f.at(x, y);
                int s = std::abs((int)p[0] - (int)p[2]);
                if (s > worst) worst = s;
            }
            return worst;
        };
        int split_off = max_split(off, off.h / 2);
        int split_on  = max_split(on, on.h / 2);
        std::printf("  aberration: max |R-B| on the centre line %d -> %d\n",
                    split_off, split_on);
        CHECK(split_on > split_off,
              "[5a] aberration produced no channel separation at all (%d -> %d)",
              split_off, split_on);
        // The middle of the frame is on the axis: r*r is ~0 there, so the
        // three channels must still land on the same pixel.
        const uint8_t *c = on.at(on.w / 2, on.h / 2);
        CHECK(std::abs((int)c[0] - (int)c[2]) <= 2,
              "[5b] the exact centre split by %d - aberration must be zero on "
              "the optical axis", std::abs((int)c[0] - (int)c[2]));
    }

    // ---------------------------------------------------------------- [6]
    // FLASH and SCANLINES.
    {
        dai_render_postfx(r, nullptr);
        dai_render_frame(r, &inst, 1);
        Frame plain = grab(r);
        float corner_plain = plain.box_lum(10, 10, 6);

        dai_postfx fx = base_fx();
        fx.flash_color[0] = 1.0f; fx.flash_color[1] = 0.2f; fx.flash_color[2] = 0.2f;
        fx.flash_amount = 0.5f;
        dai_render_postfx(r, &fx);
        dai_render_frame(r, &inst, 1);
        Frame flash = grab(r);
        float corner_flash = flash.box_lum(10, 10, 6);
        CHECK(corner_flash > corner_plain + 0.05f,
              "[6a] a 0.5 flash did not brighten the black corner (%.3f -> %.3f)",
              corner_plain, corner_flash);
        const uint8_t *fp = flash.at(10, 10);
        CHECK(fp[0] > fp[1] + 40,
              "[6b] the flash colour did not survive: r=%d g=%d b=%d for a red flash",
              fp[0], fp[1], fp[2]);

        // Scanlines: every second ROW darker. Compare the mean of the even
        // rows against the odd ones over the flash-filled frame.
        dai_postfx sc = base_fx();
        sc.flash_color[0] = sc.flash_color[1] = sc.flash_color[2] = 1.0f;
        sc.flash_amount = 0.6f;
        sc.scanlines = 0.5f;
        dai_render_postfx(r, &sc);
        dai_render_frame(r, &inst, 1);
        Frame lines = grab(r);
        double even = 0, odd = 0; int ne = 0, no = 0;
        for (uint32_t y = 0; y < lines.h; ++y)
            for (uint32_t x = 0; x < lines.w; x += 4) {
                if (y % 2) { odd += lines.lum(x, y); ++no; }
                else { even += lines.lum(x, y); ++ne; }
            }
        even /= ne; odd /= no;
        std::printf("  scanlines: even rows %.3f, odd rows %.3f\n", even, odd);
        CHECK(std::fabs(even - odd) > 0.1,
              "[6c] scanlines did not alternate (even %.3f, odd %.3f)", even, odd);
    }

    // ---------------------------------------------------------------- [7]
    // RESIZE REBUILDS THE CHAIN. The post images are the frame's size and the
    // composite's descriptor points at color_rt's view; a resize destroys both.
    // Without vk_post_make_targets being called again this frame samples freed
    // memory - which usually does not crash, it just renders garbage, so the
    // check is that the frame is still the picture it should be.
    {
        CHECK(dai_render_resize(r, 800, 480) == DAI_OK, "[7a] resize failed");
        dai_postfx fx = base_fx();
        fx.bloom_intensity = 1.2f;
        dai_render_postfx(r, &fx);
        dai_render_frame(r, &inst, 1);
        Frame f = grab(r);
        CHECK(f.w == 800 && f.h == 480, "[7b] resize did not take: %ux%u", f.w, f.h);
        // The sphere is still in the middle and the corner is still black:
        // a chain pointing at a dead image gives neither.
        float mid = f.box_lum((int)f.w / 2, (int)f.h / 2, 8);
        float cor = f.box_lum(10, 10, 8);
        std::printf("  after resize: middle %.3f, corner %.3f\n", mid, cor);
        CHECK(mid > 0.5f, "[7c] the sphere vanished after a resize (middle %.3f)", mid);
        CHECK(cor < 0.05f, "[7d] the corner is not black after a resize (%.3f) - "
                           "the chain is sampling a stale image", cor);

        // ...and OFF is still bit identical at the new size, which is the
        // claim [1] makes, re-checked after the images were rebuilt.
        dai_render_postfx(r, nullptr);
        dai_render_frame(r, &inst, 1);
        Frame off1 = grab(r);
        dai_render_frame(r, &inst, 1);
        Frame off2 = grab(r);
        CHECK(digest(off1) == digest(off2),
              "[7e] two identical frames with post-fx off differ after a resize");
        dai_render_resize(r, 640, 360);
    }

    // ---------------------------------------------------------------- [8]
    // WHAT A WINDOW WOULD SHOW is what the readback returns. The window
    // backends blit from vk_present_image(), not from color_rt, because the
    // processed frame lives in post_rt - and a window showing the plain frame
    // while dai_render_readback returned the graded one is the same frame
    // looking like two pictures depending on how you asked for it.
    //
    // That accessor is internal, so what is checked here is the property it
    // exists for: switching the chain OFF after a frame was drawn with it ON
    // must not retroactively change that finished frame. A host that draws,
    // then disarms, then presents, still presents what it drew.
    {
        dai_postfx fx = base_fx();
        fx.bloom_intensity = 1.2f;
        fx.vignette = 0.5f;
        dai_render_postfx(r, &fx);
        dai_render_frame(r, &inst, 1);
        Frame drawn = grab(r);

        // Disarm WITHOUT drawing again. The finished frame is still the
        // processed one; nothing may change until the next dai_render_frame.
        dai_render_postfx(r, nullptr);
        Frame after_disarm = grab(r);
        CHECK(digest(drawn) == digest(after_disarm),
              "[8a] disarming the chain changed a frame that was already drawn");

        // ...and the NEXT frame is the plain one again.
        dai_render_frame(r, &inst, 1);
        Frame plain = grab(r);
        CHECK(digest(plain) != digest(drawn),
              "[8b] the frame after disarming is still the processed one");
        CHECK(digest(plain) == ref_dig,
              "[8c] the frame after disarming does not match the untouched "
              "reference - something in the chain leaked into the plain path");
    }

    // ---------------------------------------------------------------- cost
    // The number quoted in docs/POSTFX.md. Not a check - the machine this runs
    // on decides it - but a measurement printed where the run can be read, so
    // the documentation cannot quote a number nobody produced.
    {
        const int N = 12;
        dai_render_resize(r, 1280, 720);
        dai_render_postfx(r, nullptr);
        double sum_off = 0;
        for (int i = 0; i < N; ++i) { dai_render_frame(r, &inst, 1); sum_off += dai_render_last_ms(r); }

        dai_postfx fx = base_fx();
        fx.bloom_intensity = 0.9f; fx.vignette = 0.35f; fx.grain = 0.04f;
        fx.aberration = 0.004f; fx.scanlines = 0.08f;
        double sum_full = 0;
        for (int i = 0; i < N; ++i) {
            fx.frame_index = (uint32_t)i;
            dai_render_postfx(r, &fx);
            dai_render_frame(r, &inst, 1);
            sum_full += dai_render_last_ms(r);
        }

        // ...and the same with the bloom switched off, which is the cheap
        // half: one pass instead of four.
        fx.bloom_intensity = 0.0f;
        double sum_nobloom = 0;
        for (int i = 0; i < N; ++i) {
            fx.frame_index = (uint32_t)i;
            dai_render_postfx(r, &fx);
            dai_render_frame(r, &inst, 1);
            sum_nobloom += dai_render_last_ms(r);
        }
        std::printf("COST 1280x720 over %d frames: off %.2f ms, composite only %.2f ms, "
                    "full chain %.2f ms (bloom+composite costs %.2f ms)\n",
                    N, sum_off / N, sum_nobloom / N, sum_full / N,
                    (sum_full - sum_off) / N);
        dai_render_postfx(r, nullptr);
        dai_render_resize(r, 640, 360);
    }

    dai_render_destroy(r);
    std::printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
