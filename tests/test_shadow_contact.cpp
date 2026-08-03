// Daidalos - peter panning: does the shadow still touch the object?
//
//   g++ -std=c++17 -O3 -Iinclude tests/test_shadow_contact.cpp $VKLIBS \
//       -o build/test_shadow_contact
//   DAI_SHADER_DIR=shaders ./build/test_shadow_contact [outdir]
//
// (not wired into build.sh here - the file that does that was being edited by
// someone else at the time; one line next to test_render_visual is all it
// needs. DAI_NO_PPM=1 skips writing the pictures.)
//
// The bug this measures: a crate standing ON the floor whose shadow starts a
// few centimetres AWAY from its foot, leaving a bright band between the object
// and its own shadow. It has exactly two causes, and they are both a bias:
// either the shadow map holds the caster's BACK faces (front face culling), in
// which case the occluder near the contact point IS the receiver and no depth
// test can separate them, or the depth/normal bias is so large that the first
// centimetres of the shadow clear it.
//
// The measurement needs no projection maths - three renders and a subtraction:
//   A = floor alone
//   C = floor + a caster that does NOT cast   -> silhouette = |C - A|
//   B = floor + a caster that DOES cast       -> shadow     = lum(C) - lum(B)
// For every column of pixels: the last silhouette pixel, then the first shadow
// pixel below it. What lies between is the gap. A shadow that is attached to
// its object measures 0. The sun is tilted so the shadow falls TOWARDS the
// camera, which puts the contact edge and the gap on the same pixel column.
//
// The counter measurement lives in the same file on purpose: peter panning and
// shadow acne are one knob turned in opposite directions, so a "fix" that only
// reports the gap is worth nothing. ACNE is measured where NOTHING may be
// shadowed: a floor lit at a glancing angle, rendered once with the floor
// casting and once with it flagged NO_SHADOW. Every pixel that got darker is
// the floor shadowing itself.

#include "daidalos.h"
#include "dai_render.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <vector>
#include <algorithm>

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
    // sum of the absolute channel differences, 0..3
    float diff(const Frame &o, uint32_t x, uint32_t y) const {
        const uint8_t *a = at(x, y), *b = o.at(x, y);
        return (std::abs(a[0]-b[0]) + std::abs(a[1]-b[1]) + std::abs(a[2]-b[2])) / 255.0f;
    }
};

static Frame grab(dai_renderer *r) {
    Frame f;
    f.w = dai_render_width(r); f.h = dai_render_height(r);
    f.px.resize((size_t)f.w * f.h * 4);
    dai_render_readback(r, f.px.data(), f.px.size());
    return f;
}

static void save(dai_renderer *r, const char *name) {
    char path[512];
    std::snprintf(path, sizeof(path), "%s/%s", g_outdir, name);
    dai_render_write_ppm(r, path);
}

// ---------------------------------------------------------------- the scene
struct Scene {
    const char *tag;
    uint32_t mesh;
    dai_vec3 caster_scale;
    dai_vec3 caster_pos;      // y is set so the object sits ON the floor
    dai_vec3 floor_pos, floor_scale;
    dai_vec3 eye, target;
    float fov, zfar, extent;
    dai_vec3 sun;             // points TOWARDS the sun
};

struct Gap {
    double mean = 0, median = 0;      // 50% of full shadow: where the eye puts the edge
    double mean02 = 0;                // first pixel that is darkened at all
    int worst = 0, cols = 0, missing = 0;
    double px_per_m = 0;      // measured, not guessed
    double shadow_px = 0;     // how deep the shadow band is - a sanity check
};

// One column of the picture: the last object pixel, then the first shadow
// pixel under it. `obj` and `sh` are the two masks.
static Gap measure_gap(dai_renderer *r, const Scene &s, int save_ppm) {
    dai_render_sky(r, 0);
    dai_render_clear_color(r, 0, 0, 0);
    dai_render_light(r, s.sun);
    dai_render_shadow_extent(r, s.extent);
    dai_render_camera(r, s.eye, s.target, dai_vec3{ 0,1,0 }, s.fov, 0.1f, s.zfar);

    dai_render_instance fl = dai_render_instance_default();
    fl.position = s.floor_pos; fl.scale = s.floor_scale;
    fl.color = { 0.85f, 0.72f, 0.20f };                   // the yellow floor of the report

    dai_render_instance ca = dai_render_instance_default();
    ca.mesh = s.mesh; ca.scale = s.caster_scale;
    ca.position = s.caster_pos;
    ca.color = { 0.15f, 0.75f, 0.22f };                   // the green cylinder of the report

    // A: floor alone
    dai_render_frame(r, &fl, 1);
    Frame A = grab(r);

    // C: caster present but not casting -> the silhouette, and the lit floor
    dai_render_instance in[2] = { fl, ca };
    in[1].flags |= DAI_RI_NO_SHADOW;
    dai_render_frame(r, in, 2);
    Frame C = grab(r);
    if (save_ppm) { char n[128]; std::snprintf(n, sizeof(n), "gap_%s_nocast.ppm", s.tag); save(r, n); }

    // B: the real picture
    in[1] = ca;
    dai_render_frame(r, in, 2);
    Frame B = grab(r);
    if (save_ppm) { char n[128]; std::snprintf(n, sizeof(n), "gap_%s.ppm", s.tag); save(r, n); }

    // scale: shift the caster 25 cm towards the camera and see how far its
    // foot moved on screen. That is the pixels-per-metre AT THE CONTACT, which
    // is the only place the number means anything.
    dai_render_instance in2[2] = { fl, ca };
    in2[1].flags |= DAI_RI_NO_SHADOW;
    in2[1].position.z += 0.25f;
    dai_render_frame(r, in2, 2);
    Frame D = grab(r);

    Gap g;
    const float T_OBJ = 0.06f;    // silhouette: clearly a different colour
    const float T_SH  = 0.020f;   // shadow: clearly darker

    // The foot of the object is where its silhouette is HALF covered - the
    // pixel row a 1px antialiased edge would otherwise steal from the gap.
    auto foot = [&](const Frame &F, uint32_t x, int *out) {
        float peak = 0.0f;
        for (uint32_t y = 0; y < F.h; ++y) { float d = F.diff(A, x, y); if (d > peak) peak = d; }
        if (peak < T_OBJ) { *out = -1; return; }
        int last = -1;
        for (uint32_t y = 0; y < F.h; ++y) if (F.diff(A, x, y) > 0.5f * peak) last = (int)y;
        *out = last;
    };

    // the columns the object actually covers, middle 60% of them
    int x0 = 1 << 30, x1 = -1;
    for (uint32_t x = 0; x < C.w; ++x) { int f; foot(C, x, &f); if (f >= 0) { if ((int)x < x0) x0 = x; if ((int)x > x1) x1 = x; } }
    if (x1 < x0) { std::printf("     [%s] the caster is not in the picture at all\n", s.tag); return g; }
    int span = x1 - x0 + 1;
    int cx0 = x0 + span * 20 / 100, cx1 = x1 - span * 20 / 100;

    // px per metre from the shifted render
    {
        double sum = 0; int n = 0;
        for (int x = cx0; x <= cx1; ++x) {
            int a, b; foot(C, x, &a); foot(D, x, &b);
            if (a >= 0 && b >= 0) { sum += (b - a); ++n; }   // moved towards the camera = down the screen
        }
        g.px_per_m = n ? (sum / n) / 0.25 : 0.0;
    }

    std::vector<int> gaps;
    double shsum = 0, sum02 = 0;
    for (int x = cx0; x <= cx1; ++x) {
        int f; foot(C, x, &f);
        if (f < 0) continue;
        // how dark this column gets once it IS in shadow
        float peak = 0.0f;
        for (uint32_t y = f + 1; y < B.h; ++y) {
            float drop = C.lum(x, y) - B.lum(x, y);
            if (drop > peak) peak = drop;
        }
        if (peak < T_SH) { ++g.missing; continue; }
        int first02 = -1, first50 = -1, depth = 0;
        for (uint32_t y = f + 1; y < B.h; ++y) {
            float drop = C.lum(x, y) - B.lum(x, y);
            if (drop > T_SH) { if (first02 < 0) first02 = (int)y; ++depth; }
            if (drop > 0.5f * peak && first50 < 0) first50 = (int)y;
            if (first02 >= 0 && drop <= T_SH && (int)y > first02 + 2) break;
        }
        if (first50 < 0) { ++g.missing; continue; }
        gaps.push_back(first50 - f - 1);
        sum02 += (first02 - f - 1);
        shsum += depth;
    }
    if (gaps.empty()) { std::printf("     [%s] no shadow found under the caster at all\n", s.tag); return g; }
    std::sort(gaps.begin(), gaps.end());
    g.cols = (int)gaps.size();
    g.worst = gaps.back();
    g.median = gaps[gaps.size() / 2];
    double sum = 0; for (int v : gaps) sum += v;
    g.mean = sum / gaps.size();
    g.mean02 = sum02 / gaps.size();
    g.shadow_px = shsum / gaps.size();

    std::printf("     [%s] GAP %.2f px (%.0f mm) | fully bright band %.2f px | worst %d | "
                "%.1f px/m | shadow %.0f px deep | %d cols, %d without shadow\n",
                s.tag, g.mean, g.px_per_m > 0.01 ? 1000.0 * g.mean / g.px_per_m : 0.0,
                g.mean02, g.worst, g.px_per_m, g.shadow_px, g.cols, g.missing);
    return g;
}

// ------------------------------------------------------------------- acne
// A floor at a glancing angle that nothing shadows. Rendered twice: casting
// and not casting. Every pixel that got darker is the floor shadowing itself.
struct Acne { int px = 0; double frac = 0; int swings = 0; int worst_line = 0; };

static Acne measure_acne(dai_renderer *r, int save_ppm) {
    dai_render_sky(r, 0);
    dai_render_clear_color(r, 0, 0, 0);
    dai_render_light(r, dai_vec3{ 0.62f, 0.14f, 0.62f });      // ~9 degrees: past the shader's slope clamp
    dai_render_shadow_extent(r, 70.0f);
    dai_render_camera(r, dai_vec3{ 0, 3.0f, 20.0f }, dai_vec3{ 0, 0.0f, -30.0f }, dai_vec3{ 0,1,0 },
                      55.0f, 0.1f, 300.0f);

    dai_render_instance fl = dai_render_instance_default();
    fl.position = { 0, -1, -20 }; fl.scale = { 60, 1, 60 };
    fl.color = { 0.70f, 0.70f, 0.70f };

    dai_render_instance no = fl; no.flags |= DAI_RI_NO_SHADOW;
    dai_render_frame(r, &no, 1);
    Frame R = grab(r);

    dai_render_frame(r, &fl, 1);
    Frame S = grab(r);
    if (save_ppm) save(r, "acne_floor.ppm");

    Acne a;
    int floor_px = 0;
    for (uint32_t y = 0; y < S.h; ++y)
        for (uint32_t x = 0; x < S.w; ++x) {
            if (R.lum(x, y) < 0.02f) continue;                  // background
            ++floor_px;
            if (R.lum(x, y) - S.lum(x, y) > 0.02f) ++a.px;
        }
    a.frac = floor_px ? (double)a.px / floor_px : 0.0;

    // banding, the same way test_render_visual [19] counts it
    for (uint32_t y = S.h * 45 / 100; y < S.h * 95 / 100; ++y) {
        int sw = 0; bool dark = false;
        float ref = S.lum(0, y);
        for (uint32_t x = 0; x < S.w; ++x) {
            float l = S.lum(x, y);
            if (!dark && l < ref * 0.80f) { dark = true; ++sw; }
            else if (dark && l > ref * 0.92f) dark = false;
            ref = ref * 0.9f + l * 0.1f;
        }
        a.swings += sw;
        if (sw > a.worst_line) a.worst_line = sw;
    }
    std::printf("     [acne] %d of %d floor pixels self shadowed (%.3f%%), %d bands (worst line %d)\n",
                a.px, floor_px, 100.0 * a.frac, a.swings, a.worst_line);
    return a;
}

// ---------------------------------------------------- acne on a convex thing
// A sphere cannot shadow itself - it is convex. So on a lone sphere lit at a
// glancing angle, EVERY pixel the shadow map darkens is acne, and a sphere
// walks through every surface slope there is, which a flat floor does not.
static Acne measure_acne_convex(dai_renderer *r, int save_ppm) {
    dai_render_sky(r, 0);
    dai_render_clear_color(r, 0, 0, 0);
    dai_render_light(r, dai_vec3{ 0.66f, 0.16f, 0.42f });      // low sun, ~11 degrees
    dai_render_shadow_extent(r, 70.0f);                        // big cascade, big texels
    dai_render_camera(r, dai_vec3{ 0, 2.0f, 22.0f }, dai_vec3{ 0, 0, 0 }, dai_vec3{ 0,1,0 },
                      55.0f, 0.1f, 300.0f);

    dai_render_instance sp = dai_render_instance_default();
    sp.mesh = DAI_MESH_SPHERE; sp.position = { 0, 0, 0 }; sp.scale = { 6, 6, 6 };
    sp.color = { 0.70f, 0.70f, 0.70f };

    dai_render_instance no = sp; no.flags |= DAI_RI_NO_SHADOW;
    dai_render_frame(r, &no, 1);
    Frame R = grab(r);
    dai_render_frame(r, &sp, 1);
    Frame S = grab(r);
    if (save_ppm) save(r, "acne_sphere.ppm");

    Acne a;
    int lit_px = 0;
    for (uint32_t y = 0; y < S.h; ++y)
        for (uint32_t x = 0; x < S.w; ++x) {
            if (R.lum(x, y) < 0.03f) continue;              // background or the dark side
            ++lit_px;
            if (R.lum(x, y) - S.lum(x, y) > 0.02f) ++a.px;
        }
    a.frac = lit_px ? (double)a.px / lit_px : 0.0;
    for (uint32_t y = S.h * 20 / 100; y < S.h * 80 / 100; ++y) {
        int sw = 0; bool dark = false;
        float ref = S.lum(S.w / 4, y);
        for (uint32_t x = S.w / 4; x < S.w * 3 / 4; ++x) {
            float l = S.lum(x, y);
            if (!dark && l < ref * 0.80f) { dark = true; ++sw; }
            else if (dark && l > ref * 0.92f) dark = false;
            ref = ref * 0.9f + l * 0.1f;
        }
        a.swings += sw;
        if (sw > a.worst_line) a.worst_line = sw;
    }
    std::printf("     [acne_sphere] %d of %d lit pixels self shadowed (%.3f%%), %d bands (worst line %d)\n",
                a.px, lit_px, 100.0 * a.frac, a.swings, a.worst_line);
    return a;
}

int main(int argc, char **argv) {
    if (argc > 1) g_outdir = argv[1];
    int save_ppm = std::getenv("DAI_NO_PPM") == nullptr;
    char err[256] = {0};
    dai_render_desc rd{}; rd.width = 800; rd.height = 800;
    dai_renderer *r = dai_render_create(&rd, err, sizeof(err));
    if (!r) { std::printf("renderer unavailable: %s\n", err); return 77; }
    std::printf("shadow contact test on: %s\n", dai_render_device_name(r));
    dai_render_ambient(r, dai_vec3{ 0.35f, 0.40f, 0.5f }, dai_vec3{ 0.2f, 0.2f, 0.2f }, 0.30f);
    dai_render_exposure(r, 1.1f);
    dai_render_fog(r, 0.0f, dai_vec3{ 0, 0, 0 });

    // ---- 1. a crate on the floor, near camera, sun behind it
    Scene near_box{};
    near_box.tag = "near_box"; near_box.mesh = DAI_MESH_BOX;
    near_box.caster_scale = { 1, 1, 1 }; near_box.caster_pos = { 0, 1, 0 };
    near_box.floor_pos = { 0, -1, 0 }; near_box.floor_scale = { 30, 1, 30 };
    near_box.eye = { 0, 3.6f, 6.0f }; near_box.target = { 0, 0.6f, 0.5f };
    near_box.fov = 55.0f; near_box.zfar = 300.0f; near_box.extent = 24.0f;   // the editor's own numbers
    near_box.sun = { 0.0f, 0.80f, -0.60f };
    std::printf("[1] crate standing on the floor, 7 m away\n");
    Gap g1 = measure_gap(r, near_box, save_ppm);

    // ---- 2. the cylinder from the report
    Scene near_cyl = near_box;
    near_cyl.tag = "near_cyl"; near_cyl.mesh = DAI_MESH_CYLINDER;
    near_cyl.caster_scale = { 0.9f, 1.0f, 0.9f };
    std::printf("[2] cylinder standing on the floor, 7 m away\n");
    Gap g2 = measure_gap(r, near_cyl, save_ppm);

    // ---- 3. the same crate 50 m out, in the far cascade, through a long lens
    // so the pixels per metre stay high enough to measure anything.
    Scene far_box{};
    far_box.tag = "far_box"; far_box.mesh = DAI_MESH_BOX;
    far_box.caster_scale = { 1.5f, 1.5f, 1.5f }; far_box.caster_pos = { 0, 1.5f, -40 };
    far_box.floor_pos = { 0, -1, -30 }; far_box.floor_scale = { 80, 1, 80 };
    far_box.eye = { 0, 30.0f, 10.0f }; far_box.target = { 0, 0.0f, -40.0f };
    far_box.fov = 16.0f; far_box.zfar = 300.0f; far_box.extent = 70.0f;
    far_box.sun = { 0.0f, 0.80f, -0.60f };
    std::printf("[3] crate 50 m away, far cascade\n");
    Gap g3 = measure_gap(r, far_box, save_ppm);

    // ---- 3b. a THIN caster. A crate is 3 m thick measured along the light, so
    // a bias of a metre still lands inside it. A plank is 6 cm thick, and every
    // centimetre of bias comes straight off the base of its shadow - which is
    // why the depth bias has to be a length, not an NDC constant that means
    // 7 cm in the near cascade and 1.7 m in the far one.
    Scene plank{};
    plank.tag = "plank_near"; plank.mesh = DAI_MESH_BOX;
    plank.caster_scale = { 1.0f, 1.0f, 0.03f }; plank.caster_pos = { 0, 1.0f, 0 };
    plank.floor_pos = { 0, -1, 0 }; plank.floor_scale = { 30, 1, 30 };
    plank.eye = { 0, 3.6f, 6.0f }; plank.target = { 0, 0.6f, 0.5f };
    plank.fov = 55.0f; plank.zfar = 300.0f; plank.extent = 24.0f;
    plank.sun = { 0.0f, 0.80f, -0.60f };
    std::printf("[3b] a 6 cm plank standing on the floor, 7 m away\n");
    Gap g4 = measure_gap(r, plank, save_ppm);

    Scene plank_far = plank;
    plank_far.tag = "plank_far";
    plank_far.caster_scale = { 1.5f, 1.5f, 0.03f }; plank_far.caster_pos = { 0, 1.5f, -40 };
    plank_far.floor_pos = { 0, -1, -30 }; plank_far.floor_scale = { 80, 1, 80 };
    plank_far.eye = { 0, 30.0f, 10.0f }; plank_far.target = { 0, 0.0f, -40.0f };
    plank_far.fov = 16.0f; plank_far.zfar = 300.0f; plank_far.extent = 70.0f;
    std::printf("[3c] the same plank 50 m away, far cascade\n");
    Gap g5 = measure_gap(r, plank_far, save_ppm);

    // ---- 4. the counter measurement
    std::printf("[4] shadow acne on a floor nothing shadows\n");
    Acne a = measure_acne(r, save_ppm);

    // ---- 5. the same question on a convex object, where every slope happens
    std::printf("[5] shadow acne on a sphere, which cannot shadow itself\n");
    Acne a2 = measure_acne_convex(r, save_ppm);

    // What the numbers have to be. The gap is the bug; the acne is the price
    // that must NOT be paid for fixing it.
    CHECK(g1.cols > 0 && g1.mean <= 1.5, "crate at 7 m: %.2f px of daylight between the object and its shadow", g1.mean);
    CHECK(g2.cols > 0 && g2.mean <= 1.5, "cylinder at 7 m: %.2f px of daylight between the object and its shadow", g2.mean);
    CHECK(g3.cols > 0 && g3.mean <= 4.0, "crate at 50 m: %.2f px of daylight between the object and its shadow", g3.mean);
    CHECK(g1.missing == 0 && g2.missing == 0 && g3.missing == 0,
          "%d/%d/%d columns have no shadow under the object at all", g1.missing, g2.missing, g3.missing);
    CHECK(g4.cols > 0 && g4.mean <= 1.5, "plank at 7 m: %.2f px between the plank and its shadow", g4.mean);
    // A 6 cm plank 50 m out is 0.4 of a shadow TEXEL thick (one texel of the
    // far cascade covers 15 cm there, which is 3.6 px in this shot), so its
    // contact cannot resolve better than about a texel and a half. 6 px is
    // that limit, not a concession: it was 31 px before.
    CHECK(g5.cols > 0 && g5.mean <= 6.0, "plank at 50 m: %.2f px between the plank and its shadow", g5.mean);
    CHECK(a.frac <= 0.005, "%.3f%% of a floor that nothing shadows is in shadow - acne", 100.0 * a.frac);
    CHECK(a.worst_line <= 1, "%d dark bands on one scanline of an unshadowed floor - acne", a.worst_line);
    CHECK(a2.frac <= 0.005, "%.3f%% of a sphere that nothing can shadow is in shadow - acne", 100.0 * a2.frac);
    // No band check on the sphere: a curved surface darkens towards its own
    // terminator, which trips a band counter without any acne being there. The
    // difference against the unshadowed render is the honest measure, and it
    // reads 74% when the normal offset is taken away.

    dai_render_destroy(r);
    std::printf("\n%d checks passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
