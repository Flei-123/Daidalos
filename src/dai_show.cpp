// The show document: formations, the storyboard, and the plan they solve into.
//
// Implements, from include/dai_show.h:
//     dai_show_settings_default
//     dai_show_create / destroy / get_settings / set_settings
//     dai_show_formation_* and dai_show_transition_get/set
//     dai_show_solve, dai_show_get_plan, dai_show_get_timings
//     dai_show_validate_show, dai_show_conflict_count, dai_show_conflict_at
//     dai_show_save / dai_show_load
//     the dai_show_plan container: from_keys, sample, sample_all, key_at,
//     drone_count, duration, keyframe_count, bytes, destroy
//
// This is dai_doc's argument one domain over: the document is the truth and
// everything else is derived from it. A formation is a record with a stable
// index, a transition belongs to the formation it arrives at, and the plan is
// a cache that is thrown away the moment a setting that could invalidate it
// changes - because a stale "no conflicts" badge is the one bug in this
// program that could hurt somebody.
//
// THE PLAN IS KEYFRAMES. Per drone: an ordered (time, position, colour, how we
// got here) list, one key per formation plus at most two for a layered detour.
// 10,000 drones through 20 formations is under 20 MB. Materialising 30,000
// ticks instead would be 3.6 GB, which is the whole reason dai_show_plan_sample
// is the only reader.
//
// Clocks are read HERE and nowhere else: dai_show_solve times the stages from
// the outside with a monotonic clock and writes the numbers into the timings.
// Nothing inside a solver ever asks what time it is.

#ifdef _WIN32
#include <windows.h>
#endif
#include "dai_show.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

// Implemented in src/dai_show_plan.cpp. The plan has to reconstruct exactly the
// curve stage 3 checked for collisions, so there is one implementation of the
// leg and both files use it rather than two that agree today.
namespace daishow {
float    ease_profile(int profile, float u);
void     leg_point(const dai_show_leg *leg, const dai_show_point *a,
                   const dai_show_point *b, float t, dai_show_point *out);
uint32_t leg_key_count(const dai_show_leg *leg, const dai_show_settings *s);
}

// ---------------------------------------------------------------------------

struct dai_show_plan {
    uint32_t                    drones = 0;
    float                       duration = 0.0f;
    std::vector<uint32_t>       first;      // drones + 1 offsets into `keys`
    std::vector<dai_show_key>   keys;
};

namespace {

const int  FORMAT_VERSION = 1;
const char MAGIC[] = "daidalos-show";

// How many conflicts the document keeps for the panel. The validator's stats
// carry the true total either way, so a show with a hundred thousand problems
// still says so - it just does not try to draw them all in a list nobody would
// scroll to the end of.
const uint32_t CONFLICT_LIMIT = 8192;

struct Formation {
    std::string                 name;
    std::string                 source;
    std::vector<dai_show_point> pts;
    float                       hold_s = 4.0f;
    int                         sample_mode = DAI_SHOW_SAMPLE_SURFACE;
    float                       t_start = 0.0f;    // derived by the last solve
    dai_show_transition         tr;                // the move INTO this one
};

// What one transition cost. Kept per transition as well as summed, because the
// two questions a director asks are different ones: "what did this show cost"
// is a sum over every move, and "why is THIS move ugly" is one row of it. A
// panel that shows only the last transition answers neither - it prints the
// numbers of whichever move happened to be solved last next to the conflict
// count of all of them, which is how "0 lifted, 0 delayed" ended up standing
// beside eleven conflicts.
struct TrStats {
    dai_show_assign_stats assign;
    dai_show_layer_stats  layer;
    int                   valid;   // 0 until this transition has been solved
};

double now_ms() {
    using clock = std::chrono::steady_clock;
    return std::chrono::duration<double, std::milli>(clock::now().time_since_epoch()).count();
}

void put(std::string &s, const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    s += buf;
}

// Shortest representation that still reads back bit identical - the same rule
// dai_doc_text follows, and for the same reason: a show file lives in version
// control, and 1.20000005 next to a hand typed 1.2 makes a diff unreadable.
std::string fstr(float v) {
    char buf[40];
    for (int prec = 6; prec < 9; ++prec) {
        snprintf(buf, sizeof(buf), "%.*g", prec, (double)v);
        if ((float)strtod(buf, nullptr) == v) return buf;
    }
    snprintf(buf, sizeof(buf), "%.9g", (double)v);
    return buf;
}

// Latitude and longitude are the one place a float would be a bug: 1e-7 degrees
// is a centimetre, and a show origin rounded to a float is eleven metres off.
std::string dstr(double v) {
    char buf[48];
    for (int prec = 15; prec < 17; ++prec) {
        snprintf(buf, sizeof(buf), "%.*g", prec, v);
        if (strtod(buf, nullptr) == v) return buf;
    }
    snprintf(buf, sizeof(buf), "%.17g", v);
    return buf;
}

const char *skip_ws(const char *p) {
    while (*p == ' ' || *p == '\t') ++p;
    return p;
}

const char *token(const char *p, std::string &out) {
    p = skip_ws(p);
    const char *start = p;
    while (*p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') ++p;
    out.assign(start, (size_t)(p - start));
    return p;
}

std::string rest_of_line(const char *p) {
    p = skip_ws(p);
    std::string s(p);
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n' ||
                          s.back() == ' '  || s.back() == '\t')) s.pop_back();
    return s;
}

void fail(char *err, size_t err_len, const char *fmt, ...) {
    if (!err || !err_len) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, err_len, fmt, ap);
    va_end(ap);
}

void copy_name(char *dst, size_t cap, const std::string &src) {
    size_t n = src.size() < cap - 1 ? src.size() : cap - 1;
    if (n) std::memcpy(dst, src.data(), n);
    dst[n] = 0;
}

bool tr_equal(const dai_show_transition &a, const dai_show_transition &b) {
    return a.duration_s == b.duration_s && a.profile == b.profile &&
           a.timing == b.timing && a.stagger_s == b.stagger_s &&
           a.assign_method == b.assign_method;
}

// Write through a temporary and rename, like every other writer in this engine:
// an interrupted save must not leave half a show where the work used to be.
dai_result write_file(const char *path, const std::string &text) {
    std::string tmp = std::string(path) + ".tmp";
    FILE *f = fopen(tmp.c_str(), "wb");
    if (!f) return DAI_ERR_FILE;
    size_t written = fwrite(text.data(), 1, text.size(), f);
    int flushed = fflush(f);
    fclose(f);
    if (written != text.size() || flushed != 0) { remove(tmp.c_str()); return DAI_ERR_FILE; }
#ifdef _WIN32
    if (!MoveFileExA(tmp.c_str(), path, MOVEFILE_REPLACE_EXISTING)) {
        remove(tmp.c_str());
        return DAI_ERR_FILE;
    }
#else
    if (rename(tmp.c_str(), path) != 0) { remove(tmp.c_str()); return DAI_ERR_FILE; }
#endif
    return DAI_OK;
}

} // namespace

struct dai_show {
    dai_show_settings              s;
    std::vector<Formation>         forms;
    dai_show_plan                 *plan = nullptr;
    dai_show_timings               timings;
    std::vector<dai_show_conflict> conflicts;
    std::vector<TrStats>           tr_stats;          // one per formation, [0] unused
    double                         sample_ms = 0.0;   // accumulated over the mesh samples
};

namespace {

// The show total. Counts add up (a lifted leg is a lifted leg wherever it
// happened), heights and gaps take the worst case - a 3% detour on one move is
// not made acceptable by twenty exact ones - and the method reported is the
// least exact one that ran, so a single clustered transition cannot hide behind
// nineteen exact ones.
void aggregate(dai_show_timings *t, const std::vector<TrStats> &per) {
    dai_show_assign_stats a;
    dai_show_layer_stats  l;
    std::memset(&a, 0, sizeof(a));
    std::memset(&l, 0, sizeof(l));
    a.method_used = DAI_SHOW_ASSIGN_EXACT;
    a.exact_cost_m = 0.0;
    a.gap_percent  = -1.0f;
    int any = 0, any_exact_cost = 1;
    for (size_t i = 0; i < per.size(); ++i) {
        if (!per[i].valid) continue;
        const dai_show_assign_stats &pa = per[i].assign;
        const dai_show_layer_stats  &pl = per[i].layer;
        any = 1;
        a.total_cost_m += pa.total_cost_m;
        if (pa.exact_cost_m >= 0.0) a.exact_cost_m += pa.exact_cost_m;
        else                        any_exact_cost = 0;
        if (pa.gap_percent > a.gap_percent) a.gap_percent = pa.gap_percent;
        if (pa.method_used > a.method_used) a.method_used = pa.method_used;
        a.clusters   += pa.clusters;
        a.iterations += pa.iterations;
        a.solve_ms   += pa.solve_ms;

        l.crossings_found    += pl.crossings_found;
        l.resolved_by_height += pl.resolved_by_height;
        l.resolved_by_delay  += pl.resolved_by_delay;
        l.unresolved         += pl.unresolved;
        l.endpoint_pairs     += pl.endpoint_pairs;
        if (pl.layers_used > l.layers_used) l.layers_used = pl.layers_used;
        if (pl.max_extra_height_m > l.max_extra_height_m) l.max_extra_height_m = pl.max_extra_height_m;
        l.solve_ms += pl.solve_ms;
    }
    if (!any) { a.method_used = DAI_SHOW_ASSIGN_AUTO; a.exact_cost_m = -1.0; }
    else if (!any_exact_cost) a.exact_cost_m = -1.0;
    t->last_assign = a;
    t->last_layer  = l;
}

} // namespace

// ---------------------------------------------------------------------------
extern "C" {

dai_show_settings dai_show_settings_default(void) {
    dai_show_settings s;
    std::memset(&s, 0, sizeof(s));
    s.min_distance_m       = 2.0f;    // the number every operator argues about first
    s.v_max_ms             = 8.0f;
    s.a_max_ms2            = 4.0f;
    s.drone_count          = 100;
    s.show_origin_lat      = 0.0;     // an unset origin, and visibly so
    s.show_origin_lon      = 0.0;
    s.show_origin_amsl     = 0.0f;
    s.show_orientation_deg = 0.0f;
    s.takeoff_alt_m        = 30.0f;
    s.fps                  = 25;
    s.fence_half_x         = 200.0f;
    s.fence_half_z         = 200.0f;
    s.fence_top_m          = 150.0f;
    s.min_ground_m         = 2.0f;
    s.seed                 = 0x9E3779B97F4A7C15ull;
    return s;
}

// ---- the plan container ---------------------------------------------------

dai_show_plan *dai_show_plan_from_keys(uint32_t drone_count, const uint32_t *counts,
                                       const dai_show_key *keys) {
    if (drone_count == 0 || !counts || !keys) return nullptr;
    dai_show_plan *p = new dai_show_plan();
    p->drones = drone_count;
    p->first.resize((size_t)drone_count + 1, 0);
    size_t total = 0;
    for (uint32_t i = 0; i < drone_count; ++i) { total += counts[i]; p->first[i + 1] = (uint32_t)total; }
    p->keys.assign(keys, keys + total);
    // Keys must be ordered in time for the binary search to mean anything. A
    // caller that hands them over in the wrong order gets them sorted rather
    // than a plan that samples nonsense - stable, so two keys at one instant
    // keep the order the caller intended.
    for (uint32_t i = 0; i < drone_count; ++i) {
        auto b = p->keys.begin() + p->first[i], e = p->keys.begin() + p->first[i + 1];
        if (!std::is_sorted(b, e, [](const dai_show_key &x, const dai_show_key &y) { return x.t < y.t; }))
            std::stable_sort(b, e, [](const dai_show_key &x, const dai_show_key &y) { return x.t < y.t; });
        if (p->first[i + 1] > p->first[i])
            p->duration = std::max(p->duration, p->keys[p->first[i + 1] - 1].t);
    }
    return p;
}

void dai_show_plan_destroy(dai_show_plan *p) { delete p; }

uint32_t dai_show_plan_drone_count(const dai_show_plan *p) { return p ? p->drones : 0; }
float    dai_show_plan_duration(const dai_show_plan *p) { return p ? p->duration : 0.0f; }

uint32_t dai_show_plan_keyframe_count(const dai_show_plan *p, uint32_t drone) {
    if (!p || drone >= p->drones) return 0;
    return p->first[drone + 1] - p->first[drone];
}

size_t dai_show_plan_bytes(const dai_show_plan *p) {
    if (!p) return 0;
    return sizeof(dai_show_plan) + p->keys.size() * sizeof(dai_show_key) +
           p->first.size() * sizeof(uint32_t);
}

int dai_show_plan_key_at(const dai_show_plan *p, uint32_t drone, uint32_t index,
                         dai_show_key *out) {
    if (!p || !out || drone >= p->drones) return 0;
    uint32_t b = p->first[drone], e = p->first[drone + 1];
    if (index >= e - b) return 0;
    *out = p->keys[b + index];
    return 1;
}

void dai_show_plan_sample(const dai_show_plan *p, uint32_t drone, float t,
                          dai_show_point *out) {
    if (!out) return;
    std::memset(out, 0, sizeof(*out));
    if (!p || drone >= p->drones) return;
    uint32_t b = p->first[drone], e = p->first[drone + 1];
    if (e == b) return;
    if (t <= p->keys[b].t)     { *out = p->keys[b].p; return; }
    if (t >= p->keys[e - 1].t) { *out = p->keys[e - 1].p; return; }

    // O(log k): the drone's own keys only. There is no array of ticks to walk
    // and there will not be one - that array is the 3.6 GB this design avoids.
    uint32_t lo = b, hi = e - 1;
    while (lo + 1 < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (p->keys[mid].t <= t) lo = mid; else hi = mid;
    }
    const dai_show_key &k0 = p->keys[lo];
    const dai_show_key &k1 = p->keys[hi];
    float span = k1.t - k0.t;
    // The segment carries the shape it was flown with, so a smooth leg reads
    // back smooth and a plan sampled at a keyframe time returns that keyframe.
    dai_show_leg leg;
    leg.t_start   = k0.t;
    leg.t_end     = (span > 0.0f) ? k1.t : k0.t + 1e-6f;
    leg.layer_y   = 0.0f;
    leg.rise_frac = 0.0f;
    leg.profile   = k1.profile;
    leg.pad       = 0;
    daishow::leg_point(&leg, &k0.p, &k1.p, t, out);
}

void dai_show_plan_sample_all(const dai_show_plan *p, float t, dai_show_point *out) {
    if (!p || !out) return;
    for (uint32_t i = 0; i < p->drones; ++i) dai_show_plan_sample(p, i, t, &out[i]);
}

// ---- the document ---------------------------------------------------------

dai_show *dai_show_create(const dai_show_settings *s) {
    dai_show *sh = new dai_show();
    sh->s = s ? *s : dai_show_settings_default();
    std::memset(&sh->timings, 0, sizeof(sh->timings));
    return sh;
}

void dai_show_destroy(dai_show *sh) {
    if (!sh) return;
    dai_show_plan_destroy(sh->plan);
    delete sh;
}

dai_show_settings dai_show_get_settings(const dai_show *sh) {
    return sh ? sh->s : dai_show_settings_default();
}

void dai_show_set_settings(dai_show *sh, const dai_show_settings *s) {
    if (!sh || !s) return;
    sh->s = *s;
    // The plan and its verdict belong to the settings they were solved under.
    // A new minimum distance next to an old "no conflicts" badge is the worst
    // thing this program could put on a screen.
    dai_show_plan_destroy(sh->plan);
    sh->plan = nullptr;
    sh->conflicts.clear();
    sh->tr_stats.clear();
    std::memset(&sh->timings, 0, sizeof(sh->timings));
    sh->timings.sample_ms = sh->sample_ms;
}

uint32_t dai_show_formation_add(dai_show *sh, const char *name, const char *source,
                                const dai_show_point *pts, uint32_t n, float hold_s) {
    if (!sh || !pts) return UINT32_MAX;
    if (n != sh->s.drone_count) return UINT32_MAX;   // no drone may be left in the air
    Formation f;
    f.name   = name ? name : "formation";
    f.source = source ? source : "";
    f.pts.assign(pts, pts + n);
    f.hold_s = (hold_s > 0.0f) ? hold_s : 0.0f;
    f.tr     = dai_show_transition_default();
    sh->forms.push_back(f);
    dai_show_plan_destroy(sh->plan);
    sh->plan = nullptr;
    sh->conflicts.clear();
    sh->tr_stats.clear();
    return (uint32_t)(sh->forms.size() - 1);
}

uint32_t dai_show_formation_from_mesh(dai_show *sh, const char *name, const char *source,
                                      const dai_show_sample_desc *desc,
                                      char *err, size_t err_len) {
    if (err && err_len) err[0] = 0;
    if (!sh || !desc) return UINT32_MAX;

    // The fleet size and the safety distance belong to the document, not to
    // whoever filled in the descriptor. A formation sampled at a different
    // count could not be flown by this show at all.
    dai_show_sample_desc d = *desc;
    d.count          = sh->s.drone_count;
    d.min_distance_m = sh->s.min_distance_m;
    if (d.count == 0) { fail(err, err_len, "the fleet is empty - set a drone count first"); return UINT32_MAX; }

    std::vector<dai_show_point> pts(d.count);
    double t0 = now_ms();
    uint32_t got = dai_show_sample(&d, pts.data(), d.count, err, err_len);
    sh->sample_ms += now_ms() - t0;
    sh->timings.sample_ms = sh->sample_ms;
    if (got != d.count) return UINT32_MAX;

    uint32_t idx = dai_show_formation_add(sh, name, source, pts.data(), got, 4.0f);
    if (idx != UINT32_MAX) sh->forms[idx].sample_mode = d.mode;
    return idx;
}

uint32_t dai_show_formation_count(const dai_show *sh) {
    return sh ? (uint32_t)sh->forms.size() : 0;
}

int dai_show_formation_get(const dai_show *sh, uint32_t i, dai_show_formation_info *out) {
    if (!sh || !out || i >= sh->forms.size()) return 0;
    const Formation &f = sh->forms[i];
    std::memset(out, 0, sizeof(*out));
    copy_name(out->name,   sizeof(out->name),   f.name);
    copy_name(out->source, sizeof(out->source), f.source);
    out->point_count = (uint32_t)f.pts.size();
    out->hold_s      = f.hold_s;
    out->sample_mode = f.sample_mode;
    out->t_start     = f.t_start;
    return 1;
}

const dai_show_point *dai_show_formation_points(const dai_show *sh, uint32_t i) {
    if (!sh || i >= sh->forms.size() || sh->forms[i].pts.empty()) return nullptr;
    return sh->forms[i].pts.data();
}

int dai_show_formation_remove(dai_show *sh, uint32_t i) {
    if (!sh || i >= sh->forms.size()) return 0;
    sh->forms.erase(sh->forms.begin() + i);
    dai_show_plan_destroy(sh->plan);
    sh->plan = nullptr;
    sh->conflicts.clear();
    sh->tr_stats.clear();
    return 1;
}

int dai_show_formation_move(dai_show *sh, uint32_t i, int delta) {
    if (!sh || i >= sh->forms.size() || delta == 0) return 0;
    long j = (long)i + delta;
    if (j < 0 || j >= (long)sh->forms.size()) return 0;
    // The transition travels with the formation it arrives at, which is what
    // makes reordering the storyboard a move rather than a rewrite.
    Formation f = sh->forms[i];
    sh->forms.erase(sh->forms.begin() + i);
    sh->forms.insert(sh->forms.begin() + j, f);
    dai_show_plan_destroy(sh->plan);
    sh->plan = nullptr;
    sh->conflicts.clear();
    sh->tr_stats.clear();
    return 1;
}

int dai_show_formation_rename(dai_show *sh, uint32_t i, const char *name) {
    if (!sh || i >= sh->forms.size() || !name) return 0;
    sh->forms[i].name = name;
    return 1;   // a name changes nothing the plan depends on
}

int dai_show_formation_set_hold(dai_show *sh, uint32_t i, float hold_s) {
    if (!sh || i >= sh->forms.size() || !(hold_s >= 0.0f)) return 0;
    sh->forms[i].hold_s = hold_s;
    dai_show_plan_destroy(sh->plan);
    sh->plan = nullptr;
    sh->conflicts.clear();
    sh->tr_stats.clear();
    return 1;
}

int dai_show_transition_get(const dai_show *sh, uint32_t i, dai_show_transition *out) {
    if (!sh || !out || i == 0 || i >= sh->forms.size()) return 0;
    *out = sh->forms[i].tr;
    return 1;
}

int dai_show_transition_set(dai_show *sh, uint32_t i, const dai_show_transition *tr) {
    if (!sh || !tr || i == 0 || i >= sh->forms.size()) return 0;
    sh->forms[i].tr = *tr;
    dai_show_plan_destroy(sh->plan);
    sh->plan = nullptr;
    sh->conflicts.clear();
    sh->tr_stats.clear();
    return 1;
}

// ---- the solve ------------------------------------------------------------

dai_result dai_show_solve(dai_show *sh, char *err, size_t err_len) {
    if (err && err_len) err[0] = 0;
    if (!sh) return DAI_ERR_INVALID_ARG;

    dai_show_plan_destroy(sh->plan);
    sh->plan = nullptr;
    sh->conflicts.clear();
    sh->tr_stats.clear();
    double keep_sample = sh->sample_ms;
    std::memset(&sh->timings, 0, sizeof(sh->timings));
    sh->timings.sample_ms  = keep_sample;
    sh->timings.drones     = sh->s.drone_count;
    sh->timings.formations = (uint32_t)sh->forms.size();
    sh->tr_stats.assign(sh->forms.size(), TrStats{});

    const uint32_t n = sh->s.drone_count;
    if (n == 0)             { fail(err, err_len, "the fleet is empty"); return DAI_ERR_STATE; }
    if (sh->forms.empty())  { fail(err, err_len, "the storyboard has no formations"); return DAI_ERR_STATE; }
    for (size_t i = 0; i < sh->forms.size(); ++i)
        if (sh->forms[i].pts.size() != n) {
            fail(err, err_len, "formation %u has %u points, the fleet is %u",
                 (unsigned)i, (unsigned)sh->forms[i].pts.size(), (unsigned)n);
            return DAI_ERR_STATE;
        }

    std::vector<std::vector<dai_show_key> > keys((size_t)n);
    std::vector<dai_show_point> cur(sh->forms[0].pts);
    std::vector<dai_show_point> nxt(n);
    std::vector<uint32_t>       perm(n);
    std::vector<dai_show_leg>   legs(n);

    for (uint32_t d = 0; d < n; ++d) {
        keys[d].reserve(sh->forms.size() * 3 + 2);
        dai_show_key k;
        k.t = 0.0f; k.p = cur[d]; k.profile = DAI_SHOW_PROFILE_LINEAR;
        keys[d].push_back(k);
    }

    float t_cursor = sh->forms[0].hold_s;
    sh->forms[0].t_start = 0.0f;
    if (t_cursor > 0.0f) {
        for (uint32_t d = 0; d < n; ++d) {
            dai_show_key k;
            k.t = t_cursor; k.p = cur[d]; k.profile = DAI_SHOW_PROFILE_LINEAR;
            keys[d].push_back(k);
        }
    }

    uint32_t unresolved_total = 0;
    for (size_t i = 1; i < sh->forms.size(); ++i) {
        Formation &f = sh->forms[i];
        for (uint32_t d = 0; d < n; ++d) nxt[d] = f.pts[d];

        dai_show_transition tr = f.tr;

        dai_show_assign_stats astats;
        std::memset(&astats, 0, sizeof(astats));
        double t0 = now_ms();
        dai_result ar = dai_show_assign(cur.data(), nxt.data(), n, tr.assign_method,
                                        perm.data(), &astats);
        double t1 = now_ms();
        sh->timings.assign_ms += t1 - t0;
        if (ar != DAI_OK) {
            fail(err, err_len, "assignment failed for formation %u", (unsigned)i);
            aggregate(&sh->timings, sh->tr_stats);   // what got as far as failing
            return DAI_ERR_STATE;
        }
        astats.solve_ms = t1 - t0;
        sh->tr_stats[i].assign = astats;
        sh->tr_stats[i].valid  = 1;

        // v_max and a_max are limits, not preferences: a duration that cannot
        // hold them is raised to the one that can, and the caller is told which
        // transition was stretched. Flying the number the director typed and
        // reporting the violation afterwards would be the wrong way round.
        // Measured on the longest leg the assignment actually produced, which
        // is why this sits after it and not before.
        float longest = 0.0f;
        for (uint32_t d = 0; d < n; ++d) {
            const dai_show_point &b = nxt[perm[d]];
            float dx = b.x - cur[d].x, dy = b.y - cur[d].y, dz = b.z - cur[d].z;
            longest = std::max(longest, std::sqrt(dx * dx + dy * dy + dz * dz));
        }
        float need = dai_show_min_duration(longest, tr.profile, &sh->s);
        if (need > tr.duration_s) {
            fail(err, err_len, "transition into '%s': %.2f s is under the limits, using %.2f s",
                 f.name.c_str(), (double)tr.duration_s, (double)need);
            tr.duration_s = need;
        }

        dai_show_layer_stats lstats;
        t0 = now_ms();
        dai_result lr = dai_show_layer(cur.data(), nxt.data(), n, perm.data(), &tr,
                                       &sh->s, t_cursor, legs.data(), &lstats);
        t1 = now_ms();
        sh->timings.layer_ms += t1 - t0;
        lstats.solve_ms = t1 - t0;
        sh->tr_stats[i].layer = lstats;
        if (lr != DAI_OK) unresolved_total += lstats.unresolved;

        // Stage 4 turns the legs into keyframes. The detour is a curve, so it
        // is cut into pieces small enough that the line the plan interpolates
        // is the curve the separator proved - a straight leg needs one key.
        t0 = now_ms();
        float t_arrive = legs[0].t_end;
        for (uint32_t d = 0; d < n; ++d) t_arrive = std::max(t_arrive, legs[d].t_end);
        for (uint32_t d = 0; d < n; ++d) {
            const dai_show_leg &g = legs[d];
            const dai_show_point a = cur[d];
            const dai_show_point b = nxt[perm[d]];
            dai_show_key k;
            if (g.t_start > keys[d].back().t) {          // staggered or delayed: wait first
                k.t = g.t_start; k.p = a; k.profile = DAI_SHOW_PROFILE_LINEAR;
                keys[d].push_back(k);
            }
            uint32_t steps = daishow::leg_key_count(&g, &sh->s);
            for (uint32_t st = 1; st <= steps; ++st) {
                float u = (float)st / (float)steps;
                k.t = g.t_start + (g.t_end - g.t_start) * u;
                if (st == steps) { k.t = g.t_end; k.p = b; }
                else daishow::leg_point(&g, &a, &b, k.t, &k.p);
                k.profile = (steps == 1) ? g.profile : DAI_SHOW_PROFILE_LINEAR;
                keys[d].push_back(k);
            }
            if (t_arrive > g.t_end) {                    // hold formation until the last one lands
                k.t = t_arrive; k.p = b; k.profile = DAI_SHOW_PROFILE_LINEAR;
                keys[d].push_back(k);
            }
            if (f.hold_s > 0.0f) {
                k.t = t_arrive + f.hold_s; k.p = b; k.profile = DAI_SHOW_PROFILE_LINEAR;
                keys[d].push_back(k);
            }
            cur[d] = b;
        }
        sh->timings.profile_ms += now_ms() - t0;

        f.t_start = t_arrive;
        t_cursor  = t_arrive + f.hold_s;
    }

    aggregate(&sh->timings, sh->tr_stats);

    std::vector<uint32_t>      counts(n);
    std::vector<dai_show_key>  flat;
    size_t total = 0;
    for (uint32_t d = 0; d < n; ++d) { counts[d] = (uint32_t)keys[d].size(); total += counts[d]; }
    flat.reserve(total);
    for (uint32_t d = 0; d < n; ++d) flat.insert(flat.end(), keys[d].begin(), keys[d].end());
    sh->plan = dai_show_plan_from_keys(n, counts.data(), flat.data());

    if (unresolved_total > 0) {
        fail(err, err_len, "%u transition conflicts could not be separated", (unsigned)unresolved_total);
        return DAI_ERR_STATE;
    }
    return DAI_OK;
}

const dai_show_plan *dai_show_get_plan(const dai_show *sh) { return sh ? sh->plan : nullptr; }

int dai_show_transition_stats(const dai_show *sh, uint32_t i,
                              dai_show_assign_stats *assign,
                              dai_show_layer_stats *layer) {
    if (!sh || i >= sh->tr_stats.size()) return 0;
    if (!sh->tr_stats[i].valid) return 0;
    if (assign) *assign = sh->tr_stats[i].assign;
    if (layer)  *layer  = sh->tr_stats[i].layer;
    return 1;
}

dai_show_timings dai_show_get_timings(const dai_show *sh) {
    if (sh) return sh->timings;
    dai_show_timings t;
    std::memset(&t, 0, sizeof(t));
    return t;
}

dai_result dai_show_validate_show(dai_show *sh) {
    if (!sh) return DAI_ERR_INVALID_ARG;
    sh->conflicts.clear();
    if (!sh->plan) return DAI_ERR_STATE;

    sh->conflicts.resize(CONFLICT_LIMIT);
    dai_show_validate_stats vs;
    std::memset(&vs, 0, sizeof(vs));
    double t0 = now_ms();
    uint32_t got = dai_show_validate(sh->plan, &sh->s, sh->conflicts.data(),
                                     CONFLICT_LIMIT, &vs);
    sh->timings.validate_ms   = now_ms() - t0;
    vs.solve_ms               = sh->timings.validate_ms;
    sh->timings.last_validate = vs;
    sh->conflicts.resize(got);
    return (vs.conflicts == 0) ? DAI_OK : DAI_ERR_STATE;
}

uint32_t dai_show_conflict_count(const dai_show *sh) {
    return sh ? (uint32_t)sh->conflicts.size() : 0;
}

int dai_show_conflict_at(const dai_show *sh, uint32_t i, dai_show_conflict *out) {
    if (!sh || !out || i >= sh->conflicts.size()) return 0;
    *out = sh->conflicts[i];
    return 1;
}

// ---- the file -------------------------------------------------------------

dai_result dai_show_save(const dai_show *sh, const char *path, char *err, size_t err_len) {
    if (err && err_len) err[0] = 0;
    if (!sh || !path) return DAI_ERR_INVALID_ARG;

    const dai_show_settings d = dai_show_settings_default();
    const dai_show_settings &s = sh->s;
    std::string out;
    put(out, "%s %d\n", MAGIC, FORMAT_VERSION);

    // Only what differs from the default, so a show file diffs per property
    // and a field added next year does not rewrite every file in the archive.
    put(out, "settings\n");
    if (s.min_distance_m != d.min_distance_m) put(out, "  min-distance %s\n", fstr(s.min_distance_m).c_str());
    if (s.v_max_ms       != d.v_max_ms)       put(out, "  v-max %s\n",        fstr(s.v_max_ms).c_str());
    if (s.a_max_ms2      != d.a_max_ms2)      put(out, "  a-max %s\n",        fstr(s.a_max_ms2).c_str());
    if (s.drone_count    != d.drone_count)    put(out, "  drones %u\n",       (unsigned)s.drone_count);
    if (s.show_origin_lat != d.show_origin_lat || s.show_origin_lon != d.show_origin_lon ||
        s.show_origin_amsl != d.show_origin_amsl)
        put(out, "  origin %s %s %s\n", dstr(s.show_origin_lat).c_str(),
            dstr(s.show_origin_lon).c_str(), fstr(s.show_origin_amsl).c_str());
    if (s.show_orientation_deg != d.show_orientation_deg)
        put(out, "  orientation %s\n", fstr(s.show_orientation_deg).c_str());
    if (s.takeoff_alt_m != d.takeoff_alt_m) put(out, "  takeoff-alt %s\n", fstr(s.takeoff_alt_m).c_str());
    if (s.fps           != d.fps)           put(out, "  fps %d\n", s.fps);
    if (s.fence_half_x != d.fence_half_x || s.fence_half_z != d.fence_half_z ||
        s.fence_top_m  != d.fence_top_m  || s.min_ground_m != d.min_ground_m)
        put(out, "  fence %s %s %s %s\n", fstr(s.fence_half_x).c_str(), fstr(s.fence_half_z).c_str(),
            fstr(s.fence_top_m).c_str(), fstr(s.min_ground_m).c_str());
    if (s.seed != d.seed) put(out, "  seed %llu\n", (unsigned long long)s.seed);

    const dai_show_transition td = dai_show_transition_default();
    for (size_t i = 0; i < sh->forms.size(); ++i) {
        const Formation &f = sh->forms[i];
        put(out, "formation\n");
        put(out, "  name %s\n", f.name.c_str());
        if (!f.source.empty()) put(out, "  source %s\n", f.source.c_str());
        if (f.hold_s != 4.0f)  put(out, "  hold %s\n", fstr(f.hold_s).c_str());
        if (f.sample_mode != DAI_SHOW_SAMPLE_SURFACE) put(out, "  sample-mode %d\n", f.sample_mode);
        if (i > 0 && !tr_equal(f.tr, td))
            put(out, "  transition %s %d %d %s %d\n", fstr(f.tr.duration_s).c_str(),
                f.tr.profile, f.tr.timing, fstr(f.tr.stagger_s).c_str(), f.tr.assign_method);
        // The points as a block. Ten thousand of them per formation is the one
        // place in this engine where a line of prose per property would be
        // silly - seven fields, one point, no keys.
        put(out, "  points %u\n", (unsigned)f.pts.size());
        for (size_t k = 0; k < f.pts.size(); ++k) {
            const dai_show_point &p = f.pts[k];
            put(out, "    %s %s %s %u %u %u %u\n", fstr(p.x).c_str(), fstr(p.y).c_str(),
                fstr(p.z).c_str(), p.r, p.g, p.b, p.w);
        }
    }

    dai_result r = write_file(path, out);
    if (r != DAI_OK) fail(err, err_len, "could not write '%s'", path);
    return r;
}

dai_show *dai_show_load(const char *path, char *err, size_t err_len) {
    if (err && err_len) err[0] = 0;
    if (!path) return nullptr;
    FILE *f = fopen(path, "rb");
    if (!f) { fail(err, err_len, "could not open '%s'", path); return nullptr; }
    std::string text;
    char buf[4096];
    size_t got;
    while ((got = fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, got);
    fclose(f);

    std::vector<std::string> lines;
    {
        size_t start = 0;
        for (size_t i = 0; i <= text.size(); ++i) {
            if (i == text.size() || text[i] == '\n') {
                lines.push_back(text.substr(start, i - start));
                start = i + 1;
            }
        }
    }
    if (lines.empty()) { fail(err, err_len, "'%s' is empty", path); return nullptr; }

    std::string tok;
    const char *p = token(lines[0].c_str(), tok);
    if (tok != MAGIC) { fail(err, err_len, "'%s' is not a show file", path); return nullptr; }
    int version = atoi(skip_ws(p));
    if (version > FORMAT_VERSION) {
        fail(err, err_len, "show format %d is newer than this build (%d)", version, FORMAT_VERSION);
        return nullptr;
    }

    dai_show_settings s = dai_show_settings_default();
    struct Loaded {
        std::string name, source;
        float hold = 4.0f;
        int   mode = DAI_SHOW_SAMPLE_SURFACE;
        dai_show_transition tr = { 0.0f, 0, 0, 0.0f, 0 };
        bool  has_tr = false;
        std::vector<dai_show_point> pts;
    };
    std::vector<Loaded> forms;

    for (size_t li = 1; li < lines.size(); ++li) {
        const char *lp = token(lines[li].c_str(), tok);
        if (tok.empty() || tok[0] == '#') continue;

        if (tok == "settings") continue;
        if (tok == "formation") { forms.push_back(Loaded()); continue; }

        if (forms.empty()) {                     // still in the settings block
            if      (tok == "min-distance") s.min_distance_m = (float)atof(skip_ws(lp));
            else if (tok == "v-max")        s.v_max_ms       = (float)atof(skip_ws(lp));
            else if (tok == "a-max")        s.a_max_ms2      = (float)atof(skip_ws(lp));
            else if (tok == "drones")       s.drone_count    = (uint32_t)strtoul(skip_ws(lp), nullptr, 10);
            else if (tok == "origin") {
                char *e1 = nullptr, *e2 = nullptr;
                s.show_origin_lat  = strtod(skip_ws(lp), &e1);
                s.show_origin_lon  = strtod(e1, &e2);
                s.show_origin_amsl = (float)strtod(e2, nullptr);
            }
            else if (tok == "orientation")  s.show_orientation_deg = (float)atof(skip_ws(lp));
            else if (tok == "takeoff-alt")  s.takeoff_alt_m  = (float)atof(skip_ws(lp));
            else if (tok == "fps")          s.fps            = atoi(skip_ws(lp));
            else if (tok == "fence") {
                char *e1 = nullptr, *e2 = nullptr, *e3 = nullptr;
                s.fence_half_x = (float)strtod(skip_ws(lp), &e1);
                s.fence_half_z = (float)strtod(e1, &e2);
                s.fence_top_m  = (float)strtod(e2, &e3);
                s.min_ground_m = (float)strtod(e3, nullptr);
            }
            else if (tok == "seed")         s.seed = strtoull(skip_ws(lp), nullptr, 10);
            continue;                        // an unknown key is a newer field, not corruption
        }

        Loaded &fm = forms.back();
        if      (tok == "name")        fm.name   = rest_of_line(lp);
        else if (tok == "source")      fm.source = rest_of_line(lp);
        else if (tok == "hold")        fm.hold   = (float)atof(skip_ws(lp));
        else if (tok == "sample-mode") fm.mode   = atoi(skip_ws(lp));
        else if (tok == "transition") {
            char *e1 = nullptr, *e2 = nullptr, *e3 = nullptr, *e4 = nullptr;
            fm.tr.duration_s    = (float)strtod(skip_ws(lp), &e1);
            fm.tr.profile       = (int)strtol(e1, &e2, 10);
            fm.tr.timing        = (int)strtol(e2, &e3, 10);
            fm.tr.stagger_s     = (float)strtod(e3, &e4);
            fm.tr.assign_method = (int)strtol(e4, nullptr, 10);
            fm.has_tr = true;
        }
        else if (tok == "points") {
            size_t count = strtoul(skip_ws(lp), nullptr, 10);
            fm.pts.reserve(count);
            for (size_t k = 0; k < count; ++k) {
                if (++li >= lines.size()) {
                    fail(err, err_len, "formation '%s': the point block ends early", fm.name.c_str());
                    return nullptr;
                }
                dai_show_point pt;
                char *e = nullptr;
                const char *q = lines[li].c_str();
                pt.x = (float)strtod(q, &e);   q = e;
                pt.y = (float)strtod(q, &e);   q = e;
                pt.z = (float)strtod(q, &e);   q = e;
                pt.r = (uint8_t)strtol(q, &e, 10); q = e;
                pt.g = (uint8_t)strtol(q, &e, 10); q = e;
                pt.b = (uint8_t)strtol(q, &e, 10); q = e;
                pt.w = (uint8_t)strtol(q, &e, 10);
                if (e == q) {
                    fail(err, err_len, "formation '%s': point %u is malformed",
                         fm.name.c_str(), (unsigned)k);
                    return nullptr;
                }
                fm.pts.push_back(pt);
            }
        }
    }

    dai_show *sh = dai_show_create(&s);
    for (size_t i = 0; i < forms.size(); ++i) {
        Loaded &fm = forms[i];
        uint32_t idx = dai_show_formation_add(sh, fm.name.c_str(), fm.source.c_str(),
                                              fm.pts.data(), (uint32_t)fm.pts.size(), fm.hold);
        if (idx == UINT32_MAX) {
            fail(err, err_len, "formation '%s' has %u points, the fleet is %u",
                 fm.name.c_str(), (unsigned)fm.pts.size(), (unsigned)s.drone_count);
            dai_show_destroy(sh);
            return nullptr;
        }
        sh->forms[idx].sample_mode = fm.mode;
        if (fm.has_tr) sh->forms[idx].tr = fm.tr;
    }
    return sh;
}

} // extern "C"
