// The drone show pipeline, end to end.
//
//   ./build/test_droneshow            the assertions, plus the scaling table
//   ./build/test_droneshow quick      skip the 10,000 drone row
//
// Two things happen here that do not happen in the four case files: the
// DETERMINISM proof and the SCALING measurement.
//
// Determinism, because it is the one claim this tool sells. A show that is
// signed off as collision free has to come out identical the second time,
// including on the paths that run on several threads - a reduction that adds
// its partial sums in thread arrival order is not deterministic, it is merely
// usually the same. The test runs the whole pipeline twice into two buffers
// and compares them with memcmp. Not "within epsilon": memcmp.
//
// Scaling, because "it handles 10,000 drones" is a claim and a table of
// milliseconds is a fact. The row for 10,000 is the one that would expose an
// O(n^2) validation tick or an O(n^3) assignment - both would take longer than
// anyone would wait, which is exactly why they are printed rather than
// asserted away.

#include "droneshow_cases.hpp"

#include <chrono>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

int g_show_pass = 0;
int g_show_fail = 0;

void show_section(const char *name) {
    std::printf("\n%s\n", name);
}

// ---- shared fixtures ------------------------------------------------------

// A box, a quad and a sphere, generated once into static storage. Generated
// rather than loaded: a test that needs a file on disk is a test that fails on
// someone else's machine for a reason that has nothing to do with the code.
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

void show_grid_formation(dai_show_point *out, uint32_t n, float spacing, dai_vec3 centre) {
    uint32_t side = 1;
    while (side * side < n) ++side;
    for (uint32_t i = 0; i < n; ++i) {
        uint32_t gx = i % side, gz = i / side;
        out[i].x = centre.x + ((float)gx - (float)(side - 1) * 0.5f) * spacing;
        out[i].y = centre.y;
        out[i].z = centre.z + ((float)gz - (float)(side - 1) * 0.5f) * spacing;
        out[i].r = (uint8_t)(gx * 255u / (side ? side : 1));
        out[i].g = (uint8_t)(gz * 255u / (side ? side : 1));
        out[i].b = 200;
        out[i].w = 0;
    }
}

// ---- the two things only this file can do ---------------------------------

static double now_ms() {
    using clock = std::chrono::steady_clock;
    return std::chrono::duration<double, std::milli>(clock::now().time_since_epoch()).count();
}

// Builds a whole show from scratch and hands back every byte a caller could
// observe: the plan's keyframes and the conflict list. Two calls must produce
// two identical buffers, or the tool cannot be trusted with a safety claim.
static void run_pipeline(uint32_t n, std::string *bytes_out, dai_show_timings *t_out,
                         size_t *plan_bytes_out = nullptr) {
    dai_show_settings s = dai_show_settings_default();
    s.drone_count    = n;
    s.min_distance_m = 2.0f;
    s.seed           = 0x5ED1CE5ull;

    dai_show *sh = dai_show_create(&s);
    if (!sh) return;

    dai_show_test_mesh mesh = show_test_mesh(2);
    for (int f = 0; f < 3; ++f) {
        dai_show_sample_desc d;
        std::memset(&d, 0, sizeof(d));
        d.positions    = mesh.positions;
        d.normals      = mesh.normals;
        d.uvs          = mesh.uvs;
        d.vertex_count = mesh.vertex_count;
        d.indices      = mesh.indices;
        d.index_count  = mesh.index_count;
        d.base_rgba    = 0xFF3080FFu;
        d.mode         = (f == 1) ? DAI_SHOW_SAMPLE_VOLUME : DAI_SHOW_SAMPLE_SURFACE;
        d.count        = n;
        d.min_distance_m   = s.min_distance_m;
        // Big enough that N points at 2 m are geometrically possible; the
        // feasibility test in droneshow_cases_sample.cpp is where "too small"
        // is checked on purpose.
        d.scale        = 2.0f * std::sqrt((float)n);
        d.centre       = dai_vec3{ 0.0f, 60.0f + 20.0f * (float)f, 0.0f };
        d.view_dir     = dai_vec3{ 0.0f, 0.0f, 1.0f };
        d.relax_iterations = 4;
        d.seed         = s.seed + (uint64_t)f;

        char err[256] = { 0 };
        char name[32];
        std::snprintf(name, sizeof(name), "figure %d", f);
        dai_show_formation_from_mesh(sh, name, "test://sphere", &d, err, sizeof(err));
    }

    dai_show_solve(sh, nullptr, 0);
    dai_show_validate_show(sh);

    if (t_out) *t_out = dai_show_get_timings(sh);
    if (plan_bytes_out) *plan_bytes_out = dai_show_plan_bytes(dai_show_get_plan(sh));

    if (bytes_out) {
        bytes_out->clear();
        const dai_show_plan *p = dai_show_get_plan(sh);
        uint32_t drones = dai_show_plan_drone_count(p);
        for (uint32_t i = 0; i < drones; ++i) {
            uint32_t kc = dai_show_plan_keyframe_count(p, i);
            for (uint32_t k = 0; k < kc; ++k) {
                dai_show_key key;
                if (!dai_show_plan_key_at(p, i, k, &key)) continue;
                bytes_out->append((const char *)&key, sizeof(key));
            }
        }
        uint32_t cn = dai_show_conflict_count(sh);
        for (uint32_t i = 0; i < cn; ++i) {
            dai_show_conflict c;
            if (!dai_show_conflict_at(sh, i, &c)) continue;
            bytes_out->append((const char *)&c, sizeof(c));
        }
    }
    dai_show_destroy(sh);
}

static void determinism(void) {
    show_section("determinism - the same input, twice, byte for byte");

    std::string a, b;
    run_pipeline(256, &a, nullptr);
    run_pipeline(256, &b, nullptr);

    CHECK(!a.empty(), "the pipeline produced nothing to compare");
    CHECK(a.size() == b.size(), "two runs produced %zu and %zu bytes", a.size(), b.size());
    CHECK(a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size()) == 0,
          "two runs of the same show are not bit identical");

    // And the same for the parallel path on its own: the clustered assignment
    // splits the work across threads, and a fixed reduction order is the only
    // thing that makes it reproducible.
    std::vector<dai_show_point> from(4096), to(4096);
    show_grid_formation(from.data(), 4096, 3.0f, dai_vec3{ 0, 50, 0 });
    show_grid_formation(to.data(),   4096, 3.0f, dai_vec3{ 40, 80, 10 });
    std::vector<uint32_t> p1(4096), p2(4096);
    dai_show_assign_stats s1{}, s2{};
    dai_show_assign(from.data(), to.data(), 4096, DAI_SHOW_ASSIGN_CLUSTER, p1.data(), &s1);
    dai_show_assign(from.data(), to.data(), 4096, DAI_SHOW_ASSIGN_CLUSTER, p2.data(), &s2);
    CHECK(std::memcmp(p1.data(), p2.data(), p1.size() * sizeof(uint32_t)) == 0,
          "the parallel assignment is not reproducible");
    CHECK(std::memcmp(&s1.total_cost_m, &s2.total_cost_m, sizeof(double)) == 0,
          "the parallel cost sum depends on thread order (%.9f vs %.9f)",
          s1.total_cost_m, s2.total_cost_m);
}

// The timings a panel prints are the WHOLE show, and this is what "whole"
// means: counts summed over every transition, the gap at its worst case. The
// bug this stands against shipped once - the document kept only the last
// transition's numbers, so a storyboard with twenty moves printed the crossing
// count of move twenty next to the conflict count of all twenty.
static void aggregation(void) {
    show_section("timings - the show total is the sum of its transitions");

    const uint32_t n = 64;
    dai_show_settings s = dai_show_settings_default();
    s.drone_count    = n;
    s.min_distance_m = 2.0f;
    s.seed           = 0xA66E6ull;

    dai_show *sh = dai_show_create(&s);
    CHECK(sh != nullptr, "the document could not be created");
    if (!sh) return;

    // Four grids, offset against each other so the assignment has real work to
    // do and the separator has real crossings to find.
    std::vector<dai_show_point> pts(n);
    for (int f = 0; f < 4; ++f) {
        float d = (float)f;
        show_grid_formation(pts.data(), n, 4.0f,
                            dai_vec3{ 6.0f * d, 40.0f + 5.0f * d, ((f & 1) ? 8.0f : -8.0f) });
        char name[32];
        std::snprintf(name, sizeof(name), "grid %d", f);
        CHECK(dai_show_formation_add(sh, name, "test://grid", pts.data(), n, 2.0f) != UINT32_MAX,
              "formation %d was refused", f);
    }
    dai_show_solve(sh, nullptr, 0);
    dai_show_timings t = dai_show_get_timings(sh);

    uint32_t crossings = 0, lifted = 0, delayed = 0, unresolved = 0;
    double   cost = 0.0;
    float    gap = -1.0f;
    int      moves = 0;
    for (uint32_t i = 1; i < 4; ++i) {
        dai_show_assign_stats a;
        dai_show_layer_stats  l;
        std::memset(&a, 0, sizeof(a));
        std::memset(&l, 0, sizeof(l));
        CHECK(dai_show_transition_stats(sh, i, &a, &l) == 1,
              "transition %u kept no stats of its own", i);
        crossings  += l.crossings_found;
        lifted     += l.resolved_by_height;
        delayed    += l.resolved_by_delay;
        unresolved += l.unresolved;
        cost       += a.total_cost_m;
        if (a.gap_percent > gap) gap = a.gap_percent;
        ++moves;
    }
    CHECK(moves == 3, "three transitions were solved, %d reported stats", moves);
    CHECK(t.last_layer.crossings_found == crossings,
          "the show total says %u crossings, the transitions add up to %u",
          t.last_layer.crossings_found, crossings);
    CHECK(t.last_layer.resolved_by_height == lifted,
          "the show total says %u lifted, the transitions add up to %u",
          t.last_layer.resolved_by_height, lifted);
    CHECK(t.last_layer.resolved_by_delay == delayed,
          "the show total says %u delayed, the transitions add up to %u",
          t.last_layer.resolved_by_delay, delayed);
    CHECK(t.last_layer.unresolved == unresolved,
          "the show total says %u unresolved, the transitions add up to %u",
          t.last_layer.unresolved, unresolved);
    CHECK(std::fabs(t.last_assign.total_cost_m - cost) < 1e-6 * (1.0 + cost),
          "the show total flew %.3f m, the transitions add up to %.3f m",
          t.last_assign.total_cost_m, cost);
    CHECK(t.last_assign.gap_percent == gap,
          "the show total reports a %.4f%% gap, the worst transition %.4f%%",
          (double)t.last_assign.gap_percent, (double)gap);
    // And the breakdown goes away with the plan it belongs to: a per transition
    // row that outlives the solve is the stale badge this document forbids.
    dai_show_formation_remove(sh, 3);
    dai_show_assign_stats a2;
    dai_show_layer_stats  l2;
    CHECK(dai_show_transition_stats(sh, 1, &a2, &l2) == 0,
          "the transition stats survived the edit that invalidated the plan");
    dai_show_destroy(sh);
}

static void scaling(int with_10k) {
    show_section("scaling - measured, not claimed (milliseconds)");
    std::printf("  %8s %10s %10s %10s %10s %10s %10s %10s %8s\n",
                "drones", "sample", "assign", "layer", "profile", "validate",
                "plan MB", "check MB", "ticks");

    const uint32_t sizes[3] = { 100, 1000, 10000 };
    double wall_of[3]  = { 0.0, 0.0, 0.0 };
    double ticks_of[3] = { 1.0, 1.0, 1.0 };
    for (int i = 0; i < (with_10k ? 3 : 2); ++i) {
        dai_show_timings t{};
        size_t plan_bytes = 0;
        double t0 = now_ms();
        run_pipeline(sizes[i], nullptr, &t, &plan_bytes);
        double wall = now_ms() - t0;
        wall_of[i] = wall;
        // Two memory columns, because they answer two questions: "plan MB" is
        // what the show COSTS to keep (dai_show_plan_bytes - keyframes, the
        // number this design exists to make small) and "check MB" is what the
        // validator borrowed while streaming over it. Materialised ticks would
        // put gigabytes in the first column and there would be no argument.
        // The tick column is there because the fixture's figures grow with the
        // fleet: 10,000 drones stand 200 m apart, v_max makes the moves between
        // them longer, and the show that has to be validated is several times
        // the length of the 1,000 drone one. Without that number in the table,
        // the wall clock column looks like an algorithmic blow-up when it is
        // mostly a longer show - and the bound below would be measuring the
        // fixture instead of the code.
        ticks_of[i] = (t.last_validate.ticks_checked > 0)
                          ? (double)t.last_validate.ticks_checked : 1.0;
        std::printf("  %8u %10.2f %10.2f %10.2f %10.2f %10.2f %10.2f %10.2f %8.0f   (%.0f ms wall)\n",
                    sizes[i], t.sample_ms, t.assign_ms, t.layer_ms, t.profile_ms,
                    t.validate_ms,
                    (double)plan_bytes / (1024.0 * 1024.0),
                    (double)t.last_validate.peak_bytes / (1024.0 * 1024.0),
                    ticks_of[i], wall);

        CHECK((double)plan_bytes < 64.0 * 1024.0 * 1024.0,
              "%u drones: the plan is %.1f MB - that is materialised ticks, not keyframes",
              sizes[i], (double)plan_bytes / (1024.0 * 1024.0));

        // The broadphase has to actually be doing something. n^2/2 pairs per
        // tick at 10,000 drones would be 5*10^7 per tick; anything near that
        // means the grid was not used and the table above is a lie.
        if (t.last_validate.ticks_checked > 0) {
            double per_tick = (double)t.last_validate.pairs_tested /
                              (double)t.last_validate.ticks_checked;
            double naive    = 0.5 * (double)sizes[i] * (double)(sizes[i] - 1);
            CHECK(per_tick < naive * 0.25,
                  "%u drones: %.0f pairs per tick against a naive %.0f - the broadphase is not being used",
                  sizes[i], per_tick, naive);
        }
    }

    // And a hard bound on the ten thousand row, so an O(n^2) tick or an O(n^3)
    // assignment that finds its way back in turns the BUILD red instead of
    // merely printing an ugly number nobody reads. The test is the growth, not
    // the absolute time: ten times the drones cost about eleven times the
    // seconds here, a quadratic path would cost a hundred and a cubic one a
    // thousand.
    //
    // The bound was sixty on the raw wall clock, which is a long way from
    // anything measured and therefore a long way from catching anything: a
    // stage that had gone quadratic could hide under it. Two changes make it
    // bite. The time is divided by the number of ticks the show actually has,
    // so the ten thousand row is compared at the same amount of work rather
    // than at ten times the drones AND nine times the length; and the bound is
    // fifteen, against a measured 4.3 - (9326/2437) / (842/946) at commit
    // 6ea748e on the machine and date named in RUN.md, which is where that
    // table comes from, so the next reader can tell a regression from a slower
    // laptop. The same run under no load has come out at 3.8; three builds
    // sharing eight cores is what the spread between those two numbers is, and
    // it is why the bound is three times the measurement rather than a whisker
    // above it. A quadratic tick would put ten in that ratio all by itself and a
    // cubic assignment a hundred. The absolute ceiling of thirty seconds is the
    // second half of the same claim: a ratio is meaningless if both rows are
    // already too slow to sell.
    if (with_10k && wall_of[1] > 0.0) {
        double growth = (wall_of[2] / ticks_of[2]) / (wall_of[1] / ticks_of[1]);
        CHECK(growth < 15.0,
              "10,000 drones cost %.1f x the 1,000 drone time per tick "
              "(%.0f ms / %.0f ticks vs %.0f ms / %.0f ticks) - that is the shape "
              "of an O(n^2) or O(n^3) path, not of the broadphase",
              growth, wall_of[2], ticks_of[2], wall_of[1], ticks_of[1]);
        CHECK(wall_of[2] < 30000.0,
              "the 10,000 drone show took %.0f ms end to end - too slow to be the "
              "scaling this feature is sold on", wall_of[2]);
    }
}

int main(int argc, char **argv) {
    int quick = (argc > 1 && std::strcmp(argv[1], "quick") == 0);

    std::printf("drone show pipeline\n");

    show_cases_sample();
    show_cases_assign();
    show_cases_plan();
    show_cases_io();
    show_cases_edit();
    determinism();
    aggregation();
    scaling(!quick);

    std::printf("\n%s: %d checks, %d failures\n",
                g_show_fail ? "FAILED" : "ok", g_show_pass + g_show_fail, g_show_fail);
    return g_show_fail ? 1 : 0;
}
