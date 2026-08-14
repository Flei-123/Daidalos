// Stages 3 and 4 under test: constructed crossings, and limits that hold.
//
// Implements show_cases_plan() - see tests/droneshow_cases.hpp.
//
//   [3a] THE CONSTRUCTED CROSSING: two drones swapping places on a straight
//        line pass through the same point at the same instant. After layering,
//        sample the legs densely and assert the separation never drops below
//        min_distance. Then the same for four drones crossing at one point,
//        and for a formation rotated 180 degrees, where every path crosses
//        every other.
//   [3b] the layering reports what it could not fix: an impossible transition
//        (two formations one metre apart with a 5 m minimum) returns
//        DAI_ERR_STATE and a non-zero unresolved count. Silence here would be
//        the worst failure this program has.
//   [3c] SMOOTH really has v = 0 at both ends, and LINEAR really does not.
//   [3d] v_max and a_max hold across the whole leg for every profile, measured
//        by differencing the sampled positions - and dai_show_min_duration is
//        the boundary: one percent under it breaks a limit, one percent over
//        it does not.
//   [3e] STAGGERED spreads the departures over stagger_s and every drone still
//        arrives before the transition ends.
//   [3f] the plan is keyframes: a 10,000 drone show reports a byte count far
//        below the materialised alternative, and sampling at a keyframe time
//        returns that keyframe's position exactly.
#include "droneshow_cases.hpp"

#include <cmath>
#include <cstring>
#include <vector>

// White box, on purpose. The curve stage 3 proved collision free is the curve
// stage 4 turns into keyframes, and it lives in src/dai_show_plan.cpp. This
// file evaluates that same function rather than a second copy of the
// arithmetic: a copy would agree with a bug instead of catching it.
namespace daishow {
void leg_point(const dai_show_leg *leg, const dai_show_point *a,
               const dai_show_point *b, float t, dai_show_point *out);
}

namespace {

dai_show_settings plan_settings(float min_d, float v_max, float a_max) {
    dai_show_settings s = dai_show_settings_default();
    s.min_distance_m = min_d;
    s.v_max_ms       = v_max;
    s.a_max_ms2      = a_max;
    s.seed           = 0x0F1E2D3C4B5A6978ull;
    return s;
}

float dist(const dai_show_point &a, const dai_show_point &b) {
    float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// The brute force answer to "did the separator actually work": every pair, at
// a thousand instants, measured. Nothing clever, nothing shared with the code
// under test except the definition of where a drone is.
float sweep_min_separation(const std::vector<dai_show_point> &from,
                           const std::vector<dai_show_point> &to,
                           const std::vector<uint32_t> &perm,
                           const std::vector<dai_show_leg> &legs,
                           int steps, float *out_time, uint32_t *out_a, uint32_t *out_b) {
    uint32_t n = (uint32_t)legs.size();
    float t0 = legs[0].t_start, t1 = legs[0].t_end;
    for (uint32_t i = 0; i < n; ++i) {
        t0 = std::fmin(t0, legs[i].t_start);
        t1 = std::fmax(t1, legs[i].t_end);
    }
    std::vector<dai_show_point> now(n);
    float best = 1e30f;
    if (out_time) *out_time = 0.0f;
    if (out_a) *out_a = 0;
    if (out_b) *out_b = 0;
    for (int k = 0; k <= steps; ++k) {
        float t = t0 + (t1 - t0) * ((float)k / (float)steps);
        for (uint32_t i = 0; i < n; ++i)
            daishow::leg_point(&legs[i], &from[i], &to[perm[i]], t, &now[i]);
        for (uint32_t i = 0; i < n; ++i)
            for (uint32_t j = i + 1; j < n; ++j) {
                float d = dist(now[i], now[j]);
                if (d < best) {
                    best = d;
                    if (out_time) *out_time = t;
                    if (out_a) *out_a = i;
                    if (out_b) *out_b = j;
                }
            }
    }
    return best;
}

// One constructed crossing, run end to end: layer it, then measure it.
void crossing_case(const char *what, const std::vector<dai_show_point> &from,
                   const std::vector<dai_show_point> &to,
                   const std::vector<uint32_t> &perm,
                   const dai_show_settings &s, float duration) {
    uint32_t n = (uint32_t)from.size();
    dai_show_transition tr = dai_show_transition_default();
    tr.duration_s = duration;

    std::vector<dai_show_leg> legs(n);
    dai_show_layer_stats st;
    dai_result r = dai_show_layer(from.data(), to.data(), n, perm.data(), &tr, &s,
                                  10.0f, legs.data(), &st);

    CHECK(r == DAI_OK, "%s: layering did not resolve the crossings (%u left)",
          what, (unsigned)st.unresolved);
    CHECK(st.crossings_found > 0, "%s: the separator found no crossing at all", what);

    float t_hit = 0.0f;
    uint32_t ha = 0, hb = 0;
    float sep = sweep_min_separation(from, to, perm, legs, 1500, &t_hit, &ha, &hb);
    CHECK(sep >= s.min_distance_m - 1e-3f,
          "%s: %u and %u come %.3f m apart at t = %.2f s, the floor is %.3f m",
          what, (unsigned)ha, (unsigned)hb, (double)sep, (double)t_hit,
          (double)s.min_distance_m);

    // Every leg still starts where the drone is and ends where it was sent.
    dai_show_point p;
    int ok_ends = 1;
    for (uint32_t i = 0; i < n; ++i) {
        daishow::leg_point(&legs[i], &from[i], &to[perm[i]], legs[i].t_start, &p);
        if (dist(p, from[i]) > 1e-3f) ok_ends = 0;
        daishow::leg_point(&legs[i], &from[i], &to[perm[i]], legs[i].t_end, &p);
        if (dist(p, to[perm[i]]) > 1e-3f) ok_ends = 0;
    }
    CHECK(ok_ends, "%s: a detour does not begin or end on its formation point", what);
}

// Speed and acceleration of one leg, by differencing the curve. The interior
// only: at the very ends the leg is clamped, and differencing across a clamp
// measures the clamp rather than the motion.
// The step count is not an accident: a second difference divides by dt^2, and
// at four thousand steps that divisor is small enough that single precision
// rounding in the positions shows up as half a metre per second squared of
// pure noise. Five hundred steps resolve the peak of a cubic to well under a
// percent and leave the arithmetic room to breathe.
void measure_leg(const dai_show_leg &leg, const dai_show_point &a, const dai_show_point &b,
                 float *out_v, float *out_acc) {
    const int K = 500;
    double T = (double)leg.t_end - (double)leg.t_start;
    double dt = T / (double)K;
    std::vector<dai_show_point> p((size_t)K + 1);
    for (int i = 0; i <= K; ++i)
        daishow::leg_point(&leg, &a, &b, (float)((double)leg.t_start + dt * (double)i),
                           &p[(size_t)i]);
    double vmax = 0.0, amax = 0.0;
    for (int i = 1; i < K; ++i) {
        double vx = ((double)p[i + 1].x - p[i - 1].x) / (2.0 * dt);
        double vy = ((double)p[i + 1].y - p[i - 1].y) / (2.0 * dt);
        double vz = ((double)p[i + 1].z - p[i - 1].z) / (2.0 * dt);
        vmax = std::fmax(vmax, std::sqrt(vx * vx + vy * vy + vz * vz));
        double ax = ((double)p[i + 1].x - 2.0 * p[i].x + p[i - 1].x) / (dt * dt);
        double ay = ((double)p[i + 1].y - 2.0 * p[i].y + p[i - 1].y) / (dt * dt);
        double az = ((double)p[i + 1].z - 2.0 * p[i].z + p[i - 1].z) / (dt * dt);
        amax = std::fmax(amax, std::sqrt(ax * ax + ay * ay + az * az));
    }
    *out_v = (float)vmax;
    *out_acc = (float)amax;
}

dai_show_leg straight_leg(float t0, float t1, int profile) {
    dai_show_leg g;
    g.t_start = t0; g.t_end = t1; g.layer_y = 0.0f; g.rise_frac = 0.0f;
    g.profile = profile; g.pad = 0;
    return g;
}

dai_show_point pt(float x, float y, float z, uint8_t r, uint8_t g, uint8_t b) {
    dai_show_point p; p.x = x; p.y = y; p.z = z; p.r = r; p.g = g; p.b = b; p.w = 0;
    return p;
}

// ---- [3a] -----------------------------------------------------------------

void case_crossings(void) {
    show_section("[3a] constructed crossings, separated and then measured");

    // Two drones swapping places: without layering they meet exactly in the
    // middle, at exactly half time. The simplest collision there is.
    {
        dai_show_settings s = plan_settings(2.0f, 12.0f, 8.0f);
        std::vector<dai_show_point> from{ pt(-10, 50, 0, 255, 0, 0), pt(10, 50, 0, 0, 0, 255) };
        std::vector<dai_show_point> to  { pt( 10, 50, 0, 255, 0, 0), pt(-10, 50, 0, 0, 0, 255) };
        std::vector<uint32_t> perm{ 0, 1 };
        crossing_case("two drones swapping", from, to, perm, s, 12.0f);
    }

    // Four drones through one point, from the four compass directions.
    {
        dai_show_settings s = plan_settings(2.0f, 12.0f, 8.0f);
        std::vector<dai_show_point> from{ pt(-10, 50,   0, 255, 0, 0), pt(10, 50,   0, 0, 255, 0),
                                          pt(  0, 50, -10, 0, 0, 255), pt( 0, 50,  10, 255, 255, 0) };
        std::vector<dai_show_point> to  { pt( 10, 50,   0, 255, 0, 0), pt(-10, 50,  0, 0, 255, 0),
                                          pt(  0, 50,  10, 0, 0, 255), pt( 0, 50, -10, 255, 255, 0) };
        std::vector<uint32_t> perm{ 0, 1, 2, 3 };
        crossing_case("four drones through one point", from, to, perm, s, 12.0f);
    }

    // A formation turned through 180 degrees about its centre. Every drone
    // flies to the point opposite, which means every path runs through the
    // centre and arrives there at the same instant: the conflict graph is
    // complete and the separator needs one layer per drone.
    {
        dai_show_settings s = plan_settings(2.0f, 30.0f, 15.0f);
        std::vector<dai_show_point> from(9);
        show_grid_formation(from.data(), 9, 4.0f, dai_vec3{ 0.0f, 50.0f, 0.0f });
        std::vector<uint32_t> perm(9);
        for (uint32_t i = 0; i < 9; ++i) perm[i] = 8 - i;   // the point opposite
        crossing_case("a formation rotated 180 degrees", from, from, perm, s, 20.0f);
    }
}

// ---- [3b] -----------------------------------------------------------------

void case_impossible(void) {
    show_section("[3b] what cannot be separated is reported, not hidden");

    // Four drones a metre apart, told to keep five. There is no routing that
    // fixes this - the formations themselves break the rule - and the only
    // acceptable behaviour is to say so.
    dai_show_settings s = plan_settings(5.0f, 10.0f, 5.0f);
    std::vector<dai_show_point> from(4), to(4);
    show_grid_formation(from.data(), 4, 1.0f, dai_vec3{ 0.0f, 50.0f, 0.0f });
    show_grid_formation(to.data(),   4, 1.0f, dai_vec3{ 6.0f, 50.0f, 0.0f });
    std::vector<uint32_t> perm{ 0, 1, 2, 3 };

    dai_show_transition tr = dai_show_transition_default();
    tr.duration_s = 10.0f;
    std::vector<dai_show_leg> legs(4);
    dai_show_layer_stats st;
    dai_result r = dai_show_layer(from.data(), to.data(), 4, perm.data(), &tr, &s,
                                  0.0f, legs.data(), &st);

    CHECK(r == DAI_ERR_STATE, "an impossible transition was reported as clean");
    CHECK(st.unresolved > 0, "an impossible transition left unresolved == 0");

    // The legs are written anyway: a director cannot fix a near miss she is
    // not allowed to look at.
    int written = 1;
    for (uint32_t i = 0; i < 4; ++i)
        if (!(legs[i].t_end > legs[i].t_start)) written = 0;
    CHECK(written, "the failed transition wrote no legs to look at");

    // And the honest case next to it: the same fleet with room to breathe
    // comes back clean, so [3b] is not simply a function that always fails.
    dai_show_settings ok = plan_settings(2.0f, 10.0f, 5.0f);
    show_grid_formation(from.data(), 4, 5.0f, dai_vec3{ 0.0f, 50.0f, 0.0f });
    show_grid_formation(to.data(),   4, 5.0f, dai_vec3{ 0.0f, 60.0f, 0.0f });
    dai_show_layer_stats st2;
    dai_result r2 = dai_show_layer(from.data(), to.data(), 4, perm.data(), &tr, &ok,
                                   0.0f, legs.data(), &st2);
    CHECK(r2 == DAI_OK, "a transition with room to spare was reported as conflicting");
    CHECK(st2.unresolved == 0, "a clean transition reported %u unresolved pairs",
          (unsigned)st2.unresolved);
}

// ---- [3c] -----------------------------------------------------------------

void case_profiles(void) {
    show_section("[3c] the profiles are the shapes they claim to be");

    dai_show_point a = pt(0, 50, 0, 255, 0, 0), b = pt(30, 50, 0, 0, 255, 0);
    const float T = 10.0f;
    const float h = 0.002f;

    struct Expect { int profile; const char *name; int v0_zero; int v1_zero; };
    const Expect table[4] = {
        { DAI_SHOW_PROFILE_LINEAR,       "linear",       0, 0 },
        { DAI_SHOW_PROFILE_SMOOTH,       "smooth",       1, 1 },
        { DAI_SHOW_PROFILE_SMOOTH_LEFT,  "smooth-left",  1, 0 },
        { DAI_SHOW_PROFILE_SMOOTH_RIGHT, "smooth-right", 0, 1 },
    };
    for (int i = 0; i < 4; ++i) {
        dai_show_leg g = straight_leg(0.0f, T, table[i].profile);
        dai_show_point p0, p1, q0, q1;
        daishow::leg_point(&g, &a, &b, 0.0f, &p0);
        daishow::leg_point(&g, &a, &b, h,    &p1);
        daishow::leg_point(&g, &a, &b, T - h, &q0);
        daishow::leg_point(&g, &a, &b, T,     &q1);
        float v_start = dist(p0, p1) / h;
        float v_end   = dist(q0, q1) / h;
        float v_mean  = dist(a, b) / T;

        if (table[i].v0_zero)
            CHECK(v_start < v_mean * 0.02f, "%s leaves at %.4f m/s, it must leave at rest",
                  table[i].name, (double)v_start);
        else
            CHECK(v_start > v_mean * 0.5f, "%s leaves at %.4f m/s, it must not ease out",
                  table[i].name, (double)v_start);
        if (table[i].v1_zero)
            CHECK(v_end < v_mean * 0.02f, "%s arrives at %.4f m/s, it must arrive at rest",
                  table[i].name, (double)v_end);
        else
            CHECK(v_end > v_mean * 0.5f, "%s arrives at %.4f m/s, it must not ease in",
                  table[i].name, (double)v_end);

        // Whatever the shape, the ends are the formation points and the colour
        // has arrived with the drone.
        CHECK(dist(p0, a) < 1e-4f && dist(q1, b) < 1e-4f,
              "%s does not connect its two formation points", table[i].name);
        CHECK(q1.r == b.r && q1.g == b.g && q1.b == b.b,
              "%s arrives in the wrong colour", table[i].name);
    }
}

// ---- [3d] -----------------------------------------------------------------

void case_limits(void) {
    show_section("[3d] v_max and a_max hold, and min_duration is the boundary");

    dai_show_settings s = plan_settings(2.0f, 6.0f, 3.0f);
    const float L = 40.0f;
    dai_show_point a = pt(0, 50, 0, 10, 20, 30), b = pt(L, 50, 0, 200, 100, 50);

    const int profiles[4] = { DAI_SHOW_PROFILE_LINEAR, DAI_SHOW_PROFILE_SMOOTH,
                              DAI_SHOW_PROFILE_SMOOTH_LEFT, DAI_SHOW_PROFILE_SMOOTH_RIGHT };
    const char *names[4] = { "linear", "smooth", "smooth-left", "smooth-right" };

    for (int i = 0; i < 4; ++i) {
        float tmin = dai_show_min_duration(L, profiles[i], &s);
        CHECK(tmin > 0.0f, "%s: min_duration returned %.3f for a 40 m leg",
              names[i], (double)tmin);

        float v, acc;
        dai_show_leg over = straight_leg(0.0f, tmin * 1.01f, profiles[i]);
        measure_leg(over, a, b, &v, &acc);
        CHECK(v <= s.v_max_ms, "%s at 101%% of min_duration flies %.3f m/s, the limit is %.3f",
              names[i], (double)v, (double)s.v_max_ms);
        if (profiles[i] != DAI_SHOW_PROFILE_LINEAR)
            CHECK(acc <= s.a_max_ms2, "%s at 101%% of min_duration pulls %.3f m/s2, the limit is %.3f",
                  names[i], (double)acc, (double)s.a_max_ms2);

        dai_show_leg under = straight_leg(0.0f, tmin * 0.99f, profiles[i]);
        measure_leg(under, a, b, &v, &acc);
        int broke = (v > s.v_max_ms) || (profiles[i] != DAI_SHOW_PROFILE_LINEAR && acc > s.a_max_ms2);
        CHECK(broke, "%s at 99%% of min_duration breaks nothing (%.3f m/s, %.3f m/s2) - "
                     "min_duration is not the boundary it claims to be",
              names[i], (double)v, (double)acc);
    }

    // A longer leg needs longer, and a zero length one needs nothing.
    CHECK(dai_show_min_duration(80.0f, DAI_SHOW_PROFILE_SMOOTH, &s) >
          dai_show_min_duration(40.0f, DAI_SHOW_PROFILE_SMOOTH, &s),
          "min_duration does not grow with the distance");
    CHECK(dai_show_min_duration(0.0f, DAI_SHOW_PROFILE_SMOOTH, &s) == 0.0f,
          "a leg of zero length needs time");

    // And the same check on a real layered transition: the detours the
    // separator adds are legs too, and they obey the same two numbers.
    dai_show_settings ls = plan_settings(2.0f, 30.0f, 15.0f);
    std::vector<dai_show_point> from(9);
    show_grid_formation(from.data(), 9, 4.0f, dai_vec3{ 0.0f, 50.0f, 0.0f });
    std::vector<uint32_t> perm(9);
    for (uint32_t i = 0; i < 9; ++i) perm[i] = 8 - i;
    dai_show_transition tr = dai_show_transition_default();
    tr.duration_s = 20.0f;
    std::vector<dai_show_leg> legs(9);
    dai_show_layer_stats st;
    dai_show_layer(from.data(), from.data(), 9, perm.data(), &tr, &ls, 0.0f, legs.data(), &st);
    float worst_v = 0.0f, worst_a = 0.0f;
    for (uint32_t i = 0; i < 9; ++i) {
        if (legs[i].t_end <= legs[i].t_start) continue;
        float v, acc;
        measure_leg(legs[i], from[i], from[perm[i]], &v, &acc);
        worst_v = std::fmax(worst_v, v);
        worst_a = std::fmax(worst_a, acc);
    }
    CHECK(worst_v <= ls.v_max_ms, "a layered detour flies %.3f m/s, the limit is %.3f",
          (double)worst_v, (double)ls.v_max_ms);
    CHECK(worst_a <= ls.a_max_ms2, "a layered detour pulls %.3f m/s2, the limit is %.3f",
          (double)worst_a, (double)ls.a_max_ms2);
    CHECK(st.max_extra_height_m > 0.0f, "nine crossing paths were separated without a single layer");
}

// ---- [3e] -----------------------------------------------------------------

void case_stagger(void) {
    show_section("[3e] staggered departures, and everyone home on time");

    // A pure translation upwards: no path crosses another, so nothing the
    // separator does can be confused with what the timing mode does.
    const uint32_t n = 25;
    dai_show_settings s = plan_settings(2.0f, 12.0f, 6.0f);
    std::vector<dai_show_point> from(n), to(n);
    show_grid_formation(from.data(), n, 4.0f, dai_vec3{ 0.0f, 50.0f, 0.0f });
    show_grid_formation(to.data(),   n, 4.0f, dai_vec3{ 0.0f, 70.0f, 0.0f });
    std::vector<uint32_t> perm(n);
    for (uint32_t i = 0; i < n; ++i) perm[i] = i;

    dai_show_transition tr = dai_show_transition_default();
    tr.duration_s = 12.0f;
    tr.timing     = DAI_SHOW_TIMING_STAGGERED;
    tr.stagger_s  = 6.0f;

    const float t0 = 20.0f;
    std::vector<dai_show_leg> legs(n);
    dai_show_layer_stats st;
    dai_result r = dai_show_layer(from.data(), to.data(), n, perm.data(), &tr, &s,
                                  t0, legs.data(), &st);
    CHECK(r == DAI_OK, "a pure translation was reported as conflicting");

    float first = legs[0].t_start, last = legs[0].t_start, end = legs[0].t_end;
    int monotonic = 1;
    for (uint32_t i = 0; i < n; ++i) {
        first = std::fmin(first, legs[i].t_start);
        last  = std::fmax(last,  legs[i].t_start);
        end   = std::fmax(end,   legs[i].t_end);
        if (i > 0 && legs[i].t_start < legs[i - 1].t_start - 1e-6f) monotonic = 0;
        if (std::fabs((legs[i].t_end - legs[i].t_start) - tr.duration_s) > 1e-3f) monotonic = 0;
    }
    CHECK(std::fabs(first - t0) < 1e-4f, "the first drone leaves at %.4f, not at %.4f",
          (double)first, (double)t0);
    CHECK(std::fabs((last - first) - tr.stagger_s) < 1e-3f,
          "the departures spread over %.4f s, stagger_s is %.4f",
          (double)(last - first), (double)tr.stagger_s);
    CHECK(monotonic, "the stagger is not a fixed function of the drone index");
    CHECK(end <= t0 + tr.duration_s + tr.stagger_s + 1e-3f,
          "the last drone arrives at %.3f, after the transition ends at %.3f",
          (double)end, (double)(t0 + tr.duration_s + tr.stagger_s));

    // SYNC is the other half: one departure, one arrival, for everybody.
    tr.timing = DAI_SHOW_TIMING_SYNC;
    dai_show_layer(from.data(), to.data(), n, perm.data(), &tr, &s, t0, legs.data(), &st);
    int together = 1;
    for (uint32_t i = 0; i < n; ++i)
        if (legs[i].t_start != legs[0].t_start || legs[i].t_end != legs[0].t_end) together = 0;
    CHECK(together, "SYNC did not leave and land the fleet together");
}

// ---- [3f] -----------------------------------------------------------------

void case_keyframes(void) {
    show_section("[3f] the plan is keyframes, and it stays keyframes");

    // A 10,000 drone show through eight keys each - the shape of a twenty
    // formation storyboard. The point is the byte count next to the one this
    // design exists to avoid: 10,000 drones x 30,000 ticks x 12 bytes.
    const uint32_t n = 10000, per = 8;
    std::vector<uint32_t> counts(n, per);
    std::vector<dai_show_key> keys((size_t)n * per);
    for (uint32_t d = 0; d < n; ++d) {
        for (uint32_t k = 0; k < per; ++k) {
            dai_show_key &key = keys[(size_t)d * per + k];
            key.t = (float)k * 15.0f;
            key.p = pt((float)(d % 100) * 3.0f + (float)k,
                       50.0f + (float)k * 2.0f,
                       (float)(d / 100) * 3.0f,
                       (uint8_t)(d % 256), (uint8_t)(k * 30u), 128);
            key.profile = (k % 2) ? DAI_SHOW_PROFILE_SMOOTH : DAI_SHOW_PROFILE_LINEAR;
        }
    }
    dai_show_plan *p = dai_show_plan_from_keys(n, counts.data(), keys.data());
    CHECK(p != nullptr, "a 10,000 drone plan could not be built");
    if (!p) return;

    CHECK(dai_show_plan_drone_count(p) == n, "the plan lost drones: %u of %u",
          (unsigned)dai_show_plan_drone_count(p), (unsigned)n);
    CHECK(dai_show_plan_keyframe_count(p, 4321) == per, "a drone has %u keys, not %u",
          (unsigned)dai_show_plan_keyframe_count(p, 4321), (unsigned)per);
    CHECK(dai_show_plan_duration(p) == (float)(per - 1) * 15.0f,
          "the plan is %.2f s long, the keys end at %.2f",
          (double)dai_show_plan_duration(p), (double)((per - 1) * 15.0f));

    size_t bytes      = dai_show_plan_bytes(p);
    size_t ticks_way  = (size_t)n * 30000u * 12u;
    CHECK(bytes < ticks_way / 1000, "the plan takes %.1f MB, materialised ticks would be %.1f MB - "
                                    "that is not a keyframe container",
          (double)bytes / 1048576.0, (double)ticks_way / 1048576.0);
    std::printf("  plan for %u drones x %u keys: %.2f MB (materialised ticks: %.0f MB)\n",
                (unsigned)n, (unsigned)per, (double)bytes / 1048576.0,
                (double)ticks_way / 1048576.0);

    // Sampling AT a keyframe returns that keyframe, exactly - not nearly.
    int exact = 1;
    for (uint32_t k = 0; k < per; ++k) {
        dai_show_key key;
        dai_show_plan_key_at(p, 777, k, &key);
        dai_show_point q;
        dai_show_plan_sample(p, 777, key.t, &q);
        if (q.x != key.p.x || q.y != key.p.y || q.z != key.p.z) exact = 0;
        if (q.r != key.p.r || q.g != key.p.g || q.b != key.p.b) exact = 0;
    }
    CHECK(exact, "sampling at a keyframe time does not return that keyframe");

    // Outside the show it clamps, which is what a viewport scrubbed past the
    // end has to draw.
    dai_show_point before, after, first, last;
    dai_show_plan_sample(p, 12, -100.0f, &before);
    dai_show_plan_sample(p, 12, 1e6f, &after);
    dai_show_key k0, kl;
    dai_show_plan_key_at(p, 12, 0, &k0);
    dai_show_plan_key_at(p, 12, per - 1, &kl);
    first = k0.p; last = kl.p;
    CHECK(before.x == first.x && before.y == first.y && before.z == first.z,
          "scrubbing before the show does not clamp to the first formation");
    CHECK(after.x == last.x && after.y == last.y && after.z == last.z,
          "scrubbing past the end does not clamp to the last formation");

    // Between two keys the drone is between them, and the whole fleet samples
    // to the same thing one at a time as it does in one call.
    std::vector<dai_show_point> all(n);
    dai_show_plan_sample_all(p, 22.5f, all.data());
    int agree = 1;
    for (uint32_t d = 0; d < n; d += 977) {
        dai_show_point one;
        dai_show_plan_sample(p, d, 22.5f, &one);
        if (std::memcmp(&one, &all[d], sizeof(one)) != 0) agree = 0;
    }
    CHECK(agree, "sample_all and sample disagree about where the fleet is");

    dai_show_point mid;
    dai_show_plan_sample(p, 5, 7.5f, &mid);
    dai_show_key ka, kb;
    dai_show_plan_key_at(p, 5, 0, &ka);
    dai_show_plan_key_at(p, 5, 1, &kb);
    CHECK(mid.x > std::fmin(ka.p.x, kb.p.x) && mid.x < std::fmax(ka.p.x, kb.p.x),
          "halfway between two keys the drone is not between them");

    // A plan whose keys arrive out of order is still readable: it is sorted,
    // not silently sampled backwards.
    dai_show_key shuffled[3];
    shuffled[0].t = 4.0f; shuffled[0].p = pt(4, 0, 0, 0, 0, 0); shuffled[0].profile = 0;
    shuffled[1].t = 0.0f; shuffled[1].p = pt(0, 0, 0, 0, 0, 0); shuffled[1].profile = 0;
    shuffled[2].t = 2.0f; shuffled[2].p = pt(2, 0, 0, 0, 0, 0); shuffled[2].profile = 0;
    uint32_t one_count = 3;
    dai_show_plan *sp = dai_show_plan_from_keys(1, &one_count, shuffled);
    dai_show_point sq;
    dai_show_plan_sample(sp, 0, 2.0f, &sq);
    CHECK(sq.x == 2.0f, "keys handed over out of order sample to %.3f, not 2.0", (double)sq.x);
    dai_show_plan_destroy(sp);

    dai_show_plan_destroy(p);
}

// ---------------------------------------------------------------------------
// [3g] the whole document, solved: what the separator promised is what the
// validator measures.
//
// The two stages look at the same show through different windows - one at the
// curve it routes, the other at the keyframes the plan stores, sampled at the
// export rate - and the gap between those windows is the only place a promise
// can be lost. So the show here is built with ONE deliberate fault in it, a
// pair of drones planted 0.4 m apart inside a formation, and the assertion is
// not "no conflicts" but the sharper one: the conflicts the validator finds
// are the fault that was planted and nothing else. A single extra pair would
// mean the layering signed off on something the plan does not fly.
void case_solved_show_is_clean(void) {
    show_section("[3g] a solved show reports the planted conflict, and only that");

    dai_show_settings s = plan_settings(2.0f, 8.0f, 4.0f);
    s.drone_count = 256;
    s.fps         = 20;
    dai_show *sh = dai_show_create(&s);
    CHECK(sh != nullptr, "the document could not be created");
    if (!sh) return;

    // Three sparse formations, a translation and a rotation apart: enough
    // crossing for the separator to have work, sparse enough that it can win.
    const uint32_t n = s.drone_count;
    std::vector<dai_show_point> a(n), b(n), c(n);
    show_grid_formation(a.data(), n, 5.0f, dai_vec3{ 0.0f, 60.0f, 0.0f });
    show_grid_formation(b.data(), n, 5.0f, dai_vec3{ 0.0f, 60.0f, 0.0f });
    show_grid_formation(c.data(), n, 5.0f, dai_vec3{ 40.0f, 90.0f, 0.0f });
    // b is a on its head: every path crosses the middle.
    for (uint32_t i = 0; i < n; ++i) {
        b[i].x = -b[i].x;
        b[i].z = -b[i].z;
    }
    // The planted fault, in the last formation so it is a formation conflict
    // and not a transition one: two drones that stand 0.4 m apart.
    const uint32_t BAD_A = 100, BAD_B = 101;
    c[BAD_B] = c[BAD_A];
    c[BAD_B].x += 0.4f;

    dai_show_formation_add(sh, "start", "test://grid", a.data(), n, 3.0f);
    dai_show_formation_add(sh, "flip",  "test://grid", b.data(), n, 3.0f);
    dai_show_formation_add(sh, "fault", "test://grid", c.data(), n, 3.0f);

    dai_show_solve(sh, nullptr, 0);
    dai_show_validate_show(sh);

    uint32_t cn = dai_show_conflict_count(sh);
    uint32_t planted = 0, other = 0;
    float worst_other = 0.0f;
    uint32_t other_a = 0, other_b = 0;
    float other_t = 0.0f;
    for (uint32_t i = 0; i < cn; ++i) {
        dai_show_conflict k;
        if (!dai_show_conflict_at(sh, i, &k)) continue;
        if (k.kind != DAI_SHOW_CONFLICT_DISTANCE) continue;
        int is_planted = (k.a == BAD_A && k.b == BAD_B) || (k.a == BAD_B && k.b == BAD_A);
        if (is_planted) { ++planted; continue; }
        ++other;
        if (k.limit - k.value > worst_other) {
            worst_other = k.limit - k.value;
            other_a = k.a; other_b = k.b; other_t = k.time_s;
        }
    }
    CHECK(planted > 0, "the planted 0.40 m pair was not reported at all");
    CHECK(other == 0,
          "%u distance conflicts beyond the planted pair - worst is %u+%u at %.2fs, "
          "%.3f m inside the limit: the separator proved a curve the plan does not fly",
          other, other_a, other_b, (double)other_t, (double)worst_other);

    dai_show_destroy(sh);
}


// ---------------------------------------------------------------------------
// [3h] the leg that is longer than the grid.
//
// The separator indexes each leg into a uniform grid, and a grid has a limit
// on how many cells one entry may span. The tempting way to respect that limit
// is to cut the range short - which quietly deletes the far end of the leg
// from the broadphase, and a leg that crosses the whole field is then invisible
// to everything it flies through. The construction here is that exact case: one
// drone travels 1.6 km past sixty-four drones parked on its line. Every one of
// those pairs is a real crossing, so the separator has to report all of them,
// not the fraction that happened to fall inside the first sixty-three cells.
void case_long_leg(void) {
    show_section("[3h] a leg across the whole field is not lost by the broadphase");

    // Half a metre apart, which is what puts the grid cell at a metre and makes
    // a kilometre-long leg span a thousand of them - a micro-drone show indoors
    // has these numbers, and so does any show whose field is large next to its
    // spacing.
    dai_show_settings s = plan_settings(0.5f, 60.0f, 5.0f);

    const uint32_t PARKED = 64;
    const uint32_t n = PARKED + 1;
    std::vector<dai_show_point> from(n), to(n);
    std::vector<uint32_t>       perm(n);
    for (uint32_t i = 0; i < n; ++i) perm[i] = i;

    // The traveller, and the line of drones it has to get past.
    from[0] = pt(-800.0f, 50.0f, 0.0f, 255, 255, 255);
    to[0]   = pt( 800.0f, 50.0f, 0.0f, 255, 255, 255);
    // The line sits in the middle third of the route, where the traveller is
    // already at cruise: a drone that is still climbing out of its formation is
    // a different test, and [3a] is where that one lives.
    for (uint32_t i = 1; i <= PARKED; ++i) {
        float x = -200.0f + 400.0f * (float)(i - 1) / (float)(PARKED - 1);
        from[i] = pt(x, 50.0f, 0.0f, 80, 160, 255);
        to[i]   = from[i];                     // parked: same point, both ends
    }

    dai_show_transition tr = dai_show_transition_default();
    tr.duration_s = 60.0f;

    std::vector<dai_show_leg> legs(n);
    dai_show_layer_stats st;
    dai_result r = dai_show_layer(from.data(), to.data(), n, perm.data(), &tr, &s,
                                  0.0f, legs.data(), &st);

    // Every parked drone is on the traveller's line, so every one of them is a
    // crossing. Finding fewer means legs left the broadphase somewhere between
    // the first cell and the last.
    CHECK(st.crossings_found >= PARKED,
          "the separator found %u crossings on a line of %u - the far end of the "
          "long leg is missing from the broadphase",
          (unsigned)st.crossings_found, (unsigned)PARKED);
    CHECK(r == DAI_OK, "the long leg was not separated (%u pairs left)",
          (unsigned)st.unresolved);

    float t_hit = 0.0f;
    uint32_t ha = 0, hb = 0;
    float sep = sweep_min_separation(from, to, perm, legs, 4000, &t_hit, &ha, &hb);
    CHECK(sep >= s.min_distance_m - 1e-3f,
          "the traveller passes %u at %.3f m (t = %.2f s), the floor is %.3f m",
          (unsigned)(ha == 0 ? hb : ha), (double)sep, (double)t_hit,
          (double)s.min_distance_m);
}

} // namespace

int show_cases_plan(void) {
    int before = g_show_fail;
    case_crossings();
    case_impossible();
    case_profiles();
    case_limits();
    case_stagger();
    case_keyframes();
    case_solved_show_is_clean();
    case_long_leg();
    return g_show_fail - before;
}
