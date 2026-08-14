// TEMPORARY diagnostic (round 4) - not part of the build.
// Rebuilds EXACTLY the fixture of tests/droneshow_cases_plan.cpp
// case_sampled_show_is_clean and prints every distance conflict with the
// formation/transition it falls into.
#include "dai_show.h"
#include "../tests/droneshow_cases.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>


#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
static std::vector<float>    g_pos[3], g_nrm[3], g_uv[3];
static std::vector<uint32_t> g_idx[3];
static int                   g_built[3] = { 0, 0, 0 };

static void push_tri(int k, const float *a, const float *b, const float *c) {
    const float *v[3] = { a, b, c };
    float e1[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] };
    float e2[3] = { c[0] - a[0], c[1] - a[1], c[2] - a[2] };
    float n[3]  = { e1[1] * e2[2] - e1[2] * e2[1],
                    e1[2] * e2[0] - e1[0] * e2[2],
                    e1[0] * e2[1] - e1[1] * e2[0] };
    float len = n[0] * n[0] + n[1] * n[1] + n[2] * n[2];
    if (len > 0.0f) {
        len = 1.0f / std::sqrt(len);
        n[0] *= len; n[1] *= len; n[2] *= len;
    }
    for (int i = 0; i < 3; ++i) {
        g_idx[k].push_back((uint32_t)(g_pos[k].size() / 3));
        g_pos[k].push_back(v[i][0]); g_pos[k].push_back(v[i][1]); g_pos[k].push_back(v[i][2]);
        g_nrm[k].push_back(n[0]);    g_nrm[k].push_back(n[1]);    g_nrm[k].push_back(n[2]);
        g_uv[k].push_back(v[i][0] + 0.5f);
        g_uv[k].push_back(v[i][2] + 0.5f);
    }
}

static void build_box(int k, float h) {
    const float s[8][3] = {
        { -h, -h, -h }, {  h, -h, -h }, {  h,  h, -h }, { -h,  h, -h },
        { -h, -h,  h }, {  h, -h,  h }, {  h,  h,  h }, { -h,  h,  h }
    };
    const int f[12][3] = {
        {0,2,1},{0,3,2}, {4,5,6},{4,6,7}, {0,1,5},{0,5,4},
        {3,7,6},{3,6,2}, {0,4,7},{0,7,3}, {1,2,6},{1,6,5}
    };
    for (int i = 0; i < 12; ++i) push_tri(k, s[f[i][0]], s[f[i][1]], s[f[i][2]]);
}

static void build_quad(int k, float h) {
    const float a[3] = { -h, 0.0f, -h }, b[3] = { h, 0.0f, -h };
    const float c[3] = {  h, 0.0f,  h }, d[3] = { -h, 0.0f,  h };
    push_tri(k, a, c, b);
    push_tri(k, a, d, c);
}

static void build_sphere(int k, float r, int rings, int segs) {
    for (int i = 0; i < rings; ++i) {
        float p0 = (float)M_PI * (float)i / (float)rings;
        float p1 = (float)M_PI * (float)(i + 1) / (float)rings;
        for (int j = 0; j < segs; ++j) {
            float t0 = 2.0f * (float)M_PI * (float)j / (float)segs;
            float t1 = 2.0f * (float)M_PI * (float)(j + 1) / (float)segs;
            float a[3] = { r * std::sin(p0) * std::cos(t0), r * std::cos(p0), r * std::sin(p0) * std::sin(t0) };
            float b[3] = { r * std::sin(p1) * std::cos(t0), r * std::cos(p1), r * std::sin(p1) * std::sin(t0) };
            float c[3] = { r * std::sin(p1) * std::cos(t1), r * std::cos(p1), r * std::sin(p1) * std::sin(t1) };
            float d[3] = { r * std::sin(p0) * std::cos(t1), r * std::cos(p0), r * std::sin(p0) * std::sin(t1) };
            push_tri(k, a, b, c);
            push_tri(k, a, c, d);
        }
    }
}

dai_show_test_mesh show_test_mesh(int kind) {
    int k = (kind >= 0 && kind < 3) ? kind : 0;
    if (!g_built[k]) {
        if (k == 0) build_box(k, 0.5f);
        else if (k == 1) build_quad(k, 5.0f);
        else build_sphere(k, 1.0f, 24, 32);
        g_built[k] = 1;
    }
    dai_show_test_mesh m;
    m.positions    = g_pos[k].data();
    m.normals      = g_nrm[k].data();
    m.uvs          = g_uv[k].data();
    m.vertex_count = (uint32_t)(g_pos[k].size() / 3);
    m.indices      = g_idx[k].data();
    m.index_count  = (uint32_t)g_idx[k].size();
    return m;
}


static void ring_soup(std::vector<float> &pos, std::vector<uint32_t> &idx, int segs, float thick) {
    auto tri = [&](const float *a, const float *b, const float *c) {
        const float *v[3] = { a, b, c };
        for (int i = 0; i < 3; ++i) {
            idx.push_back((uint32_t)(pos.size() / 3));
            pos.push_back(v[i][0]); pos.push_back(v[i][1]); pos.push_back(v[i][2]);
        }
    };
    for (int j = 0; j < segs; ++j) {
        float t0 = 6.28318530718f * (float)j / (float)segs;
        float t1 = 6.28318530718f * (float)(j + 1) / (float)segs;
        for (int k = 0; k < 12; ++k) {
            float u0 = 6.28318530718f * (float)k / 12.0f;
            float u1 = 6.28318530718f * (float)(k + 1) / 12.0f;
            auto at = [&](float t, float u, float *o) {
                float r = 1.0f + thick * std::cos(u);
                o[0] = r * std::cos(t);
                o[1] = r * std::sin(t);
                o[2] = thick * std::sin(u);
            };
            float a[3], b[3], c[3], d[3];
            at(t0, u0, a); at(t1, u0, b); at(t1, u1, c); at(t0, u1, d);
            tri(a, b, c); tri(a, c, d);
        }
    }
}

int main(void) {
    dai_show_settings s = dai_show_settings_default();
    s.min_distance_m = 2.0f;
    s.v_max_ms       = 8.0f;
    s.a_max_ms2      = 4.0f;
    s.seed           = 0x0F1E2D3C4B5A6978ull;
    s.drone_count    = 420;
    s.fps            = 10;
    s.takeoff_alt_m  = 30.0f;
    s.seed           = 20260814ull;

    dai_show *sh = dai_show_create(&s);
    struct Fig { int kind; const char *name; float size; int mode; };
    const Fig figs[3] = {
        { 2, "Sphere", 70.0f, DAI_SHOW_SAMPLE_SURFACE    },
        { 3, "Ring",   90.0f, DAI_SHOW_SAMPLE_SILHOUETTE },
        { 0, "Cube",   70.0f, DAI_SHOW_SAMPLE_SURFACE    }
    };
    std::vector<float>    ring_pos;
    std::vector<uint32_t> ring_idx;
    ring_soup(ring_pos, ring_idx, 48, 0.28f);

    for (int i = 0; i < 3; ++i) {
        dai_show_sample_desc d;
        std::memset(&d, 0, sizeof(d));
        if (figs[i].kind == 3) {
            d.positions    = ring_pos.data();
            d.vertex_count = (uint32_t)(ring_pos.size() / 3);
            d.indices      = ring_idx.data();
            d.index_count  = (uint32_t)ring_idx.size();
        } else {
            dai_show_test_mesh m = show_test_mesh(figs[i].kind);
            d.positions = m.positions; d.normals = m.normals; d.uvs = m.uvs;
            d.vertex_count = m.vertex_count;
            d.indices = m.indices; d.index_count = m.index_count;
        }
        d.base_rgba        = (i == 0) ? 0xFF40C0FFu : (i == 1) ? 0xFF60FF90u : 0xFFFF80D0u;
        d.mode             = figs[i].mode;
        d.count            = s.drone_count;
        d.min_distance_m   = s.min_distance_m;
        d.scale            = figs[i].size * 0.5f / ((figs[i].kind == 0) ? 0.5f : 1.0f);
        d.centre           = dai_vec3{ 0.0f, s.takeoff_alt_m + figs[i].size * 0.6f, 0.0f };
        d.view_dir         = dai_vec3{ 0.0f, 0.0f, 1.0f };
        d.relax_iterations = 6;
        d.seed             = s.seed + (uint64_t)i * 7919ull;
        char err[256] = { 0 };
        uint32_t idx = dai_show_formation_from_mesh(sh, figs[i].name, "test://figure",
                                                    &d, err, sizeof(err));
        if (idx == 0xFFFFFFFFu) { std::printf("sample failed %s\n", err); return 1; }
        dai_show_formation_set_hold(sh, idx, 4.0f);
    }
    {
        const dai_show_point *base = dai_show_formation_points(sh, 0);
        std::vector<dai_show_point> pts(base, base + s.drone_count);
        pts[173] = pts[172];
        pts[173].x += 0.4f;
        dai_show_formation_add(sh, "Sphere (near miss)", "test://figure",
                               pts.data(), (uint32_t)pts.size(), 5.0f);
    }

    dai_show_solve(sh, nullptr, 0);
    dai_show_validate_show(sh);

    // Where does each formation start / end on the timeline?
    std::printf("formations: %u\n", dai_show_formation_count(sh));
    for (uint32_t f = 0; f < dai_show_formation_count(sh); ++f) {
        dai_show_formation_info fi;
        if (dai_show_formation_get(sh, f, &fi))
            std::printf("  [%u] %-20s t_start %8.2f hold %.2f pts %u\n", f, fi.name,
                        fi.t_start, fi.hold_s, fi.point_count);
    }
    uint32_t cn = dai_show_conflict_count(sh);
    std::printf("conflicts: %u\n", cn);
    for (uint32_t i = 0; i < cn && i < 40u; ++i) {
        dai_show_conflict k;
        if (!dai_show_conflict_at(sh, i, &k)) continue;
        std::printf("  %2u kind %d  %u+%u  t %7.2f  value %.6f limit %.6f  deficit %.6f\n",
                    i, k.kind, k.a, k.b, k.time_s, k.value, k.limit, k.limit - k.value);
    }
    dai_show_timings tm = dai_show_get_timings(sh);
    std::printf("last_layer: cross %u height %u delay %u unresolved %u layers %u\n",
                tm.last_layer.crossings_found, tm.last_layer.resolved_by_height,
                tm.last_layer.resolved_by_delay, tm.last_layer.unresolved,
                tm.last_layer.layers_used);
    for (uint32_t f = 1; f < dai_show_formation_count(sh); ++f) {
        dai_show_layer_stats ls; dai_show_assign_stats as;
        if (!dai_show_transition_stats(sh, f, &as, &ls)) continue;
        std::printf("  transition %u: cross %u height %u delay %u unresolved %u layers %u maxextra %.2f\n",
                    f, ls.crossings_found, ls.resolved_by_height, ls.resolved_by_delay,
                    ls.unresolved, ls.layers_used, ls.max_extra_height_m);
    }
    dai_show_destroy(sh);
    return 0;
}
