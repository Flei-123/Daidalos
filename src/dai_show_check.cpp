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
// Speed and acceleration come from finite differences over the same stream,
// which is also what makes a deliberately broken plan detectable - the test
// builds one and this file has to find it.
//
// Conflicts come out sorted by (time, a, b). Not because it is tidy: an
// unsorted list built by several threads is a different list every run, and
// this program's answers have to be reproducible.

#include "dai_show.h"

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
    const float min_d2 = min_d * min_d;
    const float inv_cell = (min_d > 0.0f) ? (1.0f / min_d) : 1.0f;
    const int   fenced = (s->fence_half_x > 0.0f && s->fence_half_z > 0.0f &&
                          s->fence_top_m > 0.0f);

    // Two frames of positions and two of velocity - the whole state this check
    // ever holds. 10,000 drones is 640 KB, and it does not grow with the show.
    std::vector<dai_show_point> cur(n), prev(n);
    std::vector<float>          vel(n * 3, 0.0f);
    std::vector<uint8_t>        have_vel(n, 0);
    Grid     grid;
    Recorder rec;
    grid.reset(n);

    const uint32_t ticks = (uint32_t)std::floor((double)dur * (double)fps) + 1u;

    for (uint32_t tick = 0; tick < ticks; ++tick) {
        const float t = (float)((double)tick / (double)fps);
        dai_show_plan_sample_all(p, t, cur.data());
        ++st.ticks_checked;

        std::fill(grid.head.begin(), grid.head.end(), -1);

        for (uint32_t i = 0; i < n; ++i) {
            const dai_show_point &pi = cur[i];

            // The single-drone rules first: they need no neighbour and they are
            // the ones an operator can fix by moving one figure.
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

            if (min_d <= 0.0f) continue;

            // The broadphase. Drone i is compared with what is already in the
            // table, which is exactly the drones below it in index order - so
            // every pair is tested once, in a fixed order, and never n^2.
            int32_t cx = cell_of(pi.x, inv_cell);
            int32_t cy = cell_of(pi.y, inv_cell);
            int32_t cz = cell_of(pi.z, inv_cell);
            for (int ddx = -1; ddx <= 1; ++ddx)
            for (int ddy = -1; ddy <= 1; ++ddy)
            for (int ddz = -1; ddz <= 1; ++ddz) {
                int32_t nx = cx + ddx, ny = cy + ddy, nz = cz + ddz;
                uint32_t h = cell_hash(nx, ny, nz, grid.mask);
                for (int32_t j = grid.head[h]; j >= 0; j = grid.next[j]) {
                    // The bucket can hold foreign cells - a hash is not a cell.
                    if (grid.cell[j * 3 + 0] != nx || grid.cell[j * 3 + 1] != ny ||
                        grid.cell[j * 3 + 2] != nz) continue;
                    const dai_show_point &pj = cur[j];
                    float dx = pi.x - pj.x, dy = pi.y - pj.y, dz = pi.z - pj.z;
                    float d2 = dx * dx + dy * dy + dz * dz;
                    ++st.pairs_tested;
                    float d = std::sqrt(d2);
                    if (d < st.min_distance_m) st.min_distance_m = d;
                    if (d2 < min_d2)
                        rec.hit((uint32_t)j < i ? (uint32_t)j : i,
                                (uint32_t)j < i ? i : (uint32_t)j,
                                DAI_SHOW_CONFLICT_DISTANCE, d, min_d, t, tick);
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
                    sizeof(float) * (size_t)n * 3u + (size_t)n +
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
