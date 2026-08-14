// TEMPORARY development harness (module fix-r3-4) - not part of the build.
// Rebuilds the demo show of tools/droneshow_shot.cpp without a renderer, so
// the remaining conflicts can be looked at in a second rather than a minute.
#include "dai_show.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

struct Soup {
    std::vector<float>    pos;
    std::vector<uint32_t> idx;
    void tri(const float *a, const float *b, const float *c) {
        const float *v[3] = { a, b, c };
        for (int i = 0; i < 3; ++i) {
            idx.push_back((uint32_t)(pos.size() / 3));
            pos.push_back(v[i][0]); pos.push_back(v[i][1]); pos.push_back(v[i][2]);
        }
    }
};

static void figure_sphere(Soup &s, int rings, int segs) {
    for (int i = 0; i < rings; ++i) {
        float p0 = (float)M_PI * (float)i / (float)rings;
        float p1 = (float)M_PI * (float)(i + 1) / (float)rings;
        for (int j = 0; j < segs; ++j) {
            float t0 = 2.0f * (float)M_PI * (float)j / (float)segs;
            float t1 = 2.0f * (float)M_PI * (float)(j + 1) / (float)segs;
            float a[3] = { std::sin(p0) * std::cos(t0), std::cos(p0), std::sin(p0) * std::sin(t0) };
            float b[3] = { std::sin(p1) * std::cos(t0), std::cos(p1), std::sin(p1) * std::sin(t0) };
            float c[3] = { std::sin(p1) * std::cos(t1), std::cos(p1), std::sin(p1) * std::sin(t1) };
            float d[3] = { std::sin(p0) * std::cos(t1), std::cos(p0), std::sin(p0) * std::sin(t1) };
            s.tri(a, b, c); s.tri(a, c, d);
        }
    }
}

static void figure_cube(Soup &s) {
    const float h = 1.0f;
    const float v[8][3] = {
        { -h, -h, -h }, {  h, -h, -h }, {  h,  h, -h }, { -h,  h, -h },
        { -h, -h,  h }, {  h, -h,  h }, {  h,  h,  h }, { -h,  h,  h }
    };
    const int f[12][3] = {
        {0,2,1},{0,3,2},{4,5,6},{4,6,7},{0,1,5},{0,5,4},
        {3,7,6},{3,6,2},{0,4,7},{0,7,3},{1,2,6},{1,6,5}
    };
    for (int i = 0; i < 12; ++i) s.tri(v[f[i][0]], v[f[i][1]], v[f[i][2]]);
}

static void figure_ring(Soup &s, int segs, float thick) {
    for (int j = 0; j < segs; ++j) {
        float t0 = 2.0f * (float)M_PI * (float)j / (float)segs;
        float t1 = 2.0f * (float)M_PI * (float)(j + 1) / (float)segs;
        for (int k = 0; k < 12; ++k) {
            float u0 = 2.0f * (float)M_PI * (float)k / 12.0f;
            float u1 = 2.0f * (float)M_PI * (float)(k + 1) / 12.0f;
            auto pt = [&](float t, float u, float *o) {
                float rr = 1.0f + thick * std::cos(u);
                o[0] = rr * std::cos(t); o[1] = thick * std::sin(u); o[2] = rr * std::sin(t);
            };
            float a[3], b[3], c[3], d[3];
            pt(t0, u0, a); pt(t1, u0, b); pt(t1, u1, c); pt(t0, u1, d);
            s.tri(a, b, c); s.tri(a, c, d);
        }
    }
}

int main(void) {
    dai_show_settings s = dai_show_settings_default();
    s.min_distance_m = 2.0f;
    s.v_max_ms       = 8.0f;
    s.a_max_ms2      = 4.0f;
    s.drone_count    = 420;
    s.takeoff_alt_m  = 30.0f;
    s.fps            = 10;
    s.seed           = 20260814ull;

    dai_show *sh = dai_show_create(&s);
    struct Fig { int kind; const char *name; float size; int mode; float hold; };
    const Fig figs[3] = {
        { 0, "Sphere", 70.0f, DAI_SHOW_SAMPLE_SURFACE,    4.0f },
        { 2, "Ring",   90.0f, DAI_SHOW_SAMPLE_SILHOUETTE, 4.0f },
        { 1, "Cube",   70.0f, DAI_SHOW_SAMPLE_SURFACE,    4.0f }
    };
    for (int i = 0; i < 3; ++i) {
        Soup soup;
        if (figs[i].kind == 0)      figure_sphere(soup, 24, 32);
        else if (figs[i].kind == 1) figure_cube(soup);
        else                        figure_ring(soup, 48, 0.28f);
        dai_show_sample_desc d;
        std::memset(&d, 0, sizeof(d));
        d.positions        = soup.pos.data();
        d.vertex_count     = (uint32_t)(soup.pos.size() / 3);
        d.indices          = soup.idx.data();
        d.index_count      = (uint32_t)soup.idx.size();
        d.base_rgba        = (i == 0) ? 0xFF40C0FFu : (i == 1) ? 0xFF60FF90u : 0xFFFF80D0u;
        d.mode             = figs[i].mode;
        d.count            = s.drone_count;
        d.min_distance_m   = s.min_distance_m;
        d.scale            = figs[i].size * 0.5f;
        d.centre           = dai_vec3{ 0.0f, s.takeoff_alt_m + figs[i].size * 0.6f, 0.0f };
        d.view_dir         = dai_vec3{ 0.0f, 0.0f, 1.0f };
        d.relax_iterations = 6;
        d.seed             = s.seed + (uint64_t)i * 7919ull;
        char err[256] = { 0 };
        uint32_t idx = dai_show_formation_from_mesh(sh, figs[i].name, "builtin://figure",
                                                    &d, err, sizeof(err));
        if (idx == 0xFFFFFFFFu) { std::printf("sample %s failed: %s\n", figs[i].name, err); return 1; }
        dai_show_formation_set_hold(sh, idx, figs[i].hold);
    }
    {
        const dai_show_point *base = dai_show_formation_points(sh, 0);
        std::vector<dai_show_point> pts(base, base + s.drone_count);
        pts[173] = pts[172];
        pts[173].x += 0.4f;
        dai_show_formation_add(sh, "Sphere (near miss)", "builtin://figure",
                               pts.data(), (uint32_t)pts.size(), 5.0f);
    }
    char err[256] = { 0 };
    dai_result sr = dai_show_solve(sh, err, sizeof(err));
    dai_show_validate_show(sh);
    dai_show_timings tm = dai_show_get_timings(sh);
    std::printf("solve %s  conflicts %u  layer: cross %u height %u delay %u unresolved %u\n",
                sr == DAI_OK ? "ok" : err, dai_show_conflict_count(sh),
                tm.last_layer.crossings_found, tm.last_layer.resolved_by_height,
                tm.last_layer.resolved_by_delay, tm.last_layer.unresolved);
    for (uint32_t i = 0; i < dai_show_conflict_count(sh) && i < 20u; ++i) {
        dai_show_conflict cf;
        if (!dai_show_conflict_at(sh, i, &cf)) continue;
        std::printf("  conflict %2u  t %7.2fs  drones %u+%u  kind %d  %.4f of %.3f m\n",
                    i, cf.time_s, cf.a, cf.b, cf.kind, cf.value, cf.limit);
    }
    for (uint32_t f = 1; f < dai_show_formation_count(sh); ++f) {
        dai_show_layer_stats ls; dai_show_assign_stats as;
        if (!dai_show_transition_stats(sh, f, &as, &ls)) continue;
        std::printf("  transition %u: cross %u height %u delay %u unresolved %u layers %u\n",
                    f, ls.crossings_found, ls.resolved_by_height, ls.resolved_by_delay,
                    ls.unresolved, ls.layers_used);
    }
    dai_show_destroy(sh);
    return 0;
}
