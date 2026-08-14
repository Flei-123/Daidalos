// Stages 5 and 6 under test: a planted collision, and a lossless round trip.
//
// Implements show_cases_io() - see tests/droneshow_cases.hpp.
//
//   [5a] THE PLANTED COLLISION: build a plan with dai_show_plan_from_keys in
//        which two drones are put 0.5 m apart at a known time. The validator
//        must find it, at that time, naming those two drones, with the right
//        shortfall in metres. A validator that finds nothing is the failure
//        mode this whole panel exists to prevent.
//   [5b] a clean plan yields zero conflicts - the other half of [5a], because
//        a validator that reports everything is equally useless.
//   [5c] planted v_max, a_max, geofence and ground violations are each found
//        and reported with the right kind.
//   [5d] the conflict list is sorted by (time, a, b) and identical across two
//        runs of the same input.
//   [5e] the broadphase is used: pairs_tested per tick is far below n^2/2 on a
//        sparse formation, and the answer is the same as an O(n^2) reference
//        check computed here on a small instance.
//   [6a] JSON export -> import is LOSSLESS: same drone count, same keyframe
//        count per drone, same times, positions and colours, bit for bit.
//   [6b] .skyc export -> import is lossless in the same sense, and the file it
//        writes is a readable ZIP whose show.json parses.
//   [6c] CSV has one line per drone per frame, the header names the columns
//        the header file promises, and the values match dai_show_plan_sample.
//   [6d] an interrupted export leaves no file behind (write to temp, rename).
#include "droneshow_cases.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

// A plan built by hand, because the point of stage 5 is to find what a solver
// would never have produced. `gap` is how close the two drones come at t = 1.
dai_show_plan *two_drone_plan(float gap) {
    uint32_t counts[2] = { 3, 3 };
    dai_show_key k[6];
    std::memset(k, 0, sizeof(k));
    const float far = 20.0f;
    for (int d = 0; d < 2; ++d) {
        float side = (d == 0) ? -1.0f : 1.0f;
        for (int i = 0; i < 3; ++i) {
            dai_show_key &key = k[d * 3 + i];
            key.t   = (float)i;
            key.p.x = side * ((i == 1) ? gap * 0.5f : far);
            key.p.y = 50.0f;
            key.p.z = 0.0f;
            key.p.r = (uint8_t)(d ? 255 : 0);
            key.p.g = 128;
            key.p.b = (uint8_t)(d ? 0 : 255);
            key.p.w = 7;
            key.profile = DAI_SHOW_PROFILE_LINEAR;
        }
    }
    return dai_show_plan_from_keys(2, counts, k);
}

// Distance only: the speed, fence and ground rules are switched off so a
// distance test measures the distance test rather than four rules at once.
dai_show_settings distance_only(void) {
    dai_show_settings s = dai_show_settings_default();
    s.min_distance_m = 2.0f;
    s.v_max_ms       = 0.0f;
    s.a_max_ms2      = 0.0f;
    s.fence_half_x   = 0.0f;
    s.fence_half_z   = 0.0f;
    s.fence_top_m    = 0.0f;
    s.min_ground_m   = 0.0f;
    s.fps            = 25;
    return s;
}

// The honest reference: every pair, every tick, no grid. Only ever run on a
// small instance - it is here to prove the broadphase agrees with it, which is
// the only reason to trust the broadphase at 10,000.
uint32_t naive_pair_conflicts(const dai_show_plan *p, const dai_show_settings *s,
                              float *out_min) {
    uint32_t n = dai_show_plan_drone_count(p);
    uint32_t ticks = (uint32_t)std::floor((double)dai_show_plan_duration(p) * s->fps) + 1u;
    std::vector<dai_show_point> fleet(n);
    float mind = 1e30f;
    uint32_t episodes = 0;
    std::vector<uint8_t> was(n * n, 0), now(n * n, 0);
    for (uint32_t t = 0; t < ticks; ++t) {
        dai_show_plan_sample_all(p, (float)((double)t / s->fps), fleet.data());
        std::fill(now.begin(), now.end(), 0);
        for (uint32_t i = 0; i < n; ++i)
            for (uint32_t j = i + 1; j < n; ++j) {
                float dx = fleet[i].x - fleet[j].x, dy = fleet[i].y - fleet[j].y,
                      dz = fleet[i].z - fleet[j].z;
                float d = std::sqrt(dx * dx + dy * dy + dz * dz);
                if (d < mind) mind = d;
                if (d < s->min_distance_m) now[i * n + j] = 1;
            }
        for (uint32_t k = 0; k < n * n; ++k)
            if (was[k] && !now[k]) ++episodes;
        was = now;
    }
    for (uint32_t k = 0; k < n * n; ++k) if (was[k]) ++episodes;
    if (out_min) *out_min = mind;
    return episodes;
}

int file_exists(const char *path) {
    FILE *f = std::fopen(path, "rb");
    if (!f) return 0;
    std::fclose(f);
    return 1;
}

int same_key(const dai_show_key &a, const dai_show_key &b) {
    return std::memcmp(&a.t, &b.t, sizeof(float)) == 0 &&
           std::memcmp(&a.p.x, &b.p.x, sizeof(float)) == 0 &&
           std::memcmp(&a.p.y, &b.p.y, sizeof(float)) == 0 &&
           std::memcmp(&a.p.z, &b.p.z, sizeof(float)) == 0 &&
           a.p.r == b.p.r && a.p.g == b.p.g && a.p.b == b.p.b && a.p.w == b.p.w &&
           a.profile == b.profile;
}

// The same plan a director would export: a fleet moving between two grids,
// with a smooth leg in the middle so the profile has something to lose.
dai_show_plan *round_trip_plan(uint32_t n) {
    std::vector<dai_show_point> a(n), b(n);
    show_grid_formation(a.data(), n, 3.0f, dai_vec3{ 0.0f, 40.0f, 0.0f });
    show_grid_formation(b.data(), n, 3.0f, dai_vec3{ 25.0f, 60.0f, 10.0f });
    std::vector<uint32_t>     counts(n, 3);
    std::vector<dai_show_key> keys;
    keys.reserve(n * 3);
    for (uint32_t i = 0; i < n; ++i) {
        dai_show_key k;
        std::memset(&k, 0, sizeof(k));
        k.t = 0.0f;  k.p = a[i]; k.profile = DAI_SHOW_PROFILE_LINEAR;      keys.push_back(k);
        k.t = 6.25f; k.p = b[i]; k.profile = DAI_SHOW_PROFILE_SMOOTH;      keys.push_back(k);
        k.t = 9.5f;  k.p = a[i]; k.profile = DAI_SHOW_PROFILE_SMOOTH_LEFT; keys.push_back(k);
    }
    return dai_show_plan_from_keys(n, counts.data(), keys.data());
}

int plans_equal(const dai_show_plan *x, const dai_show_plan *y) {
    if (!x || !y) return 0;
    if (dai_show_plan_drone_count(x) != dai_show_plan_drone_count(y)) return 0;
    for (uint32_t i = 0; i < dai_show_plan_drone_count(x); ++i) {
        uint32_t kc = dai_show_plan_keyframe_count(x, i);
        if (kc != dai_show_plan_keyframe_count(y, i)) return 0;
        for (uint32_t k = 0; k < kc; ++k) {
            dai_show_key ka, kb;
            if (!dai_show_plan_key_at(x, i, k, &ka)) return 0;
            if (!dai_show_plan_key_at(y, i, k, &kb)) return 0;
            if (!same_key(ka, kb)) return 0;
        }
    }
    return 1;
}

} // namespace

int show_cases_io(void) {
    int before = g_show_fail;

    // ---- [5a] the planted collision --------------------------------------
    show_section("validation - a collision that was put there on purpose");
    {
        dai_show_plan *p = two_drone_plan(0.5f);
        dai_show_settings s = distance_only();
        dai_show_conflict c[16];
        dai_show_validate_stats st;
        uint32_t got = dai_show_validate(p, &s, c, 16, &st);

        CHECK(got >= 1 && st.conflicts >= 1, "the validator found nothing where two drones pass at 0.5 m");
        if (got >= 1) {
            CHECK(c[0].kind == DAI_SHOW_CONFLICT_DISTANCE, "kind is %d, not a distance conflict", c[0].kind);
            CHECK(c[0].a == 0 && c[0].b == 1, "the pair is (%u,%u), not (0,1)", c[0].a, c[0].b);
            CHECK(std::fabs(c[0].time_s - 1.0f) < 0.05f, "the closest moment is reported at %.3f s, not 1.0", c[0].time_s);
            CHECK(std::fabs(c[0].value - 0.5f) < 0.02f, "the gap is reported as %.3f m, not 0.5", c[0].value);
            CHECK(std::fabs(c[0].limit - 2.0f) < 1e-6f, "the limit is reported as %.3f, not 2.0", c[0].limit);
        }
        CHECK(std::fabs(st.min_distance_m - 0.5f) < 0.02f,
              "the tightest moment of the show is %.3f m, not 0.5", st.min_distance_m);
        dai_show_plan_destroy(p);
    }

    // ---- [5b] and a clean one stays clean --------------------------------
    {
        dai_show_plan *p = two_drone_plan(9.0f);
        dai_show_settings s = distance_only();
        dai_show_validate_stats st;
        uint32_t got = dai_show_validate(p, &s, nullptr, 0, &st);
        CHECK(got == 0 && st.conflicts == 0,
              "a plan with 9 m of clearance reported %u conflicts", st.conflicts);
        CHECK(st.ticks_checked > 0, "no tick was checked at all");
        dai_show_plan_destroy(p);
    }

    // ---- [5c] one planted violation per rule ------------------------------
    show_section("validation - each rule, broken once");
    {
        // 50 m in a quarter of a second: 200 m/s, and the acceleration to
        // match. Then a drone outside the fence, then one below the ground.
        uint32_t counts[3] = { 3, 2, 2 };
        dai_show_key k[7];
        std::memset(k, 0, sizeof(k));
        k[0].t = 0.0f; k[0].p.x = 0.0f;   k[0].p.y = 50.0f;
        k[1].t = 0.25f;k[1].p.x = 50.0f;  k[1].p.y = 50.0f;
        k[2].t = 2.0f; k[2].p.x = 50.0f;  k[2].p.y = 50.0f;
        k[3].t = 0.0f; k[3].p.x = 900.0f; k[3].p.y = 50.0f;   // outside the fence
        k[4].t = 2.0f; k[4].p.x = 900.0f; k[4].p.y = 50.0f;
        k[5].t = 0.0f; k[5].p.x = -900.0f;k[5].p.y = 0.25f;   // under the floor
        k[6].t = 2.0f; k[6].p.x = -900.0f;k[6].p.y = 0.25f;
        dai_show_plan *p = dai_show_plan_from_keys(3, counts, k);

        dai_show_settings s = dai_show_settings_default();
        s.min_distance_m = 2.0f;
        s.v_max_ms = 8.0f; s.a_max_ms2 = 4.0f;
        s.fence_half_x = 200.0f; s.fence_half_z = 200.0f; s.fence_top_m = 150.0f;
        s.min_ground_m = 2.0f;
        s.fps = 25;

        dai_show_conflict c[64];
        dai_show_validate_stats st;
        uint32_t got = dai_show_validate(p, &s, c, 64, &st);
        int seen[5] = { 0, 0, 0, 0, 0 };
        for (uint32_t i = 0; i < got; ++i)
            if (c[i].kind >= 0 && c[i].kind < 5) seen[c[i].kind] = 1;
        CHECK(seen[DAI_SHOW_CONFLICT_VMAX],   "200 m/s did not trip the speed limit");
        CHECK(seen[DAI_SHOW_CONFLICT_AMAX],   "800 m/s^2 did not trip the acceleration limit");
        CHECK(seen[DAI_SHOW_CONFLICT_FENCE],  "a drone 900 m out did not trip the geofence");
        CHECK(seen[DAI_SHOW_CONFLICT_GROUND], "a drone at 0.25 m did not trip the ground clearance");
        CHECK(st.max_speed_ms > 150.0f, "the peak speed came out as %.1f m/s", st.max_speed_ms);
        dai_show_plan_destroy(p);
    }

    // ---- [5d] sorted, and the same twice ----------------------------------
    {
        dai_show_plan *p = round_trip_plan(36);
        dai_show_settings s = dai_show_settings_default();
        s.min_distance_m = 4.0f;          // deliberately above the 3 m grid
        s.fps = 20;
        std::vector<dai_show_conflict> a(4096), b(4096);
        dai_show_validate_stats sa, sb;
        uint32_t ga = dai_show_validate(p, &s, a.data(), 4096, &sa);
        uint32_t gb = dai_show_validate(p, &s, b.data(), 4096, &sb);
        CHECK(ga > 0, "a 3 m grid checked against a 4 m minimum reported nothing");
        CHECK(ga == gb && std::memcmp(a.data(), b.data(), ga * sizeof(dai_show_conflict)) == 0,
              "two validations of one plan differ (%u vs %u conflicts)", ga, gb);
        int sorted = 1;
        for (uint32_t i = 1; i < ga; ++i) {
            const dai_show_conflict &x = a[i - 1], &y = a[i];
            if (x.time_s > y.time_s) { sorted = 0; break; }
            if (x.time_s == y.time_s && x.a > y.a) { sorted = 0; break; }
            if (x.time_s == y.time_s && x.a == y.a && x.b > y.b) { sorted = 0; break; }
        }
        CHECK(sorted, "the conflict list is not sorted by (time, a, b)");
        dai_show_plan_destroy(p);
    }

    // ---- [5e] the broadphase, against the honest reference ----------------
    show_section("validation - the grid agrees with every-pair, and does far less work");
    {
        dai_show_plan *p = round_trip_plan(64);
        dai_show_settings s = distance_only();
        s.min_distance_m = 2.5f;
        s.fps = 20;
        dai_show_validate_stats st;
        std::vector<dai_show_conflict> c(4096);
        uint32_t got = dai_show_validate(p, &s, c.data(), 4096, &st);
        float ref_min = 0.0f;
        uint32_t ref = naive_pair_conflicts(p, &s, &ref_min);
        CHECK(got == ref, "the grid found %u conflicts where every-pair finds %u", got, ref);
        CHECK(std::fabs(st.min_distance_m - ref_min) < 1e-3f,
              "the tightest gap is %.4f by grid, %.4f by every-pair", st.min_distance_m, ref_min);
        double per_tick = (double)st.pairs_tested / (double)st.ticks_checked;
        double naive    = 0.5 * 64.0 * 63.0;
        CHECK(per_tick < naive * 0.25,
              "%.0f pairs per tick against a naive %.0f - the grid is not doing its job",
              per_tick, naive);
        dai_show_plan_destroy(p);
    }

    // ---- [6a] JSON is lossless --------------------------------------------
    show_section("export - out and back in, key for key");
    {
        dai_show_plan *p = round_trip_plan(25);
        dai_show_settings s = dai_show_settings_default();
        s.drone_count = 25; s.show_origin_lat = 47.4979123456789;
        s.show_origin_lon = 19.0402123456789; s.min_distance_m = 2.25f;
        char err[256] = { 0 };
        dai_result r = dai_show_export_json(p, &s, "build/show_roundtrip.json", err, sizeof(err));
        CHECK(r == DAI_OK, "JSON export failed: %s", err);
        dai_show_settings back;
        std::memset(&back, 0, sizeof(back));
        dai_show_plan *q = dai_show_import_json("build/show_roundtrip.json", &back, err, sizeof(err));
        CHECK(q != nullptr, "JSON reimport failed: %s", err);
        CHECK(q && plans_equal(p, q), "the JSON round trip changed the plan");
        CHECK(std::memcmp(&back.show_origin_lat, &s.show_origin_lat, sizeof(double)) == 0,
              "the WGS84 origin came back as %.13f, not %.13f", back.show_origin_lat, s.show_origin_lat);
        CHECK(std::memcmp(&back.min_distance_m, &s.min_distance_m, sizeof(float)) == 0,
              "the minimum distance came back as %f", back.min_distance_m);
        dai_show_plan_destroy(q);
        dai_show_plan_destroy(p);
    }

    // ---- [6b] .skyc is a ZIP, and lossless too -----------------------------
    {
        dai_show_plan *p = round_trip_plan(16);
        dai_show_settings s = dai_show_settings_default();
        s.drone_count = 16;
        char err[256] = { 0 };
        dai_result r = dai_show_export_skyc(p, &s, "build/show_roundtrip.skyc", err, sizeof(err));
        CHECK(r == DAI_OK, ".skyc export failed: %s", err);

        char magic[4] = { 0 };
        FILE *f = std::fopen("build/show_roundtrip.skyc", "rb");
        if (f) { size_t n = std::fread(magic, 1, 4, f); (void)n; std::fclose(f); }
        CHECK(std::memcmp(magic, "PK\3\4", 4) == 0, "the .skyc does not start with a ZIP local header");

        dai_show_settings back;
        dai_show_plan *q = dai_show_import_skyc("build/show_roundtrip.skyc", &back, err, sizeof(err));
        CHECK(q != nullptr, ".skyc reimport failed: %s", err);
        CHECK(q && plans_equal(p, q), "the .skyc round trip changed the plan");
        CHECK(back.fps == s.fps && back.drone_count == s.drone_count,
              "the .skyc settings came back as %d fps, %u drones", back.fps, back.drone_count);
        dai_show_plan_destroy(q);
        dai_show_plan_destroy(p);
    }

    // ---- [6c] CSV says what the header file promises ------------------------
    {
        dai_show_plan *p = round_trip_plan(9);
        dai_show_settings s = dai_show_settings_default();
        s.fps = 10;
        char err[256] = { 0 };
        dai_result r = dai_show_export_csv(p, &s, "build/show_roundtrip.csv", err, sizeof(err));
        CHECK(r == DAI_OK, "CSV export failed: %s", err);

        FILE *f = std::fopen("build/show_roundtrip.csv", "rb");
        CHECK(f != nullptr, "the CSV was not written");
        if (f) {
            char head[64] = { 0 };
            if (!std::fgets(head, sizeof(head), f)) head[0] = 0;
            CHECK(std::strncmp(head, "time_s,drone,x,y,z,r,g,b,w", 26) == 0,
                  "the CSV header reads \"%s\"", head);
            uint32_t lines = 0;
            double t0 = -1.0, x0 = 0.0, y0 = 0.0, z0 = 0.0;
            unsigned d0 = 0, r0 = 0, g0 = 0, b0 = 0, w0 = 0;
            char line[256];
            while (std::fgets(line, sizeof(line), f)) {
                if (lines == 0)
                    std::sscanf(line, "%lf,%u,%lf,%lf,%lf,%u,%u,%u,%u",
                                &t0, &d0, &x0, &y0, &z0, &r0, &g0, &b0, &w0);
                ++lines;
            }
            std::fclose(f);
            uint32_t frames = (uint32_t)std::floor(dai_show_plan_duration(p) * 10.0f) + 1u;
            CHECK(lines == frames * 9u, "the CSV has %u rows, not %u drones x %u frames",
                  lines, 9u, frames);
            dai_show_point ref;
            dai_show_plan_sample(p, 0, 0.0f, &ref);
            CHECK(std::fabs(x0 - ref.x) < 1e-4 && std::fabs(y0 - ref.y) < 1e-4 &&
                  std::fabs(z0 - ref.z) < 1e-4 && r0 == ref.r && g0 == ref.g &&
                  b0 == ref.b && w0 == ref.w,
                  "the first CSV row is (%.3f,%.3f,%.3f) where the plan says (%.3f,%.3f,%.3f)",
                  x0, y0, z0, (double)ref.x, (double)ref.y, (double)ref.z);
        }
        dai_show_plan_destroy(p);
    }

    // ---- [6d] a refused export leaves nothing behind -------------------------
    {
        dai_show_plan *p = round_trip_plan(4);
        dai_show_settings s = dai_show_settings_default();
        char err[256] = { 0 };
        const char *bad = "build/no_such_directory_here/show.json";
        dai_result r = dai_show_export_json(p, &s, bad, err, sizeof(err));
        CHECK(r != DAI_OK, "an export into a directory that does not exist reported success");
        CHECK(err[0] != 0, "a failed export said nothing about why");
        CHECK(!file_exists(bad), "a failed export left a file behind");
        CHECK(!file_exists("build/no_such_directory_here/show.json.tmp"),
              "a failed export left its temporary file behind");
        dai_show_plan_destroy(p);
    }

    return g_show_fail - before;
}
