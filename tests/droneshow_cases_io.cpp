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
//   [5k] a fence episode that is measured first against a side wall and then
//        against the ceiling reports the WORSE of the two - the deeper
//        overshoot - together with the limit that overshoot belongs to.
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
//   [6e] THE FLIGHT PROOF: the pipeline's own show is exported to .skyc, read
//        back, and the REIMPORTED trajectories are flown over the whole
//        timeline at four steps per tick - separation, speed and acceleration.
//        Nothing of the plan object is consulted; what is measured is what the
//        file says the drones will do. The separation of a pair inside a
//        sub-step is a proven LOWER BOUND, not a sample: the chord distance
//        less an eighth of the second difference of the relative motion, the
//        same correction daishow::min_separation applies, refined until it
//        agrees to SEP_EPS with a distance the pair really reaches. So "no pair
//        ever came closer than X" is a statement about the whole timeline and
//        not about the instants that happened to be looked at.
//
//   [5f..5j] ONE RULE AT A TIME. [5c] proves the five rules are all there at
//        once, which is the wrong test to read when one of them is wrong: it
//        says "a kind is missing" and nothing else. The five cases below break
//        exactly one rule each, in a plan that satisfies the other four, and
//        each of them names the pair, the value, the limit and the second the
//        conflict happened in - the four things a director clicks on in the
//        Validation panel. A rule that stops being reported turns exactly one
//        of them red.
#include "droneshow_cases.hpp"
#include "dai_show_internal.hpp"

#include <algorithm>
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

// ---- one rule at a time ----------------------------------------------------
//
// All five limits switched on together, so each of the cases below is a plan
// that satisfies four rules and breaks one. A conflict list of length one is
// then the whole assertion: the right rule fired, and none of the others did.
dai_show_settings all_rules(void) {
    dai_show_settings s = dai_show_settings_default();
    s.min_distance_m = 2.0f;
    s.v_max_ms       = 8.0f;
    s.a_max_ms2      = 4.0f;
    s.fence_half_x   = 200.0f;
    s.fence_half_z   = 200.0f;
    s.fence_top_m    = 150.0f;
    s.min_ground_m   = 2.0f;
    s.fps            = 25;
    return s;
}

// A plan written out by hand, one key list per drone. Everything in the five
// single-rule cases is built with this, because a solver's output is the wrong
// input for a test about what the validator sees: the solver would refuse to
// produce four of these five plans, which is exactly why they have to be
// written by hand.
dai_show_plan *hand_plan(const std::vector<std::vector<dai_show_key>> &drones) {
    std::vector<uint32_t>     counts;
    std::vector<dai_show_key> keys;
    for (const auto &d : drones) {
        counts.push_back((uint32_t)d.size());
        for (const dai_show_key &k : d) keys.push_back(k);
    }
    return dai_show_plan_from_keys((uint32_t)counts.size(), counts.data(), keys.data());
}

dai_show_key key_at(float t, float x, float y, float z, int profile) {
    dai_show_key k;
    std::memset(&k, 0, sizeof(k));
    k.t = t; k.p.x = x; k.p.y = y; k.p.z = z;
    k.p.r = 200; k.p.g = 200; k.p.b = 200; k.p.w = 0;
    k.profile = profile;
    return k;
}

// Reports what the one conflict of a single-rule case says, in the four terms
// the Validation panel shows: which pair, how much, against what, and when.
void expect_one(const dai_show_plan *p, const dai_show_settings *s, int kind,
                uint32_t a, uint32_t b, float value, float value_tol,
                float limit, float time_s, float time_tol, const char *what) {
    dai_show_conflict c[32];
    dai_show_validate_stats st;
    uint32_t got = dai_show_validate(p, s, c, 32, &st);
    CHECK(got == 1 && st.conflicts == 1,
          "%s: the validator reported %u conflicts where exactly one was planted", what, got);
    if (got < 1) return;
    CHECK(c[0].kind == kind, "%s: the conflict came back as kind %d, not %d", what, c[0].kind, kind);
    CHECK(c[0].a == a && c[0].b == b, "%s: the conflict names (%u,%u), not (%u,%u)",
          what, c[0].a, c[0].b, a, b);
    CHECK(std::fabs(c[0].value - value) <= value_tol,
          "%s: the value is %.4f, not %.4f +- %.4f", what, (double)c[0].value,
          (double)value, (double)value_tol);
    CHECK(std::fabs(c[0].limit - limit) < 1e-6f,
          "%s: the limit is reported as %.4f, not %.4f", what, (double)c[0].limit, (double)limit);
    CHECK(std::fabs(c[0].time_s - time_s) <= time_tol,
          "%s: the moment is %.3f s, not %.3f +- %.3f s", what, (double)c[0].time_s,
          (double)time_s, (double)time_tol);
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

// ---- the flight proof ------------------------------------------------------
//
// Everything below reads a plan the way a ground station would read the file:
// through dai_show_plan_sample, at times of its own choosing, with no access to
// what the solver believed. It is deliberately NOT the validator's algorithm -
// no grid, no episodes, no keyframe cursor - because a check that shares its
// method with the thing it checks proves that the method is consistent, not
// that the show flies.

// A pair that dips below the floor somewhere in the flight, collapsed to the
// worst instant of the whole show rather than repeated per frame.
struct Breach {
    uint32_t a, b;
    float    sep;
    float    t;
};

struct Audit {
    std::vector<Breach> breaches;
    float    sep;            // the closest two drones ever come, in metres
    uint32_t sep_a, sep_b;
    float    sep_t;
    float    speed;          // the fastest any drone ever flies
    uint32_t speed_d;
    float    speed_t;
    float    accel;          // the hardest any drone ever accelerates
    uint32_t accel_d;
    float    accel_t;
};

float dist3(const dai_show_point &a, const dai_show_point &b) {
    float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// The closest the two segments a0->a1 and b0->b1 come to each other. Written
// out here rather than borrowed: it is the same quantity the separator and the
// validator work with, and a test that calls their function cannot fail when
// their function is wrong.
//
// On its own this is a statement about two STRAIGHT lines, not about two
// drones: where the trajectories bend away from their chords inside the step
// the true pair can be closer than the chords ever are. What makes it a bound
// on the flight is the curvature term pair_separation subtracts from it below.
float segment_separation(const dai_show_point &a0, const dai_show_point &a1,
                         const dai_show_point &b0, const dai_show_point &b1) {
    const float ux = a1.x - a0.x, uy = a1.y - a0.y, uz = a1.z - a0.z;
    const float vx = b1.x - b0.x, vy = b1.y - b0.y, vz = b1.z - b0.z;
    const float wx = a0.x - b0.x, wy = a0.y - b0.y, wz = a0.z - b0.z;
    const float A = ux * ux + uy * uy + uz * uz;
    const float B = ux * vx + uy * vy + uz * vz;
    const float C = vx * vx + vy * vy + vz * vz;
    const float D = ux * wx + uy * wy + uz * wz;
    const float E = vx * wx + vy * wy + vz * wz;
    const float det = A * C - B * B;
    float sN, sD = det, tN, tD = det;
    if (det < 1e-9f) { sN = 0.0f; sD = 1.0f; tN = E; tD = C > 0.0f ? C : 1.0f; }
    else {
        sN = B * E - C * D;
        tN = A * E - B * D;
        if (sN < 0.0f)      { sN = 0.0f; tN = E;     tD = C > 0.0f ? C : 1.0f; }
        else if (sN > sD)   { sN = sD;   tN = E + B; tD = C > 0.0f ? C : 1.0f; }
    }
    if (tN < 0.0f) {
        tN = 0.0f;
        if (-D < 0.0f) sN = 0.0f; else if (-D > A) sN = sD; else { sN = -D; sD = A; }
    } else if (tN > tD) {
        tN = tD;
        if ((-D + B) < 0.0f) sN = 0.0f;
        else if ((-D + B) > A) sN = sD;
        else { sN = -D + B; sD = A; }
    }
    const float sc = (std::fabs(sD) < 1e-9f) ? 0.0f : sN / sD;
    const float tc = (std::fabs(tD) < 1e-9f) ? 0.0f : tN / tD;
    const float cx = wx + sc * ux - tc * vx;
    const float cy = wy + sc * uy - tc * vy;
    const float cz = wz + sc * uz - tc * vz;
    return std::sqrt(cx * cx + cy * cy + cz * cz);
}

// A pair over one sub-step, as a LOWER BOUND on what the two drones really do
// in it - never an optimistic one - refined until the bound and a distance the
// pair provably takes agree.
//
// Two straight chords are not two trajectories. Between the sampled instants
// each drone may leave its chord, and a pair that its chords keep 2.00 m apart
// can pass at less. The step is therefore paid for the same way
// daishow::min_separation pays for it, with the same constant: the middle of
// the step is sampled, the second difference of the RELATIVE motion is taken
// over the three instants, and an eighth of its length comes off the chord
// distance. For a curve with a bounded second derivative an eighth of the
// second difference is exactly how far it can leave the chord between the two
// ends, so what is returned cannot be above the true minimum of the step.
//
// It cannot be far BELOW it either, and that is what the loop is for: `hi` is
// a distance the pair really reaches (at an end or in the middle), so `hi - lo`
// is everything still unproven. Halving the step halves the chord error and
// quarters the curvature term, and the recursion stops as soon as the two agree
// to SEP_EPS - the tolerance the whole pipeline shares - or at six levels,
// which is a step under a millisecond. What comes back is then a bound that is
// tight to SEP_EPS, in metres, over the whole sub-step and not only at its ends.
float pair_separation(const dai_show_plan *p, uint32_t i, uint32_t j,
                      float t0, float t1, const dai_show_point &a0,
                      const dai_show_point &a1, const dai_show_point &b0,
                      const dai_show_point &b1, int depth) {
    const float tm = 0.5f * (t0 + t1);
    dai_show_point am, bm;
    dai_show_plan_sample(p, i, tm, &am);
    dai_show_plan_sample(p, j, tm, &bm);

    // The second difference of the relative position over (t0, tm, t1).
    const float sx = (a0.x - b0.x) - 2.0f * (am.x - bm.x) + (a1.x - b1.x);
    const float sy = (a0.y - b0.y) - 2.0f * (am.y - bm.y) + (a1.y - b1.y);
    const float sz = (a0.z - b0.z) - 2.0f * (am.z - bm.z) + (a1.z - b1.z);
    const float slack = 0.125f * std::sqrt(sx * sx + sy * sy + sz * sz);

    const float lo = segment_separation(a0, a1, b0, b1) - slack;
    const float hi = std::min(std::min(dist3(a0, b0), dist3(a1, b1)), dist3(am, bm));
    if (depth <= 0 || hi - lo <= daishow::SEP_EPS) return lo;
    return std::min(pair_separation(p, i, j, t0, tm, a0, am, b0, bm, depth - 1),
                    pair_separation(p, i, j, tm, t1, am, a1, bm, b1, depth - 1));
}

// The whole show, flown. `sub` sub-steps per tick for the separation, and the
// speed and the acceleration measured over the export's own frame interval at
// every one of those sub-step phases - so a peak that falls between two
// exported frames is measured too, which is the half of the timeline a check
// that only looks at frame boundaries never sees.
Audit fly(const dai_show_plan *p, const dai_show_settings *s, int sub, float floor_m) {
    Audit w;
    w.sep = 3.4e38f; w.sep_a = 0; w.sep_b = 0; w.sep_t = 0.0f;
    w.speed = 0.0f; w.speed_d = 0; w.speed_t = 0.0f;
    w.accel = 0.0f; w.accel_d = 0; w.accel_t = 0.0f;

    const uint32_t n = dai_show_plan_drone_count(p);
    if (!n) return w;
    const int   fps = (s->fps > 0) ? s->fps : 25;
    const float dt  = 1.0f / (float)fps;
    const float h   = dt / (float)sub;
    const float dur = dai_show_plan_duration(p);
    const uint32_t frames = (uint32_t)std::floor((double)dur / (double)h) + 1u;

    // A ring of 2*sub+1 frames: enough to difference twice over a whole frame
    // interval at every sub-step phase, and nothing like the whole show in
    // memory.
    const int ring = 2 * sub + 1;
    std::vector<std::vector<dai_show_point>> hist((size_t)ring, std::vector<dai_show_point>(n));
    std::vector<dai_show_point> mid(n);
    // How far a drone can be from the END of the sub-step at any instant
    // inside it: the chord it flies, plus how far it may leave that chord -
    // an eighth of the second difference over the step, the same constant
    // pair_separation and daishow::min_separation pay. Chord alone was the
    // quiet optimism in this filter: a pair skipped by it was skipped on the
    // assumption that both drones fly straight lines between samples.
    std::vector<float> reach(n, 0.0f);

    for (uint32_t f = 0; f < frames; ++f) {
        const float t = (float)((double)f * (double)h);
        std::vector<dai_show_point> &now = hist[f % (uint32_t)ring];
        dai_show_plan_sample_all(p, t, now.data());

        if (f > 0) {
            const std::vector<dai_show_point> &was = hist[(f - 1) % (uint32_t)ring];
            dai_show_plan_sample_all(p, (float)((double)t - 0.5 * (double)h), mid.data());
            for (uint32_t i = 0; i < n; ++i) {
                const float cx = was[i].x - 2.0f * mid[i].x + now[i].x;
                const float cy = was[i].y - 2.0f * mid[i].y + now[i].y;
                const float cz = was[i].z - 2.0f * mid[i].z + now[i].z;
                reach[i] = dist3(was[i], now[i]) +
                           0.125f * std::sqrt(cx * cx + cy * cy + cz * cz);
            }
            for (uint32_t i = 0; i < n; ++i)
                for (uint32_t j = i + 1; j < n; ++j) {
                    const float end_d = dist3(now[i], now[j]);
                    // Nothing inside the step can be closer than this - neither
                    // drone can be further from where the step ends than its
                    // own reach - and the step is a fortieth of a second: almost
                    // every pair leaves here.
                    const float bound = end_d - reach[i] - reach[j];
                    if (bound >= w.sep && bound >= floor_m) continue;
                    const float d = pair_separation(p, i, j, t - h, t, was[i], now[i],
                                                    was[j], now[j], 6);
                    if (d < w.sep) { w.sep = d; w.sep_a = i; w.sep_b = j; w.sep_t = t; }
                    if (d < floor_m) {
                        size_t at = w.breaches.size();
                        for (size_t k = 0; k < w.breaches.size(); ++k)
                            if (w.breaches[k].a == i && w.breaches[k].b == j) { at = k; break; }
                        if (at == w.breaches.size()) w.breaches.push_back(Breach{ i, j, d, t });
                        else if (d < w.breaches[at].sep) { w.breaches[at].sep = d; w.breaches[at].t = t; }
                    }
                }
        }
        // Speed over one export frame, ending here; acceleration over two.
        if (f >= (uint32_t)sub) {
            const std::vector<dai_show_point> &back1 = hist[(f - (uint32_t)sub) % (uint32_t)ring];
            for (uint32_t i = 0; i < n; ++i) {
                const float sp = dist3(back1[i], now[i]) / dt;
                if (sp > w.speed) { w.speed = sp; w.speed_d = i; w.speed_t = t; }
            }
        }
        if (f >= (uint32_t)(2 * sub)) {
            const std::vector<dai_show_point> &back1 = hist[(f - (uint32_t)sub) % (uint32_t)ring];
            const std::vector<dai_show_point> &back2 = hist[(f - (uint32_t)(2 * sub)) % (uint32_t)ring];
            for (uint32_t i = 0; i < n; ++i) {
                const float ax = ((now[i].x - back1[i].x) - (back1[i].x - back2[i].x)) / (dt * dt);
                const float ay = ((now[i].y - back1[i].y) - (back1[i].y - back2[i].y)) / (dt * dt);
                const float az = ((now[i].z - back1[i].z) - (back1[i].z - back2[i].z)) / (dt * dt);
                const float acc = std::sqrt(ax * ax + ay * ay + az * az);
                if (acc > w.accel) { w.accel = acc; w.accel_d = i; w.accel_t = t; }
            }
        }
    }
    return w;
}

// A torus as a triangle soup: the ring the screenshot show has in the middle
// of it, and the figure that puts every drone on a thin closed curve. Written
// out here rather than shared, because the two suites that want a ring want it
// for different reasons and a fixture header nobody reads is a fixture that
// rots.
void ring_soup(std::vector<float> &pos, std::vector<uint32_t> &idx, int segs, float thick) {
    auto tri = [&](const float *a, const float *b, const float *c) {
        const float *v[3] = { a, b, c };
        for (int i = 0; i < 3; ++i) {
            idx.push_back((uint32_t)(pos.size() / 3));
            pos.push_back(v[i][0]); pos.push_back(v[i][1]); pos.push_back(v[i][2]);
        }
    };
    for (int j = 0; j < segs; ++j) {
        const float t0 = 6.28318530718f * (float)j / (float)segs;
        const float t1 = 6.28318530718f * (float)(j + 1) / (float)segs;
        for (int k = 0; k < 12; ++k) {
            const float u0 = 6.28318530718f * (float)k / 12.0f;
            const float u1 = 6.28318530718f * (float)(k + 1) / 12.0f;
            auto at = [&](float t, float u, float *o) {
                const float rr = 1.0f + thick * std::cos(u);
                o[0] = rr * std::cos(t); o[1] = thick * std::sin(u); o[2] = rr * std::sin(t);
            };
            float a[3], b[3], c[3], d[3];
            at(t0, u0, a); at(t1, u0, b); at(t1, u1, c); at(t0, u1, d);
            tri(a, b, c); tri(a, c, d);
        }
    }
}

// The show the flight proof is about, and it is deliberately not a show this
// file invented: it is the same 420 drones, the same three figures, the same
// seed and the same planted near miss as [3g] in
// tests/droneshow_cases_plan.cpp - the show the screenshots are taken of. A
// flight proof of a show chosen because it passes proves nothing about the
// pipeline; this one is the show the rest of the round is judged on, exported
// and flown out of the file.
dai_show *screenshot_show(dai_show_settings *out, float *planted_gap) {
    dai_show_settings s = dai_show_settings_default();
    s.drone_count    = 420;
    s.min_distance_m = 2.0f;
    s.v_max_ms       = 8.0f;
    s.a_max_ms2      = 4.0f;
    s.fps            = 10;
    s.takeoff_alt_m  = 30.0f;
    s.seed           = 20260814ull;
    if (out) *out = s;
    if (planted_gap) *planted_gap = 0.4f;

    dai_show *sh = dai_show_create(&s);
    if (!sh) return nullptr;

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
            d.positions    = m.positions;
            d.normals      = m.normals;
            d.uvs          = m.uvs;
            d.vertex_count = m.vertex_count;
            d.indices      = m.indices;
            d.index_count  = m.index_count;
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
        if (idx == 0xFFFFFFFFu) {
            CHECK(0, "[6e] %s could not be sampled: %s", figs[i].name, err);
            dai_show_destroy(sh);
            return nullptr;
        }
        dai_show_formation_set_hold(sh, idx, 4.0f);
    }

    // The planted fault, exactly as [3g] and the screenshot tool plant it: two
    // points 0.4 m apart in a formation. It is what makes this case able to
    // fail - a flight audit that reports nothing on a show with a known fault
    // in it is a flight audit that measures nothing.
    {
        const dai_show_point *base = dai_show_formation_points(sh, 0);
        if (!base) {
            CHECK(0, "[6e] the first formation has no points");
            dai_show_destroy(sh);
            return nullptr;
        }
        std::vector<dai_show_point> pts(base, base + s.drone_count);
        pts[173] = pts[172];
        pts[173].x += 0.4f;
        dai_show_formation_add(sh, "Sphere (near miss)", "test://figure",
                               pts.data(), (uint32_t)pts.size(), 5.0f);
    }

    dai_show_solve(sh, nullptr, 0);
    return sh;
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

    // ---- [5f..5j] one rule at a time, named to the second ------------------
    show_section("validation - each rule alone: which pair, how much, against what, when");
    {
        // [5f] DISTANCE. Two drones ease towards each other and back, all five
        // limits on: at ten seconds they are half a metre apart and nothing
        // else about the plan is illegal.
        std::vector<std::vector<dai_show_key>> d(2);
        for (int i = 0; i < 2; ++i) {
            const float side = i ? 1.0f : -1.0f;
            d[i].push_back(key_at( 0.0f, side * 10.0f,  50.0f, 0.0f, DAI_SHOW_PROFILE_SMOOTH));
            d[i].push_back(key_at(10.0f, side *  0.25f, 50.0f, 0.0f, DAI_SHOW_PROFILE_SMOOTH));
            d[i].push_back(key_at(20.0f, side * 10.0f,  50.0f, 0.0f, DAI_SHOW_PROFILE_SMOOTH));
        }
        dai_show_plan *p = hand_plan(d);
        dai_show_settings s = all_rules();
        expect_one(p, &s, DAI_SHOW_CONFLICT_DISTANCE, 0, 1, 0.5f, 0.01f, 2.0f, 10.0f, 0.1f,
                   "[5f] the minimum distance");
        dai_show_plan_destroy(p);
    }
    {
        // [5g] V_MAX alone. 160 m in 20 s eased at both ends peaks at 12 m/s
        // and at 2.4 m/s2: over the speed limit, under the acceleration one,
        // inside the fence and well above the ground.
        std::vector<std::vector<dai_show_key>> d(1);
        d[0].push_back(key_at( 0.0f, -80.0f, 50.0f, 0.0f, DAI_SHOW_PROFILE_SMOOTH));
        d[0].push_back(key_at(20.0f,  80.0f, 50.0f, 0.0f, DAI_SHOW_PROFILE_SMOOTH));
        dai_show_plan *p = hand_plan(d);
        dai_show_settings s = all_rules();
        expect_one(p, &s, DAI_SHOW_CONFLICT_VMAX, 0, 0, 12.0f, 0.2f, 8.0f, 10.0f, 0.3f,
                   "[5g] the speed limit");
        dai_show_plan_destroy(p);
    }
    {
        // [5h] A_MAX alone. Six metres in two seconds peaks at 4.5 m/s - under
        // the speed limit - and at 8.6 m/s2, which is not.
        //
        // An eased move breaks the acceleration limit TWICE and in two places a
        // pilot would name separately: pushing off (a > 0 for the first third)
        // and braking (a < 0 for the last third), with a stretch in the middle,
        // half the move long, where the drone coasts inside the limit. An
        // episode is a contiguous stretch of a broken rule, so this plan is two
        // of them, and collapsing them into one would hide the second moment -
        // the braking - from the panel that is supposed to point at it. Both
        // are asserted here, each in its own half of the move.
        std::vector<std::vector<dai_show_key>> d(1);
        d[0].push_back(key_at(0.0f, 0.0f, 50.0f, 0.0f, DAI_SHOW_PROFILE_SMOOTH));
        d[0].push_back(key_at(2.0f, 6.0f, 50.0f, 0.0f, DAI_SHOW_PROFILE_SMOOTH));
        d[0].push_back(key_at(6.0f, 6.0f, 50.0f, 0.0f, DAI_SHOW_PROFILE_SMOOTH));
        dai_show_plan *p = hand_plan(d);
        dai_show_settings s = all_rules();
        dai_show_conflict c[32];
        dai_show_validate_stats st;
        uint32_t got = dai_show_validate(p, &s, c, 32, &st);
        uint32_t amax = 0, foreign = 0;
        int foreign_kind = -1;
        for (uint32_t i = 0; i < got; ++i) {
            if (c[i].kind == DAI_SHOW_CONFLICT_AMAX) ++amax;
            else { ++foreign; if (foreign_kind < 0) foreign_kind = c[i].kind; }
        }
        CHECK(got == 2 && amax == 2,
              "[5h] the acceleration limit: %u conflicts, %u of them acceleration "
              "(the push off and the braking of one eased move)", got, amax);
        CHECK(foreign == 0,
              "[5h] a plan meant to break only the acceleration rule reported %u "
              "conflicts of another kind, the first of kind %d", foreign, foreign_kind);
        if (got == 2 && amax == 2) {
            for (uint32_t i = 0; i < 2; ++i) {
                CHECK(c[i].a == 0 && c[i].b == 0,
                      "[5h] the conflict names (%u,%u), not (0,0)", c[i].a, c[i].b);
                CHECK(c[i].value > 4.0f && c[i].value < 12.0f,
                      "[5h] the peak acceleration is reported as %.3f m/s2", (double)c[i].value);
                CHECK(std::fabs(c[i].limit - 4.0f) < 1e-6f,
                      "[5h] the limit is reported as %.3f, not 4.0", (double)c[i].limit);
            }
            // Sorted by time: the push off first, the braking second, and
            // neither of them inside the coast between them.
            CHECK(c[0].time_s >= 0.0f && c[0].time_s <= 0.7f,
                  "[5h] the push off is dated %.3f s, outside the first third of the move",
                  (double)c[0].time_s);
            CHECK(c[1].time_s >= 1.3f && c[1].time_s <= 2.2f,
                  "[5h] the braking is dated %.3f s, outside the last third of the move",
                  (double)c[1].time_s);
        }
        CHECK(st.max_speed_ms < 8.0f,
              "[5h] the plan meant to break only the acceleration rule reaches %.3f m/s",
              (double)st.max_speed_ms);
        dai_show_plan_destroy(p);
    }
    {
        // [5i] GROUND alone: one drone hovering a metre up, under a two metre
        // floor, standing still and inside the fence.
        std::vector<std::vector<dai_show_key>> d(1);
        d[0].push_back(key_at(0.0f, 0.0f, 1.0f, 0.0f, DAI_SHOW_PROFILE_LINEAR));
        d[0].push_back(key_at(4.0f, 0.0f, 1.0f, 0.0f, DAI_SHOW_PROFILE_LINEAR));
        dai_show_plan *p = hand_plan(d);
        dai_show_settings s = all_rules();
        expect_one(p, &s, DAI_SHOW_CONFLICT_GROUND, 0, 0, 1.0f, 1e-4f, 2.0f, 0.0f, 1e-4f,
                   "[5i] the ground clearance");
        dai_show_plan_destroy(p);
    }
    {
        // [5j] FENCE alone: a hundred metres outside a two hundred metre box,
        // at a legal height, not moving.
        std::vector<std::vector<dai_show_key>> d(1);
        d[0].push_back(key_at(0.0f, 300.0f, 50.0f, 0.0f, DAI_SHOW_PROFILE_LINEAR));
        d[0].push_back(key_at(4.0f, 300.0f, 50.0f, 0.0f, DAI_SHOW_PROFILE_LINEAR));
        dai_show_plan *p = hand_plan(d);
        dai_show_settings s = all_rules();
        expect_one(p, &s, DAI_SHOW_CONFLICT_FENCE, 0, 0, 300.0f, 1e-3f, 200.0f, 0.0f, 1e-4f,
                   "[5j] the geofence");
        dai_show_plan_destroy(p);
    }
    {
        // [5k] FENCE severity: one episode, two walls.
        //
        // The drone hangs a metre past the 200 m side wall for the whole show
        // and rises through the 150 m ceiling in the middle of it, so a single
        // unbroken episode is measured first against one limit and then
        // against another. Which of the two moments is the WORST is the whole
        // point: fifteen metres over the ceiling is a worse breach than one
        // metre past the wall, and the only number that says so is the
        // overshoot. The raw values are 165 and 201, so a rule that keeps the
        // SMALLER of the two - "lower is worse", which is right for distance
        // and ground and wrong for a fence - reports a drone that is fifteen
        // metres out as 165 of 200: comfortably inside a limit it is nowhere
        // near. The limit has to travel with the value, or the pair that
        // leaves the validator was never measured together.
        //
        // Speed and acceleration are switched off rather than tiptoed around:
        // this case is about the fence alone, and a rise slow enough to please
        // a 4 m/s2 limit would take seven minutes of ticks to say the same
        // thing.
        std::vector<std::vector<dai_show_key>> d(1);
        d[0].push_back(key_at( 0.0f, 201.0f,  50.0f, 0.0f, DAI_SHOW_PROFILE_LINEAR));
        d[0].push_back(key_at(60.0f, 201.0f, 165.0f, 0.0f, DAI_SHOW_PROFILE_LINEAR));
        d[0].push_back(key_at(70.0f, 201.0f, 165.0f, 0.0f, DAI_SHOW_PROFILE_LINEAR));
        dai_show_plan *p = hand_plan(d);
        dai_show_settings s = all_rules();
        s.v_max_ms  = 0.0f;
        s.a_max_ms2 = 0.0f;
        expect_one(p, &s, DAI_SHOW_CONFLICT_FENCE, 0, 0, 165.0f, 1e-3f, 150.0f, 60.0f, 0.2f,
                   "[5k] the fence severity");
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

    // ---- [6e] the exported show, flown ---------------------------------------
    //
    // The claim this whole tool is sold on, measured on the file that leaves
    // the building: the only pair of the reimported trajectories that ever
    // comes closer than the minimum distance is the fault the fixture planted
    // inside a formation - which no transition can undo and which the flight is
    // therefore only required not to make worse - no trajectory exceeds v_max
    // or a_max, and it is all checked between the exported frames, not only on
    // them.
    show_section("export - the .skyc, read back and flown, is flyable");
    {
        dai_show_settings s;
        float planted_gap = 0.4f;
        dai_show *sh = screenshot_show(&s, &planted_gap);
        CHECK(sh != nullptr, "[6e] the pipeline show could not be built");
        if (sh) {
            const dai_show_plan *plan = dai_show_get_plan(sh);
            CHECK(plan != nullptr, "[6e] the solved show has no plan");
            char err[256] = { 0 };
            dai_result r = dai_show_export_skyc(plan, &s, "build/show_flight.skyc", err, sizeof(err));
            CHECK(r == DAI_OK, "[6e] .skyc export failed: %s", err);

            dai_show_settings back;
            std::memset(&back, 0, sizeof(back));
            dai_show_plan *q = dai_show_import_skyc("build/show_flight.skyc", &back, err, sizeof(err));
            CHECK(q != nullptr, "[6e] the exported .skyc could not be read back: %s", err);
            if (q) {
                CHECK(dai_show_plan_drone_count(q) == s.drone_count,
                      "[6e] the file carries %u drones, the show has %u",
                      dai_show_plan_drone_count(q), s.drone_count);
                CHECK(std::fabs(dai_show_plan_duration(q) - dai_show_plan_duration(plan)) < 1e-3f,
                      "[6e] the file is %.3f s long, the show is %.3f s",
                      (double)dai_show_plan_duration(q), (double)dai_show_plan_duration(plan));

                // Four sub-steps per exported frame, and over each of them
                // the curvature-corrected bound on every pair - the proven
                // minimum of the step, not the distance at its ends.
                const Audit w = fly(q, &back, 4, back.min_distance_m);

                // What the flight is allowed to contain, and nothing else: the
                // pair the FIXTURE parks 0.4 m apart inside a formation. No
                // route, delay or stretch can pull apart what a formation puts
                // together (see [3i]), so the promise the pipeline really makes
                // is the one measured here - that one pair is the ONLY pair in
                // 420 that ever goes below the minimum distance, anywhere on
                // the timeline, and that the flight never makes it worse than
                // the formation already is. Every other pair keeps the full
                // 2 m. Recognised by its gap rather than by its index, because
                // assignment renumbers who flies where.
                uint32_t planted_seen = 0, other = 0;
                float    planted_worst = 3.4e38f;
                uint32_t oa = 0, ob = 0; float ov = 0.0f, ot = 0.0f;
                for (size_t bi = 0; bi < w.breaches.size(); ++bi) {
                    const Breach &b = w.breaches[bi];
                    if (std::fabs(b.sep - planted_gap) < 0.02f) {
                        ++planted_seen;
                        if (b.sep < planted_worst) planted_worst = b.sep;
                        continue;
                    }
                    ++other;
                    if (other == 1 || b.sep < ov) { oa = b.a; ob = b.b; ov = b.sep; ot = b.t; }
                }
                CHECK(planted_seen == 1,
                      "[6e] the %.2f m fault planted in the formation was found %u times "
                      "in the flown file, expected once", (double)planted_gap, planted_seen);
                CHECK(planted_worst >= planted_gap - 0.01f,
                      "[6e] the planted pair closes to %.4f m in flight where its formation "
                      "puts it %.3f m apart - the flight made the fault worse",
                      (double)planted_worst, (double)planted_gap);
                CHECK(other == 0,
                      "[6e] %u pairs beyond the planted fault break the floor - the worst is "
                      "%u and %u at %.4f m at %.2f s, against a %.2f m minimum: the exported "
                      "show does not fly",
                      other, oa, ob, (double)ov, (double)ot, (double)back.min_distance_m);
                CHECK(w.speed <= back.v_max_ms,
                      "[6e] drone %u flies at %.3f m/s at %.2f s, against a %.2f m/s limit",
                      w.speed_d, (double)w.speed, (double)w.speed_t, (double)back.v_max_ms);
                CHECK(w.accel <= back.a_max_ms2,
                      "[6e] drone %u accelerates at %.3f m/s2 at %.2f s, against a %.2f m/s2 limit",
                      w.accel_d, (double)w.accel, (double)w.accel_t, (double)back.a_max_ms2);
                std::printf("  flown from the file: %.3f m closest, %.3f m/s fastest, "
                            "%.3f m/s2 hardest, over %.1f s at %d Hz x 4\n",
                            (double)w.sep, (double)w.speed, (double)w.accel,
                            (double)dai_show_plan_duration(q), back.fps);

                // And the validator, which measures the PLAN, agrees with the
                // flight of the FILE about how tight the show is. Two paths,
                // one answer: that is what makes either of them believable.
                dai_show_validate_stats st;
                dai_show_validate(q, &back, nullptr, 0, &st);
                CHECK(std::fabs(st.min_distance_m - w.sep) < 0.05f,
                      "[6e] the validator calls the tightest moment %.4f m where flying the "
                      "file finds %.4f m", (double)st.min_distance_m, (double)w.sep);
                dai_show_plan_destroy(q);
            }
            dai_show_destroy(sh);
        }
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
