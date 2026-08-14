// Stage 5: the whole timeline, checked, without ever squaring the fleet.
//
// Implements, from include/dai_show.h:
//     dai_show_validate
//
// Two numbers decide the design. 10,000 drones is 5*10^7 pairs; a five minute
// show at 25 fps is 7,500 ticks. Multiplied, that is a number no laptop will
// finish, so the pairwise test goes through a uniform grid whose cell edge is
// the minimum distance - a drone can only be too close to something in its own
// cell or the 26 around it, and the grid is rebuilt per tick from positions
// that were never stored.
//
// The second number is memory: the fleet exists for one tick at a time,
// sampled out of the keyframes, so the check streams and its peak is two
// frames of positions plus the grid.
//
// The distance rule is NOT decided at the sample points. A fleet checked
// twenty-five times a second moves a third of a metre between two looks, and
// two drones can pass through each other in that gap - so between two ticks the
// pair is treated as what it is, two moving points, and the closest they come
// anywhere inside the interval is computed: the difference of the two straight
// pieces sweeps a parallelogram, the distance from the origin to it is a lower
// bound on their separation, and an interval that comes near the limit is
// halved until that bound and the measured distance agree to within SEP_EPS.
// Where a keyframe falls inside the interval the piece is cut there first, so
// each piece really is straight. A verdict this file gives is a verdict about
// the whole timeline, not about the instants it happened to look at.
//
// Speed and acceleration are finite differences over the same stream, sampled
// at s->fps - which is also what makes a deliberately broken plan detectable:
// the test builds one and this file has to find it.
//
// Conflicts come out sorted by (time, a, b). Not because it is tidy: an
// unsorted list built by several threads is a different list every run, and
// this program's answers have to be reproducible.

#include "dai_show.h"
#include "dai_show_internal.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace {

// One tick's worth of grid. The cell edge is the minimum distance, so a pair
// closer than that cannot be more than one cell apart in any axis - 27 cells
// is the whole search, whatever the fleet size. The table is rebuilt per tick
// into the same arrays; nothing here grows with the length of the show.
struct Grid {
    std::vector<int32_t>  head;    // hash bucket -> first drone, -1 = empty
    std::vector<int32_t>  next;    // drone -> next drone in the same bucket
    std::vector<int32_t>  cell;    // drone -> cx, cy, cz (3 per drone)
    uint32_t              mask = 0;

    void reset(uint32_t n) {
        uint32_t size = 16;
        while (size < n * 2u) size <<= 1;
        mask = size - 1;
        head.assign(size, -1);
        next.assign(n, -1);
        cell.assign((size_t)n * 3u, 0);
    }
    size_t bytes() const {
        return head.size() * sizeof(int32_t) + next.size() * sizeof(int32_t) +
               cell.size() * sizeof(int32_t);
    }
};

// A hash that mixes all three axes. Multiplicative with odd constants, because
// the alternative - packing coordinates into bit fields - buckets a whole
// vertical column together the moment a figure is taller than the field is
// wide, which is every figure.
inline uint32_t cell_hash(int32_t x, int32_t y, int32_t z, uint32_t mask) {
    uint32_t h = (uint32_t)x * 0x8DA6B343u ^ (uint32_t)y * 0xD8163841u ^
                 (uint32_t)z * 0xCB1AB31Fu;
    h ^= h >> 15;
    return h & mask;
}

inline int32_t cell_of(float v, float inv_cell) {
    return (int32_t)std::floor(v * inv_cell);
}

// An episode: one rule broken by one drone (or one pair) over a run of
// consecutive ticks. Reporting every tick of a ten second overlap would bury
// the operator in five hundred copies of one problem, so the run is collapsed
// to its WORST instant - the moment a director has to look at - and emitted
// when it ends. The list stays a list of problems rather than a list of ticks.
struct Episode {
    float    worst_value;
    float    worst_time;
    float    limit;
    uint32_t last_tick;
};

inline uint64_t episode_key(uint32_t a, uint32_t b, int kind) {
    return ((uint64_t)a << 34) | ((uint64_t)b << 4) | (uint64_t)(kind & 15);
}

// Distance, fence and ground get worse as the number falls; speed and
// acceleration get worse as it rises. One comparison, told which way is down.
inline bool worse(int kind, float value, float previous) {
    if (kind == DAI_SHOW_CONFLICT_VMAX || kind == DAI_SHOW_CONFLICT_AMAX)
        return value > previous;
    return value < previous;
}

struct Recorder {
    std::unordered_map<uint64_t, Episode> live;
    std::vector<dai_show_conflict>        done;
    uint32_t                              total = 0;

    void hit(uint32_t a, uint32_t b, int kind, float value, float limit,
             float t, uint32_t tick) {
        uint64_t k = episode_key(a, b, kind);
        auto it = live.find(k);
        if (it == live.end()) {
            Episode e; e.worst_value = value; e.worst_time = t; e.limit = limit;
            e.last_tick = tick;
            live.emplace(k, e);
            return;
        }
        if (worse(kind, value, it->second.worst_value)) {
            it->second.worst_value = value;
            it->second.worst_time  = t;
        }
        it->second.last_tick = tick;
    }

    void emit(uint64_t key, const Episode &e) {
        dai_show_conflict c;
        c.time_s = e.worst_time;
        c.a      = (uint32_t)((key >> 34) & 0x3FFFFFFFu);
        c.b      = (uint32_t)((key >> 4)  & 0x3FFFFFFFu);
        c.kind   = (int)(key & 15u);
        c.value  = e.worst_value;
        c.limit  = e.limit;
        done.push_back(c);
        ++total;
    }

    // Everything that was not touched this tick has ended. Iteration order of
    // the map does not leak into the answer: the finished list is sorted once,
    // at the end, on (time, a, b, kind).
    void close_stale(uint32_t tick) {
        for (auto it = live.begin(); it != live.end(); ) {
            if (it->second.last_tick != tick) { emit(it->first, it->second); it = live.erase(it); }
            else ++it;
        }
    }
    void close_all() {
        for (auto &kv : live) emit(kv.first, kv.second);
        live.clear();
    }
    size_t bytes() const {
        return live.size() * (sizeof(Episode) + sizeof(uint64_t) + 3 * sizeof(void *)) +
               done.capacity() * sizeof(dai_show_conflict);
    }
};


// ---- the swept pair test --------------------------------------------------
//
// Everything below answers one question: over the interval between two ticks,
// how close do drones i and j actually come? Not "how close were they when we
// looked", which is the question a sampled check answers and the reason a
// sampled check can sign off a plan in which two drones swap places.

struct Vec3 { float x, y, z; };
inline Vec3 vsub(Vec3 a, Vec3 b) { return Vec3{ a.x - b.x, a.y - b.y, a.z - b.z }; }
inline Vec3 vadd(Vec3 a, Vec3 b) { return Vec3{ a.x + b.x, a.y + b.y, a.z + b.z }; }
inline float vdot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline float vlen(Vec3 a) { return std::sqrt(vdot(a, a)); }
inline Vec3 vof(const dai_show_point &p) { return Vec3{ p.x, p.y, p.z }; }
inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

// Distance from the origin to the segment base + u*e, u in [0, 1].
float dist_origin_segment(Vec3 base, Vec3 e) {
    float ee = vdot(e, e);
    if (!(ee > 0.0f)) return vlen(base);
    float u = clampf(-vdot(base, e) / ee, 0.0f, 1.0f);
    return vlen(Vec3{ base.x + e.x * u, base.y + e.y * u, base.z + e.z * u });
}

// Distance from the origin to the parallelogram c + u*e1 + v*e2, u,v in [0, 1].
//
// This is where the guarantee comes from. Over one straight piece drone i is
// somewhere on a segment and drone j is somewhere on another; nothing is
// assumed about WHERE on it either of them is at a given instant, because the
// two legs may be eased differently. The set of differences the pair can take
// is exactly this parallelogram, so its distance to the origin is a separation
// neither drone can undercut - whatever the timing does.
float dist_origin_parallelogram(Vec3 c, Vec3 e1, Vec3 e2) {
    const float A = vdot(e1, e1), B = vdot(e1, e2), C = vdot(e2, e2);
    const float D = vdot(e1, c),  E = vdot(e2, c);
    const float det = A * C - B * B;
    if (det > 1e-12f) {
        float u = (B * E - C * D) / det;
        float v = (B * D - A * E) / det;
        if (u >= 0.0f && u <= 1.0f && v >= 0.0f && v <= 1.0f)
            return vlen(Vec3{ c.x + e1.x * u + e2.x * v,
                              c.y + e1.y * u + e2.y * v,
                              c.z + e1.z * u + e2.z * v });
    }
    // Flat, degenerate or the interior minimum lies outside the unit square:
    // then the closest point is on an edge, and four clamped 1D solves is the
    // exact answer rather than an approximation of it.
    float best = dist_origin_segment(c, e1);
    best = std::min(best, dist_origin_segment(vadd(c, e2), e1));
    best = std::min(best, dist_origin_segment(c, e2));
    best = std::min(best, dist_origin_segment(vadd(c, e1), e2));
    return best;
}

// A separation, with its error bars: `lo` is proven - the pair never comes
// closer than this - and `hi` is measured, a distance the pair really does
// take at some instant. The verdict uses `lo`, the "tightest moment in the
// show" number uses `hi`, and neither is ever the other.
struct Sep { float lo, hi; };

// One straight piece of the interval, refined until the two agree.
//
// The parallelogram bound is pessimistic by roughly the distance the pair
// travels inside the piece, which is why a piece that comes near the limit is
// halved: the two drones are sampled at the midpoint out of the plan, and the
// bound of each half is tighter by a factor of two. Six levels take a third of
// a metre down to five millimetres. Only pieces that are near the limit are
// refined at all - the rest answer at the first bound and cost two dot
// products.
Sep swept_piece(const dai_show_plan *p, uint32_t i, uint32_t j,
                float t0, float t1, Vec3 a0, Vec3 a1, Vec3 b0, Vec3 b1,
                float slack, float floor_m, int depth) {
    Vec3 c  = vsub(a0, b0);
    Vec3 e1 = vsub(a1, a0);
    Vec3 e2 = vsub(b0, b1);
    Sep r;
    r.lo = dist_origin_parallelogram(c, e1, e2) - slack;
    r.hi = std::min(vlen(c), vlen(vsub(a1, b1)));
    if (r.lo >= floor_m || depth <= 0 || r.hi - r.lo <= daishow::SEP_EPS) return r;

    const float tm = 0.5f * (t0 + t1);
    dai_show_point am, bm;
    dai_show_plan_sample(p, i, tm, &am);
    dai_show_plan_sample(p, j, tm, &bm);
    Sep l = swept_piece(p, i, j, t0, tm, a0, vof(am), b0, vof(bm), slack, floor_m, depth - 1);
    Sep h = swept_piece(p, i, j, tm, t1, vof(am), a1, vof(bm), b1, slack, floor_m, depth - 1);
    r.lo = std::min(l.lo, h.lo);
    r.hi = std::min(l.hi, h.hi);
    return r;
}

// How deep the halving is allowed to go. Six levels below a 25 fps tick is a
// piece of 0.6 ms; a fixed cap keeps the cost of a dense formation bounded and
// the answer identical on every machine.
const int SWEPT_DEPTH = 6;

// How many keyframe times a single interval may be cut at. A tick that
// contains more turning points than this is a tick whose drones are being
// re-routed faster than the check looks, and the bow of the polyline is added
// as slack instead - conservative, so such an interval over-reports rather
// than under-reports.
const int MAX_SPLITS = 6;

// The whole interval for one pair: cut at every keyframe that falls inside it,
// so each piece is two straight lines and the bound above is honest, then the
// worst piece decides. A tick that holds more turning points than the split
// list can take is handled by adding the bow of the polyline as slack, which
// over-reports rather than under-reports - the direction a safety check is
// allowed to be wrong in.
Sep swept_pair(const dai_show_plan *p, uint32_t i, uint32_t j, float t0, float t1,
               const dai_show_point &a0, const dai_show_point &a1,
               const dai_show_point &b0, const dai_show_point &b1,
               const uint32_t *kfirst, const uint32_t *kend, const float *bow,
               float floor_m) {
    float split[MAX_SPLITS];
    int   ns = 0, overflow = 0;
    for (int side = 0; side < 2; ++side) {
        const uint32_t d = side ? j : i;
        for (uint32_t m = kfirst[d]; m < kend[d]; ++m) {
            dai_show_key k;
            if (!dai_show_plan_key_at(p, d, m, &k)) break;
            if (!(k.t > t0 && k.t < t1)) continue;
            if (ns >= MAX_SPLITS) { overflow = 1; break; }
            split[ns++] = k.t;
        }
    }
    // Insertion sort: six elements at most, and it keeps the split order a
    // property of the times themselves rather than of a library's pivot.
    for (int a = 1; a < ns; ++a) {
        float v = split[a]; int b = a - 1;
        while (b >= 0 && split[b] > v) { split[b + 1] = split[b]; --b; }
        split[b + 1] = v;
    }
    const float slack = overflow ? (bow[i] + bow[j]) : 0.0f;

    Sep out; out.lo = 3.4e38f; out.hi = 3.4e38f;
    float pt0 = t0;
    Vec3  pa0 = vof(a0), pb0 = vof(b0);
    for (int k = 0; k <= ns; ++k) {
        const float pt1 = (k < ns) ? split[k] : t1;
        if (k < ns && !(pt1 > pt0)) continue;
        Vec3 pa1, pb1;
        if (k < ns) {
            dai_show_point sa, sb;
            dai_show_plan_sample(p, i, pt1, &sa);
            dai_show_plan_sample(p, j, pt1, &sb);
            pa1 = vof(sa); pb1 = vof(sb);
        } else {
            pa1 = vof(a1); pb1 = vof(b1);
        }
        Sep r = swept_piece(p, i, j, pt0, pt1, pa0, pa1, pb0, pb1, slack, floor_m, SWEPT_DEPTH);
        out.lo = std::min(out.lo, r.lo);
        out.hi = std::min(out.hi, r.hi);
        pt0 = pt1; pa0 = pa1; pb0 = pb1;
    }
    return out;
}

} // namespace

extern "C" {

uint32_t dai_show_validate(const dai_show_plan *p, const dai_show_settings *s,
                           dai_show_conflict *out, uint32_t max,
                           dai_show_validate_stats *stats) {
    dai_show_validate_stats st;
    std::memset(&st, 0, sizeof(st));
    st.min_distance_m = 3.4e38f;
    if (stats) *stats = st;
    if (!p || !s) return 0;

    const uint32_t n = dai_show_plan_drone_count(p);
    if (n == 0) return 0;

    const int   fps = (s->fps > 0) ? s->fps : 25;
    const float dt  = 1.0f / (float)fps;
    const float dur = dai_show_plan_duration(p);
    const float min_d = (s->min_distance_m > 0.0f) ? s->min_distance_m : 0.0f;
    // One limit, from src/dai_show_internal.hpp, and the separator in
    // dai_show_plan.cpp works to the same one. A formation packed to exactly
    // min_distance - which is what Poisson sampling produces - is not a fault,
    // and a validator that says otherwise buries the real conflict in noise.
    const float floor_m = min_d - daishow::SEP_EPS;
    const int   fenced = (s->fence_half_x > 0.0f && s->fence_half_z > 0.0f &&
                          s->fence_top_m > 0.0f);

    // Two frames of positions, one of velocity, one keyframe cursor per drone -
    // the whole state this check ever holds. 10,000 drones is under a megabyte,
    // and none of it grows with the length of the show.
    std::vector<dai_show_point> cur(n), prev(n);
    std::vector<float>          vel(n * 3, 0.0f);
    std::vector<uint8_t>        have_vel(n, 0);
    std::vector<uint32_t>       kcount(n), kcur(n, 0), kfirst(n, 0), kend(n, 0);
    std::vector<float>          bow(n, 0.0f), reach(n, 0.0f);
    for (uint32_t i = 0; i < n; ++i) kcount[i] = dai_show_plan_keyframe_count(p, i);
    Grid     grid;
    Recorder rec;
    grid.reset(n);

    const uint32_t ticks = (uint32_t)std::floor((double)dur * (double)fps) + 1u;

    for (uint32_t tick = 0; tick < ticks; ++tick) {
        const float t  = (float)((double)tick / (double)fps);
        const float t0 = (tick > 0) ? (float)((double)(tick - 1) / (double)fps) : t;
        dai_show_plan_sample_all(p, t, cur.data());
        if (tick == 0) prev = cur;
        ++st.ticks_checked;

        // Pass one: the rules a drone breaks on its own - the ones an operator
        // fixes by moving one figure - and how far it has travelled since the
        // last look. That second number sizes the grid: a cell has to be wide
        // enough that a pair which meets ANYWHERE inside the interval is still
        // neighbouring cells at the end of it.
        float reach_max = 0.0f;
        for (uint32_t i = 0; i < n; ++i) {
            const dai_show_point &pi = cur[i];

            if (fenced) {
                float ox = std::fabs(pi.x) - s->fence_half_x;
                float oz = std::fabs(pi.z) - s->fence_half_z;
                float oy = pi.y - s->fence_top_m;
                float worst_over = ox; float limit = s->fence_half_x; float value = std::fabs(pi.x);
                if (oz > worst_over) { worst_over = oz; limit = s->fence_half_z; value = std::fabs(pi.z); }
                if (oy > worst_over) { worst_over = oy; limit = s->fence_top_m;  value = pi.y; }
                if (worst_over > 0.0f)
                    rec.hit(i, i, DAI_SHOW_CONFLICT_FENCE, value, limit, t, tick);
            }
            if (s->min_ground_m > 0.0f && pi.y < s->min_ground_m)
                rec.hit(i, i, DAI_SHOW_CONFLICT_GROUND, pi.y, s->min_ground_m, t, tick);

            if (tick > 0) {
                float dx = pi.x - prev[i].x, dy = pi.y - prev[i].y, dz = pi.z - prev[i].z;
                float vx = dx / dt, vy = dy / dt, vz = dz / dt;
                float sp = std::sqrt(vx * vx + vy * vy + vz * vz);
                if (sp > st.max_speed_ms) st.max_speed_ms = sp;
                if (s->v_max_ms > 0.0f && sp > s->v_max_ms)
                    rec.hit(i, i, DAI_SHOW_CONFLICT_VMAX, sp, s->v_max_ms, t, tick);
                if (have_vel[i]) {
                    float ax = (vx - vel[i * 3 + 0]) / dt;
                    float ay = (vy - vel[i * 3 + 1]) / dt;
                    float az = (vz - vel[i * 3 + 2]) / dt;
                    float acc = std::sqrt(ax * ax + ay * ay + az * az);
                    if (acc > st.max_accel_ms2) st.max_accel_ms2 = acc;
                    if (s->a_max_ms2 > 0.0f && acc > s->a_max_ms2)
                        rec.hit(i, i, DAI_SHOW_CONFLICT_AMAX, acc, s->a_max_ms2, t, tick);
                }
                vel[i * 3 + 0] = vx; vel[i * 3 + 1] = vy; vel[i * 3 + 2] = vz;
                have_vel[i] = 1;
            }

            // Between two ticks the drone flies the polyline through whatever
            // keyframes fall inside, so the keys are walked once, in step with
            // the clock - every key of the show is looked at exactly once over
            // the whole check. `bow` is how far that polyline leaves its own
            // chord, and it is zero whenever the drone is inside one leg,
            // which is nearly always.
            const Vec3 a0 = vof(prev[i]), a1 = vof(pi);
            const Vec3 chord = vsub(a1, a0);
            bow[i] = 0.0f;
            uint32_t m = kcur[i];
            dai_show_key k;
            while (m < kcount[i] && dai_show_plan_key_at(p, i, m, &k) && k.t <= t0) ++m;
            kfirst[i] = m;
            while (m < kcount[i] && dai_show_plan_key_at(p, i, m, &k) && k.t < t) {
                bow[i] = std::max(bow[i], dist_origin_segment(vsub(a0, vof(k.p)), chord));
                ++m;
            }
            kend[i] = m;
            kcur[i] = m;             // a key exactly at t opens the next interval

            reach[i] = vlen(chord) + 2.0f * bow[i];
            if (reach[i] > reach_max) reach_max = reach[i];
        }

        if (min_d <= 0.0f) { prev.swap(cur); rec.close_stale(tick); continue; }

        // The broadphase. The cell edge is the minimum distance plus twice the
        // furthest any drone travelled in this interval, so two drones that
        // touch at any instant inside it are at most one cell apart at the end
        // of it - which is where they are indexed. Drone i is compared with
        // what is already in the table, exactly the drones below it in index
        // order, so every pair is tested once, in a fixed order, and never n^2.
        float cell = min_d + 2.0f * reach_max;
        if (!(cell > 0.0f)) cell = 1.0f;
        const float inv_cell = 1.0f / cell;

        std::fill(grid.head.begin(), grid.head.end(), -1);

        for (uint32_t i = 0; i < n; ++i) {
            const dai_show_point &pi = cur[i];
            int32_t cx = cell_of(pi.x, inv_cell);
            int32_t cy = cell_of(pi.y, inv_cell);
            int32_t cz = cell_of(pi.z, inv_cell);
            for (int ddx = -1; ddx <= 1; ++ddx)
            for (int ddy = -1; ddy <= 1; ++ddy)
            for (int ddz = -1; ddz <= 1; ++ddz) {
                int32_t nx = cx + ddx, ny = cy + ddy, nz = cz + ddz;
                uint32_t h = cell_hash(nx, ny, nz, grid.mask);
                for (int32_t jj = grid.head[h]; jj >= 0; jj = grid.next[jj]) {
                    // The bucket can hold foreign cells - a hash is not a cell.
                    if (grid.cell[jj * 3 + 0] != nx || grid.cell[jj * 3 + 1] != ny ||
                        grid.cell[jj * 3 + 2] != nz) continue;
                    const uint32_t j = (uint32_t)jj;
                    ++st.pairs_tested;
                    // The cheap rejection first. The cells are as wide as the
                    // furthest ANY drone moved this interval, so most of what
                    // lands in them is a pair that was never going to touch:
                    // if the two are further apart at the end of the interval
                    // than the limit plus what the two of them could possibly
                    // have travelled, no instant inside it can be closer.
                    const dai_show_point &pj = cur[j];
                    float ex = pi.x - pj.x, ey = pi.y - pj.y, ez = pi.z - pj.z;
                    float end_d = std::sqrt(ex * ex + ey * ey + ez * ez);
                    if (end_d < st.min_distance_m) st.min_distance_m = end_d;
                    if (end_d > min_d + reach[i] + reach[j]) continue;
                    Sep sep = swept_pair(p, i, j, t0, t, prev[i], pi, prev[j], cur[j],
                                         kfirst.data(), kend.data(), bow.data(), floor_m);
                    if (sep.hi < st.min_distance_m) st.min_distance_m = sep.hi;
                    if (sep.lo < floor_m) {
                        float value = sep.lo < 0.0f ? 0.0f : sep.lo;
                        rec.hit(j < i ? j : i, j < i ? i : j,
                                DAI_SHOW_CONFLICT_DISTANCE, value, min_d, t, tick);
                    }
                }
            }
            uint32_t hi = cell_hash(cx, cy, cz, grid.mask);
            grid.cell[i * 3 + 0] = cx; grid.cell[i * 3 + 1] = cy; grid.cell[i * 3 + 2] = cz;
            grid.next[i] = grid.head[hi];
            grid.head[hi] = (int32_t)i;
        }

        rec.close_stale(tick);
        prev.swap(cur);
    }
    rec.close_all();

    // (time, a, b, kind), so two runs of the same show produce byte identical
    // lists - the property the whole safety claim rests on.
    std::sort(rec.done.begin(), rec.done.end(),
              [](const dai_show_conflict &x, const dai_show_conflict &y) {
                  if (x.time_s != y.time_s) return x.time_s < y.time_s;
                  if (x.a != y.a) return x.a < y.a;
                  if (x.b != y.b) return x.b < y.b;
                  return x.kind < y.kind;
              });

    st.conflicts  = rec.total;
    st.peak_bytes = sizeof(dai_show_point) * (size_t)n * 2u +
                    sizeof(float) * (size_t)n * 4u + (size_t)n +
                    sizeof(uint32_t) * (size_t)n * 4u +
                    grid.bytes() + rec.bytes();
    if (st.min_distance_m > 3.3e38f) st.min_distance_m = 0.0f;
    if (stats) *stats = st;

    uint32_t written = 0;
    if (out && max) {
        written = (uint32_t)std::min((size_t)max, rec.done.size());
        std::memcpy(out, rec.done.data(), written * sizeof(dai_show_conflict));
    }
    return written;
}

} // extern "C"
