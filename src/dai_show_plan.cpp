// Stages 3 and 4: crossings are separated, and motion is shaped in time.
//
// Implements, from include/dai_show.h:
//     dai_show_layer
//     dai_show_min_duration
//     dai_show_transition_default
//
// Layering is the stage with the safety claim on it. Two straight legs that
// cross in space are found with the same uniform grid the validator uses -
// never by testing every pair - and separated by lifting one of them onto a
// height layer, or by delaying it, or both. The layers are handed out in a
// fixed order over pairs sorted by (lower index, higher index), so the result
// does not depend on how the work was divided.
//
// What cannot be separated inside the duration allowed is COUNTED and handed
// back, and dai_show_layer returns DAI_ERR_STATE rather than DAI_OK. A tool
// that reports success while leaving two drones on a collision course is worse
// than a tool that does nothing.
//
// The profiles are the other half: v = 0 at both ends is a cubic Bezier, eased
// at one end is the same curve with one control point moved, and the linear
// case is what a director asks for when the figure has to arrive on a beat.
// dai_show_min_duration is the inverse - given the leg and the limits, the
// shortest time that does not break v_max or a_max.

#include "dai_show.h"
#include "dai_show_internal.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

// ---------------------------------------------------------------------------
// The leg, evaluated. dai_show.cpp turns legs into keyframes and therefore has
// to reconstruct EXACTLY the curve this file separated - an approximation in
// one of the two places would mean the plan is not the thing that was proven
// collision free. Hence one implementation, shared through this small internal
// interface rather than through a second copy of the arithmetic.
// ---------------------------------------------------------------------------
namespace daishow {

float    ease_profile(int profile, float u);
void     leg_point(const dai_show_leg *leg, const dai_show_point *a,
                   const dai_show_point *b, float t, dai_show_point *out);
uint32_t leg_key_count(const dai_show_leg *leg, const dai_show_settings *s);

} // namespace daishow

namespace {

// How many straight pieces a detour is cut into when it becomes keyframes.
//
// A detour is a curve; keyframes are lines between samples, and the validator
// differences those lines at the EXPORT TICK. That is the whole reason this is
// not a constant: a corner of the polyline turns the drone in one tick, so a
// key spacing of h seconds shows up as roughly h/dt times the acceleration the
// curve really has. Twelve pieces across a nineteen second leg at ten frames
// per second is a corner every one and a half seconds and an acceleration
// reading fifteen times too high - a detour that flies perfectly and validates
// as a violation. So the pieces are cut at the tick rate the show is exported
// at, one key per tick, and bounded at both ends: below by twelve (a short leg
// still gets a recognisable curve), above by 256 (a five minute transition at
// 60 fps must not turn the plan back into the array of ticks this design
// exists to avoid).
const uint32_t DETOUR_KEYS_MIN = 12;
const uint32_t DETOUR_KEYS_MAX = 256;
const float    DETOUR_KEYS_PER_TICK = 1.0f;

// What a detour has to leave on the table. Even at one key per tick the
// polyline corners slightly harder than the curve, so a lift is only accepted
// when the CURVE stays this far inside the limits - the thing that flies is
// the polyline, and it is the polyline the validator judges.
const float DETOUR_V_MARGIN = 0.90f;
const float DETOUR_A_MARGIN = 0.60f;

// Height layers, and time slots on top of them. 64 x 4 is 256 mutually
// separated ways to route a leg - more than any real transition needs, and a
// hard ceiling is better than a loop that grows until the machine gives up.
const uint32_t MAX_LAYERS = 64;
const uint32_t MAX_SLOTS  = 4;

// How much longer than the transition ONE lifted leg may take. The detour over
// a ninety metre figure is a hundred and sixty metres of extra path, and no
// stretch of the whole transition pays for that without turning a six second
// move into a minute for four hundred drones that never needed it. So the leg
// that climbs gets its own clock and the formation waits for it - which is what
// dai_show.cpp already does, because it holds every arrived drone until the
// last one lands.
const float MAX_LEG_STRETCH = 8.0f;

// The delay ladder: how many steps, and how long a step is as a fraction of the
// transition. A tenth is enough to break a leaning pair apart and small enough
// that the drone is still inside the fleet's own movement rather than arriving
// into a formation everyone else is already standing in.
const uint32_t DELAY_SLOTS = 6;

// How many multiples of the measured deficit a micro step may climb before the
// fleet-wide ladder takes over.
const uint32_t MICRO_STEPS = 8;
const float    DELAY_FRACTION = 0.10f;

// How often the separator looks again. Lifting a leg changes the geometry and
// can create a crossing that was not there before, so the rounds run until the
// picture stops changing - eight is far past what any measured case needed.
const int MAX_ROUNDS = 32;
// ...and how much of that a big fleet may afford. A round is a broadphase over
// every leg, so at ten thousand drones it costs a hundred times what it costs
// at four hundred, and the deep search - micro steps, the route bias, three
// durations - turns a ten second solve into most of a minute. So the SEARCH is
// a function of the fleet size and of nothing else: under two thousand drones
// it is run to the end, above it the separator does the one pass it has always
// done. Same answer on every machine either way, and the report says what was
// left over rather than pretending.
const int SMALL_FLEET  = 2000;
const int ROUNDS_LARGE = 8;

// A pair is in conflict when it comes closer than the minimum distance. The
// epsilon is there so a formation whose neighbours sit at EXACTLY min_distance
// - a grid built to the limit, which is the normal case - is not reported as
// conflicting with itself at the moment it stands still. The number comes from
// src/dai_show_internal.hpp rather than from here: the validator judges by the
// same bound this stage separates to, and a tolerance that lives twice ends up
// with two values and a list of phantom conflicts.
const float SEP_EPS = daishow::SEP_EPS;


inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

// The shape of one profile in the unit interval, and the two constants that
// fall out of it: the peak of s' (how much faster than average the drone gets)
// and the peak of |s''| (what that costs in acceleration).
//
// SMOOTH is the cubic Bezier with both inner control points on the ends, which
// is 3u^2 - 2u^3: v = 0 at u = 0 and u = 1. SMOOTH_LEFT moves the second
// control point to 2/3 so the curve leaves at rest and arrives at full speed
// (2u^2 - u^3), and SMOOTH_RIGHT is its mirror. LINEAR has no acceleration
// anywhere in the interior; its jumps are at the ends, where the drone is
// already stationary in the formation, and that is the trade a director makes
// when the figure has to land on a beat.
void profile_factors(int profile, float *kv, float *ka) {
    switch (profile) {
    case DAI_SHOW_PROFILE_SMOOTH:       *kv = 1.5f;        *ka = 6.0f; break;
    case DAI_SHOW_PROFILE_SMOOTH_LEFT:
    case DAI_SHOW_PROFILE_SMOOTH_RIGHT: *kv = 4.0f / 3.0f; *ka = 4.0f; break;
    default:                            *kv = 1.0f;        *ka = 0.0f; break;
    }
}

// The vertical bump a detour rides on. Zero at both ends, exactly one over the
// cruise, and smooth at the joins - a corner here would be an acceleration
// spike the validator would (rightly) report a moment later.
float bump(float s, float rise_frac) {
    if (rise_frac <= 0.0f) return 0.0f;
    float rf = clampf(rise_frac, 1e-3f, 0.5f);
    float x;
    if (s < rf)            x = s / rf;
    else if (s > 1.0f - rf) x = (1.0f - s) / rf;
    else                    return 1.0f;
    x = clampf(x, 0.0f, 1.0f);
    return x * x * (3.0f - 2.0f * x);
}

inline uint8_t lerp_u8(uint8_t a, uint8_t b, float s) {
    float v = (float)a + ((float)b - (float)a) * s;
    return (uint8_t)clampf(v + 0.5f, 0.0f, 255.0f);
}

struct Vec3 { float x, y, z; };
inline Vec3 sub3(Vec3 a, Vec3 b) { return Vec3{ a.x - b.x, a.y - b.y, a.z - b.z }; }
inline float dot3(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline float len3(Vec3 a) { return std::sqrt(dot3(a, a)); }

// Distance from the origin to the segment p..q. The exact answer for two legs
// that share a time window and a profile: their difference travels a straight
// line, so the closest they ever come is a point-to-segment distance and no
// sampling is needed at all.
float dist_origin_segment(Vec3 p, Vec3 q) {
    Vec3 d = sub3(q, p);
    float dd = dot3(d, d);
    if (dd <= 0.0f) return len3(p);
    float t = clampf(-dot3(p, d) / dd, 0.0f, 1.0f);
    Vec3 c{ p.x + d.x * t, p.y + d.y * t, p.z + d.z * t };
    return len3(c);
}

struct Box {
    float lo[3], hi[3];
};

bool boxes_overlap(const Box &a, const Box &b) {
    for (int k = 0; k < 3; ++k)
        if (a.hi[k] < b.lo[k] || b.hi[k] < a.lo[k]) return false;
    return true;
}

inline Vec3 leg_pos(const dai_show_leg &leg, const dai_show_point &a,
                    const dai_show_point &b, float t) {
    dai_show_point p;
    daishow::leg_point(&leg, &a, &b, t, &p);
    return Vec3{ p.x, p.y, p.z };
}

// How finely a leg is walked when the closed form does not apply, and how many
// slices that walk is grouped into for the broadphase. The samples are shared
// between every pair rather than recomputed inside it: at 10,000 drones the
// fine test runs on hundreds of thousands of pairs per round, and evaluating
// two eased cubics per sample per pair would be most of the stage.
const int SAMPLES = 64;                       // intervals; SAMPLES + 1 positions

// The leg, sampled once on a clock shared by the whole transition, and then
// cut into COARSE slices with a box around each. Two legs that cross the same
// piece of sky but not at the same moment - which is most of them, because
// that is exactly what a delay buys - are thrown out by comparing slice k
// against slice k, sixteen box tests instead of a fine sweep. The box is grown
// by the deviation of the curve from its chord, so a slice never loses a piece
// of its own leg and the broadphase stays conservative.
const int COARSE = 16;                        // slices; SAMPLES must be a multiple

void sample_leg(const dai_show_leg &leg, const dai_show_point &a, const dai_show_point &b,
                float gt0, float gt1, Vec3 *out) {
    for (int i = 0; i <= SAMPLES; ++i)
        out[i] = leg_pos(leg, a, b, gt0 + (gt1 - gt0) * ((float)i / (float)SAMPLES));
}

void slice_boxes(const Vec3 *p, float margin, Box *out) {
    const int per = SAMPLES / COARSE;
    for (int c = 0; c < COARSE; ++c) {
        Box r;
        for (int k = 0; k < 3; ++k) { r.lo[k] = 1e30f; r.hi[k] = -1e30f; }
        float dev = 0.0f;
        for (int j = c * per; j <= (c + 1) * per; ++j) {
            r.lo[0] = std::min(r.lo[0], p[j].x); r.hi[0] = std::max(r.hi[0], p[j].x);
            r.lo[1] = std::min(r.lo[1], p[j].y); r.hi[1] = std::max(r.hi[1], p[j].y);
            r.lo[2] = std::min(r.lo[2], p[j].z); r.hi[2] = std::max(r.hi[2], p[j].z);
            if (j > 0 && j < SAMPLES) {
                Vec3 s2{ p[j + 1].x - 2.0f * p[j].x + p[j - 1].x,
                         p[j + 1].y - 2.0f * p[j].y + p[j - 1].y,
                         p[j + 1].z - 2.0f * p[j].z + p[j - 1].z };
                dev = std::max(dev, 0.125f * len3(s2));
            }
        }
        dev += margin;
        for (int k = 0; k < 3; ++k) { r.lo[k] -= dev; r.hi[k] += dev; }
        out[c] = r;
    }
}

// How close two legs ever come, as a LOWER BOUND - never an optimistic one.
//
// The cheap case is exact: same window, same profile, no detour on either side
// means the difference of the two positions runs along a straight segment, and
// the closest the two drones ever come is a point-to-segment distance.
//
// Otherwise the relative motion is walked over the samples both legs already
// have, and the gap between samples is paid for honestly: between two samples
// the curve leaves its chord by at most an eighth of the local second
// difference, so the chord distance minus that slack is a bound the true
// minimum cannot fall below. Sampling on its own would be a guess; this is a
// proof with a constant in it.
float min_separation(const dai_show_leg &la, const dai_show_point &a0, const dai_show_point &a1,
                     const dai_show_leg &lb, const dai_show_point &b0, const dai_show_point &b1,
                     const Vec3 *pa, const Vec3 *pb, float floor_m) {
    const bool same_window = (la.t_start == lb.t_start) && (la.t_end == lb.t_end) &&
                             (la.profile == lb.profile) &&
                             (la.rise_frac == 0.0f) && (lb.rise_frac == 0.0f);
    if (same_window) {
        Vec3 p{ a0.x - b0.x, a0.y - b0.y, a0.z - b0.z };
        Vec3 q{ a1.x - b1.x, a1.y - b1.y, a1.z - b1.z };
        return dist_origin_segment(p, q);
    }

    Vec3 rel[SAMPLES + 1];
    for (int i = 0; i <= SAMPLES; ++i) rel[i] = sub3(pa[i], pb[i]);
    float best = len3(rel[0]);
    for (int i = 0; i < SAMPLES; ++i) {
        float d = dist_origin_segment(rel[i], rel[i + 1]);
        int im = (i > 0) ? i - 1 : 0;
        int ip = (i + 2 <= SAMPLES) ? i + 2 : SAMPLES;
        Vec3 s1{ rel[i + 1].x - 2.0f * rel[i].x + rel[im].x,
                 rel[i + 1].y - 2.0f * rel[i].y + rel[im].y,
                 rel[i + 1].z - 2.0f * rel[i].z + rel[im].z };
        Vec3 s2{ rel[ip].x - 2.0f * rel[i + 1].x + rel[i].x,
                 rel[ip].y - 2.0f * rel[i + 1].y + rel[i].y,
                 rel[ip].z - 2.0f * rel[i + 1].z + rel[i].z };
        d -= 0.125f * std::max(len3(s1), len3(s2));
        if (d < best) {
            best = d;
            // Already a conflict. How deep it goes is the validator's number,
            // not the separator's, and finishing the sweep to find out costs
            // more than the answer is worth.
            if (best < floor_m) return best;
        }
    }
    return best;
}

// What a leg really costs in speed and in acceleration.
//
// Straight legs have a closed form - the profile says how much faster than the
// average the drone gets, and how hard it turns to do it. A detour has no such
// form, so it is differenced on the curve at a FIXED step count: two machines
// that walk the same 64 steps read the same two numbers, which is what a
// verdict has to be.
void leg_peaks(const dai_show_leg &leg, const dai_show_point &a, const dai_show_point &b,
               float *v_peak, float *a_peak) {
    float T = leg.t_end - leg.t_start;
    float kv, ka;
    profile_factors(leg.profile, &kv, &ka);
    float L = len3(Vec3{ b.x - a.x, b.y - a.y, b.z - a.z });
    if (!(T > 0.0f)) { *v_peak = 1e30f; *a_peak = 1e30f; return; }
    if (leg.rise_frac <= 0.0f) {
        *v_peak = L * kv / T;
        *a_peak = L * ka / (T * T);
        return;
    }
    const int K = 64;
    Vec3 p[K + 1];
    for (int i = 0; i <= K; ++i)
        p[i] = leg_pos(leg, a, b, leg.t_start + T * ((float)i / (float)K));
    float dt = T / (float)K;
    float vmax = 0.0f, amax = 0.0f;
    for (int i = 0; i < K; ++i)
        vmax = std::max(vmax, len3(sub3(p[i + 1], p[i])) / dt);
    for (int i = 1; i < K; ++i) {
        Vec3 acc{ (p[i + 1].x - 2.0f * p[i].x + p[i - 1].x) / (dt * dt),
                  (p[i + 1].y - 2.0f * p[i].y + p[i - 1].y) / (dt * dt),
                  (p[i + 1].z - 2.0f * p[i].z + p[i - 1].z) / (dt * dt) };
        amax = std::max(amax, len3(acc));
    }
    *v_peak = vmax;
    *a_peak = amax;
}

// What the plan loses when the curve becomes a polyline, in metres.
//
// The separator proves a claim about the CURVE it routes; the plan stores that
// curve as the keyframes stage 4 cuts it into, and the validator - like the
// ground station, like the drone - flies the straight lines between them. The
// two differ by the sag of a chord, and a guarantee that ignores it is a
// guarantee about a trajectory nobody flies. That difference is what produced
// a list of conflicts, all of them a couple of centimetres deep, in a
// transition this stage had just declared clean.
//
// The bound is the classical one: a curve deviates from its chord over an
// interval h by at most |f''| h^2 / 8. What |f''| is filled in with matters
// more than it looks: a_max - the ceiling the leg was merely ALLOWED to use -
// makes the reserve three centimetres on a detour that in truth curves a
// hundredth of that, and three centimetres is wider than the margin a formation
// packed at the minimum distance has to give. The separator then declares a
// pair unsolvable because of slack it invented, and no route can ever win. So
// the number used here is the acceleration this leg REALLY has, measured on the
// curve, doubled: the second difference reads the curvature at 64 steps rather
// than at its peak, and twice the measured value is the margin that costs
// nothing anywhere it is not needed. Zero for a straight leg, which the plan
// stores as one key with the leg's own profile and therefore reproduces exactly.
float chord_reserve(const dai_show_leg &leg, const dai_show_point &a,
                    const dai_show_point &b, const dai_show_settings &s) {
    if (!(leg.rise_frac > 0.0f)) return 0.0f;
    uint32_t k = daishow::leg_key_count(&leg, &s);
    if (!k) return 0.0f;
    float h = (leg.t_end - leg.t_start) / (float)k;
    float vp = 0.0f, ap = 0.0f;
    leg_peaks(leg, a, b, &vp, &ap);
    if (s.a_max_ms2 > 0.0f && ap > s.a_max_ms2) ap = s.a_max_ms2;
    return 0.125f * (2.0f * ap) * h * h;
}

// Does this leg stay inside v_max and a_max?
bool leg_within_limits(const dai_show_leg &leg, const dai_show_point &a,
                       const dai_show_point &b, const dai_show_settings &s) {
    float T = leg.t_end - leg.t_start;
    if (!(T > 0.0f)) return false;
    float vmax = 0.0f, amax = 0.0f;
    leg_peaks(leg, a, b, &vmax, &amax);
    const float vm = (leg.rise_frac > 0.0f) ? DETOUR_V_MARGIN : 1.0f;
    const float am = (leg.rise_frac > 0.0f) ? DETOUR_A_MARGIN : 1.0f;
    if (s.v_max_ms  > 0.0f && vmax > s.v_max_ms  * vm) return false;
    if (s.a_max_ms2 > 0.0f && amax > s.a_max_ms2 * am) return false;
    return true;
}

// ---- the broadphase -------------------------------------------------------
//
// The same uniform grid idea the validator uses, with time as a fourth axis.
// Each leg is already cut into COARSE slices; a slice goes into every cell its
// box touches, and only legs that share a cell IN THE SAME SLICE become
// candidates. Two boxes that overlap always meet in at least one cell, so
// nothing can slip through - but a pair that crosses the same piece of sky an
// hour apart never meets, which is exactly what a delay is for.
//
// The numbers this is worth, measured at 10,000 drones on the scaling case:
// whole-leg boxes produce 3.4 million candidate pairs, slice boxes produce a
// hundredth of that, and the layering stage goes from sixteen seconds to a
// fraction of one. Testing all pairs would be 5*10^7, on every round.
struct Entry { uint64_t cell; uint32_t leg; };

bool entry_less(const Entry &a, const Entry &b) {
    if (a.cell != b.cell) return a.cell < b.cell;
    return a.leg < b.leg;
}

void candidate_pairs(const std::vector<Box> &slices, uint32_t n, float min_d,
                     std::vector<uint64_t> &out) {
    out.clear();
    if (n < 2 || slices.empty()) return;

    float lo[3] = { slices[0].lo[0], slices[0].lo[1], slices[0].lo[2] };
    double ext_sum = 0.0;
    for (size_t i = 0; i < slices.size(); ++i)
        for (int k = 0; k < 3; ++k) {
            lo[k] = std::min(lo[k], slices[i].lo[k]);
            ext_sum += (double)(slices[i].hi[k] - slices[i].lo[k]);
        }

    // Cell size: start at the spacing the fleet is packed at and double until
    // the number of (cell, slice) entries is bounded by a small multiple of
    // the fleet. Starting from the LEG length instead - the obvious choice -
    // is what turns this into the bottleneck: legs are long and drones are two
    // metres apart, so a cell the size of a leg holds hundreds of them and the
    // pair loop inside it is quadratic again.
    float cell = 2.0f * min_d;
    if (!(cell > 0.0f)) cell = (float)(ext_sum / (3.0 * (double)slices.size()));
    if (!(cell > 0.0f)) cell = 1.0f;
    const double budget = 6.0 * (double)slices.size() + 64.0;
    for (int attempt = 0; attempt < 24; ++attempt) {
        double est = 0.0;
        for (size_t i = 0; i < slices.size() && est <= budget; ++i) {
            double c = 1.0;
            for (int k = 0; k < 3; ++k)
                c *= std::floor((double)(slices[i].hi[k] - slices[i].lo[k]) / (double)cell) + 1.0;
            est += c;
        }
        if (est <= budget) break;
        cell *= 2.0f;
    }

    // A slice that spans more cells than this in one axis is not indexed. NOT
    // clipped to the first 63 cells, which is what stood here and is exactly
    // the bug tests/droneshow_cases_plan.cpp [3h] was written for: a drone
    // crossing 1.6 km of field at half a metre spacing spans three thousand
    // cells, the range was cut at sixty-three, and everything past the first
    // thirty metres of that flight was invisible to the broadphase. A pair the
    // separator never sees is a collision nobody separates.
    //
    // Such a slice is handed to the narrow phase against the whole fleet in its
    // own time slot instead. That is O(n) for ONE slice, and the cell size above
    // was already doubled until the index fits its budget, so a slice is only
    // wide when it really is an outlier - the traveller, not the fleet. Paying
    // n box tests for it is a hundred microseconds; missing it is a crash.
    const int64_t WIDE_CELLS = 63;
    std::vector<Entry> entries;
    std::vector<Entry> wide;              // (slot, slice) that skipped the grid
    entries.reserve(slices.size() * 2);
    for (size_t i = 0; i < slices.size(); ++i) {
        const uint64_t slot = (uint64_t)(i % (size_t)COARSE);
        int64_t c0[3], c1[3];
        bool too_wide = false;
        for (int k = 0; k < 3; ++k) {
            c0[k] = (int64_t)std::floor((slices[i].lo[k] - lo[k]) / cell);
            c1[k] = (int64_t)std::floor((slices[i].hi[k] - lo[k]) / cell);
            if (c1[k] - c0[k] > WIDE_CELLS) too_wide = true;
        }
        if (too_wide) {
            Entry w; w.cell = slot; w.leg = (uint32_t)(i / (size_t)COARSE);
            wide.push_back(w);
            continue;
        }
        for (int64_t x = c0[0]; x <= c1[0]; ++x)
            for (int64_t y = c0[1]; y <= c1[1]; ++y)
                for (int64_t z = c0[2]; z <= c1[2]; ++z) {
                    // Twenty bits per axis and four for the slice. Two cells a
                    // million apart share a key; that costs one rejected box
                    // test and never a missed pair, because the box test below
                    // is the thing that decides.
                    uint64_t key = ((uint64_t)((uint32_t)(int32_t)x & 0xFFFFFu) << 44) |
                                   ((uint64_t)((uint32_t)(int32_t)y & 0xFFFFFu) << 24) |
                                   ((uint64_t)((uint32_t)(int32_t)z & 0xFFFFFu) <<  4) |
                                   (slot & 0xFu);
                    Entry e; e.cell = key; e.leg = (uint32_t)(i / (size_t)COARSE);
                    entries.push_back(e);
                }
    }
    std::sort(entries.begin(), entries.end(), entry_less);

    for (size_t i = 0; i < entries.size();) {
        size_t j = i;
        while (j < entries.size() && entries[j].cell == entries[i].cell) ++j;
        for (size_t a = i; a < j; ++a)
            for (size_t b = a + 1; b < j; ++b) {
                uint32_t x = entries[a].leg, y = entries[b].leg;
                if (x == y) continue;
                if (x > y) std::swap(x, y);
                uint32_t sl = (uint32_t)(entries[i].cell & 0xFu);
                if (!boxes_overlap(slices[(size_t)x * COARSE + sl],
                                   slices[(size_t)y * COARSE + sl])) continue;
                out.push_back(((uint64_t)x << 32) | (uint64_t)y);
            }
        i = j;
    }

    // The wide slices, against everyone in the same slot. The box test is the
    // same one the grid path ends on, so a pair reaches the narrow phase for
    // the same reason either way, and the loop runs over drone indices in
    // order, so the pair list stays independent of how the grid was built.
    for (size_t w = 0; w < wide.size(); ++w) {
        const uint32_t sl = (uint32_t)wide[w].cell;
        const uint32_t x  = wide[w].leg;
        const Box &bx = slices[(size_t)x * COARSE + sl];
        for (uint32_t y = 0; y < n; ++y) {
            if (y == x) continue;
            if (!boxes_overlap(bx, slices[(size_t)y * COARSE + sl])) continue;
            uint32_t a = x < y ? x : y, b = x < y ? y : x;
            out.push_back(((uint64_t)a << 32) | (uint64_t)b);
        }
    }

    // One pair, once, in a fixed order - the order the layers are then handed
    // out in, and the reason two runs produce the same heights.
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
}

// Greedy colouring in drone order. Not the smallest colouring that exists -
// that problem is NP hard and a show does not need the optimum, it needs the
// same answer twice. Lowest free colour, neighbours in index order, no
// randomisation, no tie break that depends on anything but the index.
void colour_graph(const std::vector<std::vector<uint32_t> > &adj, std::vector<uint32_t> &colour) {
    uint32_t n = (uint32_t)adj.size();
    colour.assign(n, 0);
    std::vector<uint32_t> used;
    for (uint32_t i = 0; i < n; ++i) {
        used.clear();
        for (size_t k = 0; k < adj[i].size(); ++k) {
            uint32_t j = adj[i][k];
            if (j < i) used.push_back(colour[j]);
        }
        std::sort(used.begin(), used.end());
        uint32_t c = 0;
        for (size_t k = 0; k < used.size(); ++k) {
            if (used[k] == c) ++c;
            else if (used[k] > c) break;
        }
        colour[i] = c;
    }
}

struct Config {
    float height_y;   // the absolute cruise height of the detour
    int   slot;       // how many delay steps
    int   routed;     // 0 = stay at the base height, only wait
    int   micro;      // > 0: a step of the height the pair itself asked for
};

// The report, written from whatever the last round left behind. Separate
// because there are two ways out of the loop and both owe the caller the same
// honest numbers.
void fill_stats(dai_show_layer_stats *stats, uint32_t n, const dai_show_leg *legs,
                const std::vector<dai_show_point> &src, const std::vector<dai_show_point> &dst,
                const std::vector<uint32_t> &colour, const std::vector<float> &base_delay,
                float t_start, size_t edge_count, size_t open_count) {
    if (!stats) return;
    stats->crossings_found    = (uint32_t)edge_count;
    stats->unresolved         = (uint32_t)open_count;
    uint32_t heights = 0, delays = 0, layers = 1;
    float extra = 0.0f;
    for (uint32_t i = 0; i < n; ++i) {
        if (legs[i].rise_frac > 0.0f) {
            ++heights;
            extra = std::max(extra, legs[i].layer_y - std::max(src[i].y, dst[i].y));
        }
        if (legs[i].t_start > t_start + base_delay[i] + 1e-6f) ++delays;
        if (colour[i] + 1 > layers) layers = colour[i] + 1;
    }
    stats->resolved_by_height = heights;
    stats->resolved_by_delay  = delays;
    stats->layers_used        = layers;
    stats->max_extra_height_m = extra;
    // solve_ms stays zero on purpose: nothing that produces a result in this
    // library reads a clock. dai_show_solve times the stage from the outside
    // and fills the number in.
    stats->solve_ms = 0.0;
}

} // namespace

// ---------------------------------------------------------------------------
namespace daishow {

float ease_profile(int profile, float u) {
    if (u <= 0.0f) return 0.0f;
    if (u >= 1.0f) return 1.0f;
    switch (profile) {
    case DAI_SHOW_PROFILE_SMOOTH:       return u * u * (3.0f - 2.0f * u);
    case DAI_SHOW_PROFILE_SMOOTH_LEFT:  return u * u * (2.0f - u);
    case DAI_SHOW_PROFILE_SMOOTH_RIGHT: { float w = 1.0f - u; return 1.0f - w * w * (2.0f - w); }
    default:                            return u;
    }
}

void leg_point(const dai_show_leg *leg, const dai_show_point *a,
               const dai_show_point *b, float t, dai_show_point *out) {
    if (!leg || !a || !b || !out) return;
    float T = leg->t_end - leg->t_start;
    float u = (T > 0.0f) ? clampf((t - leg->t_start) / T, 0.0f, 1.0f)
                         : ((t < leg->t_end) ? 0.0f : 1.0f);
    float s = ease_profile(leg->profile, u);
    out->x = a->x + (b->x - a->x) * s;
    out->y = a->y + (b->y - a->y) * s;
    out->z = a->z + (b->z - a->z) * s;
    if (leg->rise_frac > 0.0f) {
        float g = bump(s, leg->rise_frac);
        out->y += g * (leg->layer_y - out->y);
    }
    out->r = lerp_u8(a->r, b->r, s);
    out->g = lerp_u8(a->g, b->g, s);
    out->b = lerp_u8(a->b, b->b, s);
    out->w = lerp_u8(a->w, b->w, s);
}

uint32_t leg_key_count(const dai_show_leg *leg, const dai_show_settings *s) {
    if (!leg) return 0;
    if (!(leg->rise_frac > 0.0f)) return 1u;             // a straight leg is one line
    float fps = (s && s->fps > 0) ? (float)s->fps : 30.0f;
    float T   = leg->t_end - leg->t_start;
    float k   = std::ceil(T * fps * DETOUR_KEYS_PER_TICK);   // keys AFTER the start
    if (!(k > (float)DETOUR_KEYS_MIN)) return DETOUR_KEYS_MIN;
    if (k > (float)DETOUR_KEYS_MAX)    return DETOUR_KEYS_MAX;
    return (uint32_t)k;
}

} // namespace daishow

// ---------------------------------------------------------------------------
extern "C" {

dai_show_transition dai_show_transition_default(void) {
    dai_show_transition t;
    t.duration_s    = 8.0f;
    t.profile       = DAI_SHOW_PROFILE_SMOOTH;
    t.timing        = DAI_SHOW_TIMING_SYNC;
    t.stagger_s     = 0.0f;
    t.assign_method = DAI_SHOW_ASSIGN_AUTO;
    return t;
}

float dai_show_min_duration(float distance_m, int profile, const dai_show_settings *s) {
    if (!s || !(distance_m > 0.0f)) return 0.0f;
    float kv, ka;
    profile_factors(profile, &kv, &ka);
    float t = 0.0f;
    if (s->v_max_ms > 0.0f) t = distance_m * kv / s->v_max_ms;
    if (ka > 0.0f && s->a_max_ms2 > 0.0f) {
        float ta = std::sqrt(distance_m * ka / s->a_max_ms2);
        if (ta > t) t = ta;
    }
    return t;
}

dai_result dai_show_layer(const dai_show_point *from, const dai_show_point *to, uint32_t n,
                          const uint32_t *perm, const dai_show_transition *tr,
                          const dai_show_settings *s, float t_start,
                          dai_show_leg *out_legs, dai_show_layer_stats *stats) {
    if (stats) std::memset(stats, 0, sizeof(*stats));
    if (!from || !to || !perm || !tr || !s || !out_legs || n == 0) return DAI_ERR_INVALID_ARG;
    for (uint32_t i = 0; i < n; ++i)
        if (perm[i] >= n) return DAI_ERR_INVALID_ARG;

    const float asked_duration = (tr->duration_s > 0.0f) ? tr->duration_s : 0.001f;
    const float min_d          = (s->min_distance_m > 0.0f) ? s->min_distance_m : 0.0f;

    // The drone's own two endpoints. Everything below works on these, never on
    // `to` through the permutation again - one indirection, resolved once.
    std::vector<dai_show_point> src(n), dst(n);
    for (uint32_t i = 0; i < n; ++i) { src[i] = from[i]; dst[i] = to[perm[i]]; }

    // The base timing. STAGGERED spreads the departures over stagger_s in
    // drone index order: deterministic by construction, and the order a
    // director can predict when she watches the fleet leave.
    std::vector<float> base_delay(n, 0.0f);
    if (tr->timing == DAI_SHOW_TIMING_STAGGERED && tr->stagger_s > 0.0f && n > 1) {
        for (uint32_t i = 0; i < n; ++i)
            base_delay[i] = tr->stagger_s * ((float)i / (float)(n - 1));
    }

    // Every detour flies clear of BOTH formations - over the top of them or
    // under the bottom - so a routed leg cannot meet a drone that is standing
    // still in either. Both ends of that envelope are worth having: on a
    // ninety metre figure the ceiling is eighty metres above a drone in the
    // middle and the floor is fifteen below it, and the floor route is the one
    // that fits inside v_max.
    float ceil_y = src[0].y, floor_y = src[0].y;
    for (uint32_t i = 0; i < n; ++i) {
        ceil_y  = std::max(ceil_y,  std::max(src[i].y, dst[i].y));
        floor_y = std::min(floor_y, std::min(src[i].y, dst[i].y));
    }
    const float gap = std::max(min_d * 1.5f, 1.0f);

    // The routes a leg can be moved onto, enumerated once. Colour k takes route
    // k - 1, so two drones of different colours are always on different routes,
    // and the mapping does not depend on which drone asked first. A route the
    // geofence forbids is left out of the list rather than silently clamped
    // onto the one below it, which would put two colours at the same height.
    //
    // How hard this transition is searched - see SMALL_FLEET.
    const bool deep = (n <= (uint32_t)SMALL_FLEET);

    std::vector<Config> routes;
    const bool fenced = (s->fence_top_m > 0.0f);
    // The cheapest route of all comes first: leave a moment later, on the same
    // line, at the same speed. Most of what the broadphase reports on a real
    // figure is not an X of two paths, it is two neighbours a whisker over the
    // minimum whose straight lines lean together in the middle - and a tenth of
    // the transition of delay bends the RELATIVE path away from the origin
    // without costing a metre of climb or a metre per second.
    // The cheapest route of all is a STEP as high as the pair that complained
    // needs and no higher. Most of what the broadphase reports on a real figure
    // is not an X of two paths: it is two neighbours a whisker over the minimum
    // whose straight lines lean together in the middle, one metre ninety of a
    // two metre floor. Eleven centimetres of height puts them right, and eleven
    // centimetres is a detour nobody else in a fleet spaced metres apart ever
    // notices. The fleet-wide ladder further down costs three metres a rung and
    // lands the drone in somebody else's lane; this step is measured from the
    // deficit the previous round reported.
    for (uint32_t step = 1; deep && step <= MICRO_STEPS; ++step) {
        Config c; c.height_y = 0.0f; c.slot = 0; c.routed = 1; c.micro = (int)step;
        routes.push_back(c);
    }
    for (uint32_t slot = 1; slot <= DELAY_SLOTS; ++slot) {
        Config c; c.height_y = 0.0f; c.slot = (int)slot; c.routed = 0; c.micro = 0;
        routes.push_back(c);
    }
    for (uint32_t slot = 0; slot < MAX_SLOTS; ++slot) {
        // Under the fleet first, because it is nearer: the drone that has to
        // move sits somewhere inside the figure, and the floor is always closer
        // to it than the ceiling. Ground clearance is a limit, not a
        // preference, so a rung below it is left out rather than clamped onto
        // the one above - two colours at the same height are not two routes.
        for (uint32_t k = 0; k < MAX_LAYERS; ++k) {
            float ly = floor_y - gap * (float)(k + 1);
            if (ly < s->min_ground_m + gap * 0.5f) break;
            Config c; c.height_y = ly; c.slot = (int)slot; c.routed = 1; c.micro = 0;
            routes.push_back(c);
        }
        for (uint32_t k = 0; k < MAX_LAYERS; ++k) {
            float ly = ceil_y + gap * (float)(k + 1);
            if (fenced && ly > s->fence_top_m) break;
            Config c; c.height_y = ly; c.slot = (int)slot; c.routed = 1; c.micro = 0;
            routes.push_back(c);
        }
    }
    std::vector<uint32_t>               colour(n, 0);
    std::vector<float>                  reserve(n, 0.0f);  // chord sag, per leg
    std::vector<Box>                    slices;
    std::vector<Vec3>                   samples;
    std::vector<uint64_t>               pairs;
    std::vector<uint64_t>               edges;   // sorted, accumulated over rounds
    std::vector<std::vector<uint32_t> > adj(n);
    // What the previous round left: the legs as they were, which of them moved
    // since, and what was in conflict then. A pair of legs that both stood
    // still cannot have changed its mind about the other.
    std::vector<dai_show_leg> was(n);
    std::vector<uint8_t>      moved(n, 1);
    std::vector<uint64_t>     was_found;
    std::vector<uint64_t>     found;
    std::vector<uint32_t>     bias(n, 0);
    // How high the last round says this drone would have had to be to clear
    // whatever it came too close to: the deficit, turned into a height, which
    // is what a micro step is built from.
    std::vector<float>        lift_need(n, 0.0f);

    // The way out when the sky is full and the clock is not.
    //
    // A transition whose legs already run at v_max has no room for a detour:
    // every lift is refused, every route falls back to the base one, and the
    // separator used to hand back the whole crossing set as "unresolved" - a
    // correct report of a solvable problem, which is the worst kind. What was
    // missing is that the DURATION is a parameter too. Stretching it lowers the
    // speed of every leg and buys exactly the headroom the detour needs, and
    // dai_show_min_duration says how much is needed for the detour that was
    // refused: length of the leg plus twice the height it would have to climb.
    //
    // The stretch is bounded (three attempts, at most triple the asked
    // duration) because a separator that silently turns a six second move into
    // a minute has solved a different show than the one on the storyboard. What
    // was really used is in the legs - t_end - t_start - and dai_show_solve
    // reads it back off them and says so.
    const int   MAX_ATTEMPTS = 5;
    const float MAX_STRETCH  = 5.0f;
    float       duration     = asked_duration;
    size_t      edge_total   = 0;
    size_t      best_open    = (size_t)-1;
    size_t      best_edges   = 0;
    std::vector<dai_show_leg> best_legs(n);
    std::vector<uint32_t>     best_colour(n, 0);
    std::vector<uint64_t>     best_found;

    const int  round_budget = deep ? MAX_ROUNDS : ROUNDS_LARGE;
    const int  attempts     = deep ? MAX_ATTEMPTS : 1;

    // One route, laid on one drone: what the leg becomes, and whether the
    // limits allow it. Both the colouring above and the rescue below route a
    // leg, and they have to route it the SAME way - two copies of this
    // arithmetic would be two different sets of legs proven by one broadphase.
    auto route_leg = [&](uint32_t i, uint32_t r, const dai_show_leg &base,
                         float step, dai_show_leg *out) -> bool {
        dai_show_leg lifted = base;
        if (routes[r].slot > 0) {
            lifted.t_start += step * (float)routes[r].slot;
            lifted.t_end   += step * (float)routes[r].slot;
        }
        // The rise is stretched over more of the leg until it fits - a higher
        // layer needs a gentler ramp, and the gentlest one this shape allows is
        // rising for the whole first half. Only when even that is too much is
        // the lift refused, and then the pair stays in the report rather than
        // in the sky.
        const float ramps[3] = { 0.34f, 0.42f, 0.5f };
        if (!routes[r].routed) {
            *out = lifted;
            return leg_within_limits(lifted, src[i], dst[i], *s);
        }
        if (routes[r].micro > 0) {
            // Odd steps go up, even steps go down, and each pair of steps is
            // three quarters of a minimum distance further out than the last.
            // Both halves matter: the drone this one is about to touch may be
            // ABOVE it, and then climbing by the deficit is the one move that
            // makes it worse. And the steps have to differ by more than the
            // deficit, or two drones of the same pair take steps one and two,
            // rise eleven centimetres apart, and arrive at the same problem.
            int   k    = routes[r].micro - 1;
            float mag  = std::max(lift_need[i], min_d * 0.25f)
                       + (float)(k / 2) * min_d * 0.75f;
            float sign = (k % 2 == 0) ? 1.0f : -1.0f;
            lifted.layer_y = ((sign > 0.0f) ? std::max(src[i].y, dst[i].y)
                                            : std::min(src[i].y, dst[i].y))
                           + sign * mag;
            if (lifted.layer_y < s->min_ground_m + min_d) {   // the ground wins
                *out = base;
                return false;
            }
        } else {
            lifted.layer_y = routes[r].height_y;
        }
        bool took = false;
        for (int k = 0; k < 3 && !took; ++k) {
            lifted.rise_frac = ramps[k];
            took = leg_within_limits(lifted, src[i], dst[i], *s);
        }
        // Still refused: it is not the shape that is wrong, it is the clock. A
        // climb to the fleet ceiling and back is the leg plus twice the height,
        // and dai_show_min_duration says what that costs; the leg takes that
        // long and the formation waits for it. Bounded, and only for the drones
        // that have to move - the alternative measured here was stretching the
        // whole transition, which slowed four hundred drones to fix six.
        if (!took) {
            float L     = len3(sub3(Vec3{ dst[i].x, dst[i].y, dst[i].z },
                                    Vec3{ src[i].x, src[i].y, src[i].z }));
            float climb = std::fabs(lifted.layer_y - std::max(src[i].y, dst[i].y));
            float dive  = std::fabs(lifted.layer_y - std::min(src[i].y, dst[i].y));
            climb = std::max(climb, dive);
            float need = dai_show_min_duration((L + 2.0f * climb) / DETOUR_V_MARGIN,
                                               lifted.profile, s);
            float cap  = duration * MAX_LEG_STRETCH;
            if (need > cap) need = cap;
            if (need > duration) {
                lifted.t_end = lifted.t_start + need;
                for (int k = 0; k < 3 && !took; ++k) {
                    lifted.rise_frac = ramps[k];
                    took = leg_within_limits(lifted, src[i], dst[i], *s);
                }
            }
        }
        *out = lifted;
        return took;
    };

    for (int attempt = 0; ; ++attempt) {
    const float delay_step = DELAY_FRACTION * duration;
    colour.assign(n, 0);
    moved.assign(n, 1);
    bias.assign(n, 0);
    lift_need.assign(n, 0.0f);
    edges.clear();
    was_found.clear();
    found.clear();
    edge_total = 0;

    // One round: lay the current colouring down as legs, then look again. The
    // legs are rebuilt from scratch every time, so a route that was tried and
    // rejected leaves nothing behind.
    for (int round = 0; ; ++round) {
        for (uint32_t i = 0; i < n; ++i) {
            dai_show_leg &g = out_legs[i];
            g.t_start   = t_start + base_delay[i];
            g.t_end     = g.t_start + duration;
            g.layer_y   = 0.0f;
            g.rise_frac = 0.0f;
            g.profile   = tr->profile;
            g.pad       = 0;
            if (colour[i] == 0 && bias[i] == 0) continue;
            // The colour picks a route; the bias says how far along the list to
            // start looking. Without the bias the loop is a fixed point: the
            // same conflict graph is coloured the same way, so a pair that is
            // still too close after both drones were routed gets exactly the
            // same two routes on the next round and the separator gives up on a
            // problem it never really searched. The bias is what turns that
            // into a search - the higher-numbered drone of a pair that survived
            // its routing steps one route further along, in index order, so the
            // walk is the same on every machine.
            uint32_t r = colour[i] + bias[i];
            if (r == 0) continue;                             // the base route
            // Past the end of the list the walk starts over rather than
            // falling off it: the cheap routes at the front are worth trying
            // again once the OTHER drone of the pair has moved, and a bias that
            // runs off the end is a drone that stopped searching while the
            // report still says it has a problem.
            r = (r - 1) % (uint32_t)routes.size();
            const bool base_ok = leg_within_limits(g, src[i], dst[i], *s);
            dai_show_leg lifted;
            // A route that breaks v_max or a_max is not a fix, it is a
            // different violation - unless the leg was already breaking them
            // standing still, and then the routed one is no worse and at least
            // tries. That is the only case a refused route is flown.
            if (route_leg(i, r, g, delay_step, &lifted) || !base_ok) g = lifted;
        }

        float gt0 = out_legs[0].t_start, gt1 = out_legs[0].t_end;
        for (uint32_t i = 0; i < n; ++i) {
            gt0 = std::min(gt0, out_legs[i].t_start);
            gt1 = std::max(gt1, out_legs[i].t_end);
        }
        slices.resize((size_t)n * COARSE);
        samples.resize((size_t)n * (SAMPLES + 1));
        for (uint32_t i = 0; i < n; ++i) {
            Vec3 *p = &samples[(size_t)i * (SAMPLES + 1)];
            sample_leg(out_legs[i], src[i], dst[i], gt0, gt1, p);
            reserve[i] = chord_reserve(out_legs[i], src[i], dst[i], *s);
            // The broadphase box grows with the reserve too, or a pair that is
            // only a conflict once the chord sag is counted never reaches the
            // narrow phase that would count it.
            slice_boxes(p, min_d * 0.5f + reserve[i], &slices[(size_t)i * COARSE]);
            moved[i] = (round == 0 || std::memcmp(&was[i], &out_legs[i], sizeof(dai_show_leg)) != 0);
            was[i] = out_legs[i];
        }
        candidate_pairs(slices, n, min_d, pairs);

        // Two legs that did not move cannot have changed their minds about
        // each other, so the round after a recolouring only pays for the legs
        // the recolouring actually touched. Worth an order of magnitude on a
        // dense figure, where nine tenths of the fleet keeps its route.
        found.clear();
        found.reserve(pairs.size() / 4 + 4);
        for (size_t k = 0; k < pairs.size(); ++k) {
            uint32_t a = (uint32_t)(pairs[k] >> 32), b = (uint32_t)(pairs[k] & 0xFFFFFFFFu);
            if (!moved[a] && !moved[b]) {
                if (std::binary_search(was_found.begin(), was_found.end(), pairs[k]))
                    found.push_back(pairs[k]);
                continue;
            }
            // The bar this pair has to clear is the minimum distance PLUS what
            // the two legs lose when the plan cuts them into keyframes. Two
            // drones that pass at exactly min_distance on the curve pass closer
            // than that on the polyline, and the validator - which reads the
            // polyline - would report the pair the separator had just signed
            // off. One number, computed the same way on both sides.
            const float bar = min_d - SEP_EPS + reserve[a] + reserve[b];
            float sep = min_separation(out_legs[a], src[a], dst[a],
                                       out_legs[b], src[b], dst[b],
                                       &samples[(size_t)a * (SAMPLES + 1)],
                                       &samples[(size_t)b * (SAMPLES + 1)],
                                       bar);
            if (sep < bar) {
                found.push_back(pairs[k]);
                float clear = (sep > 0.0f)
                            ? std::sqrt(std::max(0.0f, bar * bar - sep * sep)) : bar;
                clear += min_d * 0.1f;                     // and a little over
                lift_need[a] = std::max(lift_need[a], clear);
                lift_need[b] = std::max(lift_need[b], clear);
            }
        }
        was_found = found;

#ifdef DAI_SHOW_PLAN_DEBUG
        if (n > 300) {
            std::fprintf(stderr, "  attempt %d round %d: open=%zu dur=%.2f\n",
                         attempt, round, found.size(), duration);
            for (size_t k = 0; k < found.size() && k < 6; ++k) {
                uint32_t a = (uint32_t)(found[k] >> 32), b = (uint32_t)(found[k] & 0xFFFFFFFFu);
                std::fprintf(stderr, "    %u(c=%u,bias=%u,rise=%.2f,y=%.1f,t=%.1f) "
                             "%u(c=%u,bias=%u,rise=%.2f,y=%.1f,t=%.1f) need=%.2f/%.2f\n",
                             a, colour[a], bias[a], out_legs[a].rise_frac, out_legs[a].layer_y,
                             out_legs[a].t_start,
                             b, colour[b], bias[b], out_legs[b].rise_frac, out_legs[b].layer_y,
                             out_legs[b].t_start, lift_need[a], lift_need[b]);
            }
        }
#endif
        if (found.empty() || round >= round_budget - 1) {
            edge_total = std::max(edges.size(), found.size());
            break;
        }

        // A pair that is STILL too close although both its drones were routed
        // gets one of them moved along: the routing that was tried is known not
        // to work, so trying it again is the definition of a stuck loop.
        for (size_t k = 0; deep && k < found.size(); ++k) {
            if (!std::binary_search(edges.begin(), edges.end(), found[k])) continue;
            // ONE of them - the higher-numbered first, so the choice does not
            // depend on the order the pairs came out of the grid, and
            // measurably better than moving both: two drones stepping aside in
            // the same direction at the same moment have not stepped aside at
            // all. Once that one has been round the whole list, the other takes
            // over: sometimes the drone that has to give way is the one that
            // was standing still, and a pair where only one side ever moves is
            // a pair that never gets out of its own way.
            uint32_t hi = (uint32_t)(found[k] & 0xFFFFFFFFu);
            uint32_t lo = (uint32_t)(found[k] >> 32);
            if (bias[hi] + 1u < (uint32_t)routes.size()) ++bias[hi];
            else                                         ++bias[lo];
        }

        // The crossing set only ever grows. A pair that was separated by a lift
        // must stay separated when the next round hands out colours again, so
        // its edge stays in the graph.
        edges.insert(edges.end(), found.begin(), found.end());
        std::sort(edges.begin(), edges.end());
        edges.erase(std::unique(edges.begin(), edges.end()), edges.end());

        for (uint32_t i = 0; i < n; ++i) adj[i].clear();
        for (size_t k = 0; k < edges.size(); ++k) {
            uint32_t a = (uint32_t)(edges[k] >> 32), b = (uint32_t)(edges[k] & 0xFFFFFFFFu);
            adj[a].push_back(b);
            adj[b].push_back(a);
        }
        // A drone whose route was refused keeps its colour - refusing it a
        // second time would loop - but its neighbours must not be told the
        // conflict is gone, which is what the accumulated edge set does.
        colour_graph(adj, colour);
    }

    // Keep the best attempt, not the last one. A longer duration gives every
    // leg more room, but it also re-colours the whole transition from scratch,
    // and a colouring that untangles nine pairs out of ten can come back with a
    // different tenth. Handing the caller whichever attempt happened to run
    // last would make a bigger search budget produce a worse show, which is not
    // a trade anybody would take.
    if (found.size() < best_open) {
        best_open   = found.size();
        best_edges  = edge_total;
        best_legs.assign(out_legs, out_legs + n);
        best_colour = colour;
        best_found  = found;
    }
    if (found.empty()) break;

    // What the refused detours would have needed. The height is the lowest
    // layer above both formations, the path is the leg plus the climb and the
    // descent, and dai_show_min_duration turns that into the seconds v_max and
    // a_max demand. Taking the maximum over the pairs that are still open is
    // the smallest stretch that gives every one of them a route.
    float want = duration;
    for (size_t k = 0; k < found.size(); ++k) {
        const uint32_t two[2] = { (uint32_t)(found[k] >> 32), (uint32_t)(found[k] & 0xFFFFFFFFu) };
        for (int e = 0; e < 2; ++e) {
            uint32_t i = two[e];
            float L = len3(Vec3{ dst[i].x - src[i].x, dst[i].y - src[i].y, dst[i].z - src[i].z });
            float climb = std::max(src[i].y, dst[i].y) - (floor_y - gap);
            if (climb < 0.0f) climb = 0.0f;
            float need = dai_show_min_duration(L + 2.0f * climb, tr->profile, s);
            if (need > want) want = need;
        }
    }
    // Never less than a fifth more than the current try: a stretch that rounds
    // to nothing is a round that repeats itself.
    float next = std::max(want, duration * 1.2f);
    if (next > asked_duration * MAX_STRETCH) next = asked_duration * MAX_STRETCH;
    if (attempt + 1 >= attempts || next <= duration * 1.001f) break;
    duration = next;
    }

    if (best_open < found.size()) {                  // an earlier attempt did better
        std::memcpy(out_legs, best_legs.data(), (size_t)n * sizeof(dai_show_leg));
        colour     = best_colour;
        edge_total = best_edges;
        found      = best_found;
    }
#ifdef DAI_SHOW_PLAN_DEBUG
    for (size_t k = 0; k < found.size(); ++k) {
        uint32_t a = (uint32_t)(found[k] >> 32), b = (uint32_t)(found[k] & 0xFFFFFFFFu);
        Vec3 s0{src[a].x - src[b].x, src[a].y - src[b].y, src[a].z - src[b].z};
        Vec3 d0{dst[a].x - dst[b].x, dst[a].y - dst[b].y, dst[a].z - dst[b].z};
        std::fprintf(stderr, "OPEN %u/%u src_gap=%.3f dst_gap=%.3f "
                     "legA[t=%.2f..%.2f rise=%.2f y=%.1f] legB[t=%.2f..%.2f rise=%.2f y=%.1f] "
                     "lenA=%.1f lenB=%.1f dur=%.2f\n", a, b, len3(s0), len3(d0),
                     out_legs[a].t_start, out_legs[a].t_end, out_legs[a].rise_frac, out_legs[a].layer_y,
                     out_legs[b].t_start, out_legs[b].t_end, out_legs[b].rise_frac, out_legs[b].layer_y,
                     len3(sub3(Vec3{dst[a].x,dst[a].y,dst[a].z}, Vec3{src[a].x,src[a].y,src[a].z})),
                     len3(sub3(Vec3{dst[b].x,dst[b].y,dst[b].z}, Vec3{src[b].x,src[b].y,src[b].z})),
                     duration);
        std::fprintf(stderr, "  PTS a src %.4f %.4f %.4f dst %.4f %.4f %.4f\n"
                             "  PTS b src %.4f %.4f %.4f dst %.4f %.4f %.4f\n",
                     src[a].x, src[a].y, src[a].z, dst[a].x, dst[a].y, dst[a].z,
                     src[b].x, src[b].y, src[b].z, dst[b].x, dst[b].y, dst[b].z);
    }
#endif
    fill_stats(stats, n, out_legs, src, dst, colour, base_delay, t_start,
               edge_total, std::min(best_open, found.size()));
    return (std::min(best_open, found.size()) == 0) ? DAI_OK : DAI_ERR_STATE;
}

} // extern "C"
