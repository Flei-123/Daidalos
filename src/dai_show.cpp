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
#include "dai_show_internal.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
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

const int  FORMAT_VERSION = 2;   // 2 adds the formation transform and the colour override
const char MAGIC[] = "daidalos-show";

// How many conflicts the document keeps for the panel. The validator's stats
// carry the true total either way, so a show with a hundred thousand problems
// still says so - it just does not try to draw them all in a list nobody would
// scroll to the end of.
const uint32_t CONFLICT_LIMIT = 8192;

struct Formation {
    std::string                 name;
    std::string                 source;
    // TWO copies of the figure, and the split is the whole point of the
    // transform: `local` is what came off the mesh and is never touched by a
    // move, `pts` is that put through `xf` and is what the solver, the
    // exporter and the preview read. Everything downstream already asked for
    // world coordinates, so the cache keeps the name they already use and not
    // one line of the pipeline had to learn about a transform.
    std::vector<dai_show_point> local;
    std::vector<dai_show_point> pts;               // = local through xf, cached
    dai_show_transform          xf;
    dai_vec3                    pivot = { 0.0f, 0.0f, 0.0f };
    int                         colour_override = 0;
    uint8_t                     ovr[4] = { 255, 255, 255, 0 };
    float                       hold_s = 4.0f;
    int                         sample_mode = DAI_SHOW_SAMPLE_SURFACE;
    float                       t_start = 0.0f;    // derived by the last solve
    dai_show_transition         tr;                // the move INTO this one
    // Same group id = the figures fly one after another. Different ids = at
    // the same time, each group over its own slice of the fleet. Zero is the
    // group every figure ever made before this field existed is in, which is
    // why zero is the default: an old show re-reads as the show it was.
    int                         group = 0;
    // A paint stroke is colour WITH A TIME: "these points, from this second
    // on". The point's own colour is what it starts the show in; every stroke
    // whose time has come repaints its points until a later stroke repaints
    // them again. That is what makes "paint at the playhead" a light program
    // instead of a static recolour.
    struct Stroke {
        float                 t;
        uint8_t               rgba[4];
        std::vector<uint32_t> idx;
    };
    std::vector<Stroke>         strokes;
};

// local -> world, plus the colour override, into the cache everything reads.
//
// The identity path COPIES. It does not multiply by one and add zero: a show
// saved before this field existed has to solve to the same plan it solved to
// before, bit for bit, and (x - p) * 1 + p is not x for every float x. A
// feature that moves an untouched show by one ulp has broken the determinism
// rule this engine is built on, quietly, in the one place nobody looks.
void rebuild(Formation &f) {
    f.pts.resize(f.local.size());
    const size_t n = f.local.size();
    if (dai_show_transform_is_identity(&f.xf)) {
        for (size_t k = 0; k < n; ++k) f.pts[k] = f.local[k];
    } else {
        const float rx = f.xf.rotation_deg.x * (float)(3.14159265358979323846 / 180.0);
        const float ry = f.xf.rotation_deg.y * (float)(3.14159265358979323846 / 180.0);
        const float rz = f.xf.rotation_deg.z * (float)(3.14159265358979323846 / 180.0);
        const float cx = std::cos(rx), sx = std::sin(rx);
        const float cy = std::cos(ry), sy = std::sin(ry);
        const float cz = std::cos(rz), sz = std::sin(rz);
        // Rz * Ry * Rx, written out once rather than three matrix products per
        // point: ten thousand points times a dragged slider is a per-frame cost.
        const float m00 =  cz * cy;
        const float m01 =  cz * sy * sx - sz * cx;
        const float m02 =  cz * sy * cx + sz * sx;
        const float m10 =  sz * cy;
        const float m11 =  sz * sy * sx + cz * cx;
        const float m12 =  sz * sy * cx - cz * sx;
        const float m20 = -sy;
        const float m21 =  cy * sx;
        const float m22 =  cy * cx;
        for (size_t k = 0; k < n; ++k) {
            const dai_show_point &l = f.local[k];
            float ax = (l.x - f.pivot.x) * f.xf.scale.x;
            float ay = (l.y - f.pivot.y) * f.xf.scale.y;
            float az = (l.z - f.pivot.z) * f.xf.scale.z;
            dai_show_point w = l;
            w.x = f.xf.position.x + m00 * ax + m01 * ay + m02 * az + f.pivot.x;
            w.y = f.xf.position.y + m10 * ax + m11 * ay + m12 * az + f.pivot.y;
            w.z = f.xf.position.z + m20 * ax + m21 * ay + m22 * az + f.pivot.z;
            f.pts[k] = w;
        }
    }
    if (f.colour_override) {
        for (size_t k = 0; k < n; ++k) {
            f.pts[k].r = f.ovr[0]; f.pts[k].g = f.ovr[1];
            f.pts[k].b = f.ovr[2]; f.pts[k].w = f.ovr[3];
        }
    }
}

// Where one keyframe's COLOUR came from, so a colour can be changed without
// throwing away a solve.
//
// Colour is not geometry. Repainting a figure moves no drone, breaks no
// minimum distance and invalidates no assignment - but the colour lives in the
// keyframes, so without this the only honest thing the editor could do after a
// click on a swatch is discard the plan and make the user solve ten thousand
// drones again to see a shade of blue. That is not a tool, that is a dare.
//
// So the solve records, per key, which two formation points that key's colour
// was mixed from and at what fraction, and a repaint walks the list. Sixteen
// bytes per key against the twenty-four the key itself costs; a ten thousand
// drone show is a couple of megabytes of it, which is the same order as the
// plan and orders below the array of ticks this design exists to avoid.
struct KeySrc {
    uint32_t form;       // the formation being flown INTO, or held at
    uint32_t form_from;  // the formation flown FROM - NOT form - 1, because a
                         // group's predecessor is a sibling, not a neighbour
    uint32_t pa;         // point index in forms[form_from]; == pb while standing
    uint32_t pb;         // point index in forms[form]
    float    s;          // the mix: 0 = the colour of a, 1 = the colour of b
    // 1 on the two keys a paint stroke INSERTS at its second. They are not
    // the solver's keys: every colour refresh drops them and inserts them
    // again from the strokes that exist then, so a cleared stroke leaves no
    // keyhole behind.
    uint8_t  stroke_key = 0;
};

inline uint8_t mix_u8(uint8_t a, uint8_t b, float s) {
    // The same arithmetic daishow::lerp_u8 does, so a repainted key is byte for
    // byte the key a fresh solve would have produced. Two roundings that differ
    // by one would make "recolour" and "solve again" two different pictures.
    float v = (float)a + ((float)b - (float)a) * s;
    v = v + 0.5f;
    if (!(v > 0.0f)) v = 0.0f;
    if (v > 255.0f)  v = 255.0f;
    return (uint8_t)v;
}

dai_vec3 centroid_of(const std::vector<dai_show_point> &v) {
    dai_vec3 c = { 0.0f, 0.0f, 0.0f };
    if (v.empty()) return c;
    // Summed in double and in index order: the pivot is part of every world
    // coordinate the show is flown at, so it may not depend on how the compiler
    // felt about reassociating a float sum.
    double sx = 0.0, sy = 0.0, sz = 0.0;
    for (size_t i = 0; i < v.size(); ++i) { sx += v[i].x; sy += v[i].y; sz += v[i].z; }
    c.x = (float)(sx / (double)v.size());
    c.y = (float)(sy / (double)v.size());
    c.z = (float)(sz / (double)v.size());
    return c;
}

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
    // Parallel to plan->keys, filled by the solve, read by a repaint. Empty
    // when there is no plan, and a size mismatch is treated as "do not touch"
    // rather than as an index to trust.
    std::vector<KeySrc>            key_src;
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

// The first pair of a formation that stands closer than min_distance, in
// (lower index, higher index) order so two runs name the same one. Called only
// when dai_show_layer has already said such a pair exists, and through the same
// uniform grid the rest of the pipeline uses, so naming it costs O(n) rather
// than the O(n^2) a fleet of ten thousand cannot pay. Returns 0 when the
// formation is clean after all.
int first_endpoint_pair(const dai_show_point *p, uint32_t n, float min_d,
                        uint32_t *out_a, uint32_t *out_b, float *out_gap) {
    if (!p || n < 2 || !(min_d > 0.0f)) return 0;
    const float cell = min_d;
    std::unordered_map<int64_t, std::vector<uint32_t> > grid;
    grid.reserve(n * 2);
    auto key = [&](int cx, int cy, int cz) -> int64_t {
        return ((int64_t)(cx & 0x1FFFFF) << 42) | ((int64_t)(cy & 0x1FFFFF) << 21) |
                (int64_t)(cz & 0x1FFFFF);
    };
    auto cell_of = [&](float v) -> int { return (int)std::floor(v / cell); };
    for (uint32_t i = 0; i < n; ++i)
        grid[key(cell_of(p[i].x), cell_of(p[i].y), cell_of(p[i].z))].push_back(i);

    uint32_t ba = 0, bb = 0; float bgap = 0.0f; int found = 0;
    for (uint32_t i = 0; i < n; ++i) {
        int cx = cell_of(p[i].x), cy = cell_of(p[i].y), cz = cell_of(p[i].z);
        for (int dx = -1; dx <= 1; ++dx)
            for (int dy = -1; dy <= 1; ++dy)
                for (int dz = -1; dz <= 1; ++dz) {
                    auto it = grid.find(key(cx + dx, cy + dy, cz + dz));
                    if (it == grid.end()) continue;
                    for (size_t k = 0; k < it->second.size(); ++k) {
                        uint32_t j = it->second[k];
                        if (j <= i) continue;
                        float ex = p[i].x - p[j].x, ey = p[i].y - p[j].y, ez = p[i].z - p[j].z;
                        float d = std::sqrt(ex * ex + ey * ey + ez * ez);
                        if (!(d < min_d - daishow::SEP_EPS)) continue;
                        if (!found || i < ba || (i == ba && j < bb)) {
                            ba = i; bb = j; bgap = d; found = 1;
                        }
                    }
                }
    }
    if (!found) return 0;
    if (out_a) *out_a = ba;
    if (out_b) *out_b = bb;
    if (out_gap) *out_gap = bgap;
    return 1;
}

// A repaint, straight into the plan the last solve left behind.
//
// Nothing here decides anything: it re-reads the colour of the two formation
// points every key was mixed from and mixes them again at the same fraction.
// No position is touched, so the show that was validated is still the show in
// memory - which is why a colour edit may keep the "no conflicts" badge while a
// moved figure may not.
// The colour of one point at one second: its own colour, repainted by every
// stroke whose time has come, the latest stroke last.
void colour_at(const Formation &f, uint32_t pt, float t,
               uint8_t *r, uint8_t *g, uint8_t *b, uint8_t *w) {
    const dai_show_point &p = f.pts[pt];
    *r = p.r; *g = p.g; *b = p.b; *w = p.w;
    for (size_t si = 0; si < f.strokes.size(); ++si) {
        const Formation::Stroke &st = f.strokes[si];
        if (st.t > t + 1e-4f) continue;
        for (size_t k = 0; k < st.idx.size(); ++k)
            if (st.idx[k] == pt) {
                *r = st.rgba[0]; *g = st.rgba[1]; *b = st.rgba[2]; *w = st.rgba[3];
                break;
            }
    }
}

// The mixed colour one key carries when the clock stands at t_eval.
void key_colour(const dai_show *sh, const KeySrc &src, float t_eval,
                uint8_t *r, uint8_t *g, uint8_t *b, uint8_t *w) {
    const Formation &fb = sh->forms[src.form];
    const Formation &fa = sh->forms[src.form_from];
    uint8_t ar, ag, ab, aw, br, bg, bb, bw;
    colour_at(fa, src.pa, t_eval, &ar, &ag, &ab, &aw);
    colour_at(fb, src.pb, t_eval, &br, &bg, &bb, &bw);
    *r = mix_u8(ar, br, src.s);
    *g = mix_u8(ag, bg, src.s);
    *b = mix_u8(ab, bb, src.s);
    *w = mix_u8(aw, bw, src.s);
}

void recolour(dai_show *sh) {
    if (!sh || !sh->plan) return;
    if (sh->key_src.size() != sh->plan->keys.size()) return;  // out of step: leave it alone
    const size_t nf = sh->forms.size();
    for (size_t k = 0; k < sh->plan->keys.size(); ++k) {
        const KeySrc &src = sh->key_src[k];
        if (src.stroke_key) continue;          // inserted keys keep their colour
        if (src.form >= nf || src.form_from >= nf) continue;
        const Formation &fb = sh->forms[src.form];
        const Formation &fa = sh->forms[src.form_from];
        if (src.pa >= fa.pts.size() || src.pb >= fb.pts.size()) continue;
        dai_show_point &o = sh->plan->keys[k].p;
        key_colour(sh, src, sh->plan->keys[k].t, &o.r, &o.g, &o.b, &o.w);
    }
}

// Colours after any edit: the stroke keys from last time OUT, every solver
// key re-mixed at its own second, and for every stroke two fresh keys IN at
// its second - one holding the old colour until the stroke's time, one with
// the new colour from it on. Without the pair the plan would BLEND towards a
// stroke across the whole segment before it, which is a fade nobody ordered.
void refresh_colours(dai_show *sh) {
    if (!sh || !sh->plan) return;
    if (sh->key_src.size() != sh->plan->keys.size()) return;
    dai_show_plan *pl = sh->plan;

    // 1) out with the old stroke keys
    {
        size_t w = 0;
        bool dropped = false;
        std::vector<uint32_t> first(pl->drones + 1, 0);
        uint32_t d = 0;
        for (size_t k = 0; k < pl->keys.size(); ++k) {
            while (d < pl->drones && k >= pl->first[d + 1]) { first[d + 1] = (uint32_t)w; ++d; }
            if (sh->key_src[k].stroke_key) { dropped = true; continue; }
            pl->keys[w] = pl->keys[k];
            sh->key_src[w] = sh->key_src[k];
            ++w;
        }
        while (d < pl->drones) { first[d + 1] = (uint32_t)w; ++d; }
        if (dropped) {
            pl->keys.resize(w);
            sh->key_src.resize(w);
            pl->first = first;
        }
    }

    // 2) every solver key re-mixed at its own second
    recolour(sh);

    // 3) in with the new stroke keys, per drone, in time order
    bool any = false;
    for (size_t i = 0; i < sh->forms.size(); ++i) any |= !sh->forms[i].strokes.empty();
    if (!any) return;

    std::vector<dai_show_key>  nk;
    std::vector<KeySrc>        ns;
    std::vector<uint32_t>      first(pl->drones + 1, 0);
    nk.reserve(pl->keys.size() + 64);
    ns.reserve(pl->keys.size() + 64);
    for (uint32_t d = 0; d < pl->drones; ++d) {
        uint32_t b = pl->first[d], e = pl->first[d + 1];
        first[d] = (uint32_t)nk.size();
        // Collect this drone's insertions.
        struct Ins { float t; dai_show_key kold, knew; KeySrc src; };
        std::vector<Ins> ins;
        for (uint32_t fi = 0; fi < (uint32_t)sh->forms.size(); ++fi) {
            const Formation &f = sh->forms[fi];
            for (size_t si = 0; si < f.strokes.size(); ++si) {
                const Formation::Stroke &st = f.strokes[si];
                if (e == b || st.t <= pl->keys[b].t || st.t >= pl->keys[e - 1].t) continue;
                uint32_t lo = b, hi = e - 1;
                while (lo + 1 < hi) {
                    uint32_t mid = lo + (hi - lo) / 2;
                    if (pl->keys[mid].t <= st.t) lo = mid; else hi = mid;
                }
                const KeySrc &src = sh->key_src[lo];
                if (src.stroke_key || src.form != fi) continue;
                bool hits = false;
                for (size_t q = 0; q < st.idx.size() && !hits; ++q)
                    hits = (st.idx[q] == src.pb);
                if (!hits) continue;
                Ins one;
                one.t = st.t;
                one.src = src;
                one.src.stroke_key = 1;
                dai_show_plan_sample(pl, d, st.t, &one.kold.p);
                one.knew = one.kold;
                one.kold.t = st.t; one.knew.t = st.t;
                one.kold.profile = DAI_SHOW_PROFILE_LINEAR;
                one.knew.profile = DAI_SHOW_PROFILE_LINEAR;
                key_colour(sh, src, st.t - 1e-3f,
                           &one.kold.p.r, &one.kold.p.g, &one.kold.p.b, &one.kold.p.w);
                key_colour(sh, src, st.t,
                           &one.knew.p.r, &one.knew.p.g, &one.knew.p.b, &one.knew.p.w);
                ins.push_back(one);
            }
        }
        std::stable_sort(ins.begin(), ins.end(),
                         [](const Ins &a, const Ins &b) { return a.t < b.t; });
        size_t ii = 0;
        for (uint32_t k = b; k < e; ++k) {
            while (ii < ins.size() && ins[ii].t <= pl->keys[k].t) {
                nk.push_back(ins[ii].kold); ns.push_back(ins[ii].src);
                nk.push_back(ins[ii].knew); ns.push_back(ins[ii].src);
                ++ii;
            }
            nk.push_back(pl->keys[k]); ns.push_back(sh->key_src[k]);
        }
        while (ii < ins.size()) {
            nk.push_back(ins[ii].kold); ns.push_back(ins[ii].src);
            nk.push_back(ins[ii].knew); ns.push_back(ins[ii].src);
            ++ii;
        }
    }
    first[pl->drones] = (uint32_t)nk.size();
    pl->keys.swap(nk);
    sh->key_src.swap(ns);
    pl->first = first;
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
    if (n == 0) return UINT32_MAX;
    // Any count goes in now; whether the counts ADD UP is the solver's
    // question, asked per group against the fleet size - that is where the
    // answer "no drone may be left in the air" lives since groups exist.
    Formation f;
    f.name   = name ? name : "formation";
    f.source = source ? source : "";
    // What comes in is where the figure stands today, so it becomes the LOCAL
    // shape and the transform starts at identity - which means the world cache
    // is a copy of it and this call behaves exactly as it did before there was
    // a transform at all. The pivot goes to the figure's own centre so the
    // first thing a user does with the gizmo (scale) does the expected thing.
    f.local.assign(pts, pts + n);
    f.xf    = dai_show_transform_identity();
    f.pivot = centroid_of(f.local);
    rebuild(f);
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

    // The safety distance belongs to the document, not to whoever filled in
    // the descriptor. The COUNT may be the caller's - groups fly slices of
    // the fleet, so a figure no longer has to be the whole sky - and only
    // defaults to the full fleet when the caller did not ask.
    dai_show_sample_desc d = *desc;
    if (d.count == 0) d.count = sh->s.drone_count;
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

uint32_t dai_show_image_sample(dai_show *sh,
                               const uint8_t *rgba, uint32_t w, uint32_t h,
                               float width_m, uint8_t threshold,
                               uint32_t count, dai_show_point *out,
                               char *err, size_t err_len) {
    if (err && err_len) err[0] = 0;
    if (!sh || !rgba || !w || !h || !out) return 0;
    if (count == 0) count = sh->s.drone_count;
    if (count == 0) { fail(err, err_len, "the fleet is empty - set a drone count first"); return 0; }
    const float cell = sh->s.min_distance_m;
    if (!(cell > 0.0f) || !(width_m >= cell)) {
        fail(err, err_len, "the figure is %.1f m wide but a drone needs %.1f m",
             (double)width_m, (double)cell);
        return 0;
    }
    const float height_m = width_m * (float)h / (float)w;
    const uint32_t cols = (uint32_t)(width_m / cell);
    const uint32_t rows = (uint32_t)(height_m / cell);
    if (!cols || !rows) {
        fail(err, err_len, "the figure is too small for one lit cell");
        return 0;
    }
    const float cw = width_m / (float)cols;
    const float ch = height_m / (float)rows;

    struct Cand { float x, y; uint8_t r, g, b, lum; };
    std::vector<Cand> lit;
    lit.reserve((size_t)cols * rows / 2);
    for (uint32_t cy = 0; cy < rows; ++cy) {
        for (uint32_t cx = 0; cx < cols; ++cx) {
            uint32_t pxx = (uint32_t)(((float)cx + 0.5f) / (float)cols * (float)w);
            uint32_t pxy = (uint32_t)(((float)cy + 0.5f) / (float)rows * (float)h);
            if (pxx >= w) pxx = w - 1;
            if (pxy >= h) pxy = h - 1;
            const uint8_t *px = rgba + ((size_t)pxy * w + pxx) * 4;
            uint8_t lum = (uint8_t)((px[0] * 299u + px[1] * 587u + px[2] * 114u) / 1000u);
            if (lum < threshold) continue;
            Cand c;
            c.x = -width_m * 0.5f + ((float)cx + 0.5f) * cw;
            // Image row 0 is the TOP; the sky's y grows upwards. The figure
            // stands with its foot at the take-off altitude, like every
            // builtin does.
            c.y = sh->s.takeoff_alt_m + height_m - ((float)cy + 0.5f) * ch;
            c.r = px[0]; c.g = px[1]; c.b = px[2]; c.lum = lum;
            lit.push_back(c);
        }
    }
    if ((uint32_t)lit.size() < count) {
        fail(err, err_len,
             "the image lights %u cells at %.1f m spacing, but %u drones are asked for - "
             "lower the threshold or widen the figure",
             (unsigned)lit.size(), (double)cell, (unsigned)count);
        return 0;
    }
    if ((uint32_t)lit.size() > count) {
        // The brightest cells survive: a dim edge sacrificed keeps the shape
        // readable, a random one leaves holes in the middle of a logo.
        std::stable_sort(lit.begin(), lit.end(),
                         [](const Cand &a, const Cand &b) { return a.lum > b.lum; });
        lit.resize(count);
    }
    for (size_t k = 0; k < lit.size(); ++k) {
        out[k].x = lit[k].x; out[k].y = lit[k].y; out[k].z = 0.0f;
        out[k].r = lit[k].r; out[k].g = lit[k].g; out[k].b = lit[k].b; out[k].w = 0;
    }
    return (uint32_t)lit.size();
}

uint32_t dai_show_formation_from_image(dai_show *sh, const char *name,
                                       const char *source,
                                       const uint8_t *rgba, uint32_t w, uint32_t h,
                                       float width_m, uint8_t threshold,
                                       uint32_t count, char *err, size_t err_len) {
    if (err && err_len) err[0] = 0;
    if (!sh) return UINT32_MAX;
    uint32_t want = count ? count : sh->s.drone_count;
    std::vector<dai_show_point> pts(want ? want : 1);
    uint32_t got = dai_show_image_sample(sh, rgba, w, h, width_m, threshold,
                                         want, pts.data(), err, err_len);
    if (!got) return UINT32_MAX;
    uint32_t idx = dai_show_formation_add(sh, name, source, pts.data(), got, 4.0f);
    if (idx != UINT32_MAX) sh->forms[idx].sample_mode = DAI_SHOW_SAMPLE_SILHOUETTE;
    return idx;
}

uint32_t dai_show_add_takeoff_grid(dai_show *sh, int group, float spacing,
                                   char *err, size_t err_len) {
    if (err && err_len) err[0] = 0;
    if (!sh || group < 0) return UINT32_MAX;
    // How many drones this step flies: whatever its figures already fly. A
    // grid of a different size would break the group audit at solve, which is
    // a worse way to learn the same thing.
    uint32_t count = 0;
    uint32_t first_of_group = UINT32_MAX;
    for (uint32_t i = 0; i < (uint32_t)sh->forms.size(); ++i) {
        if (sh->forms[i].group != group) continue;
        if (first_of_group == UINT32_MAX) first_of_group = i;
        count = (uint32_t)sh->forms[i].pts.size();
        break;
    }
    if (!count) count = sh->s.drone_count;
    if (!count) { fail(err, err_len, "the fleet is empty - set a drone count first"); return UINT32_MAX; }

    float gap = (spacing > 0.0f) ? spacing : sh->s.min_distance_m * 1.5f;
    if (gap < sh->s.min_distance_m) gap = sh->s.min_distance_m;
    // A square as near as the count allows: rows that differ by one are what
    // a ground crew actually tapes out.
    uint32_t side = 1;
    while (side * side < count) ++side;
    const float y = (sh->s.min_ground_m > 0.0f) ? sh->s.min_ground_m : 0.0f;
    std::vector<dai_show_point> pts(count);
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t gx = i % side, gz = i / side;
        pts[i].x = ((float)gx - (float)(side - 1) * 0.5f) * gap;
        pts[i].y = y;
        pts[i].z = ((float)gz - (float)(side - 1) * 0.5f) * gap;
        pts[i].r = 40; pts[i].g = 40; pts[i].b = 40; pts[i].w = 0;   // dark on the ground
    }
    uint32_t idx = dai_show_formation_add(sh, "Takeoff grid", "builtin://takeoff",
                                          pts.data(), count, 2.0f);
    if (idx == UINT32_MAX) return idx;
    sh->forms[idx].group = group;
    // To the FRONT of its step: a launch pad that is not first is a landing.
    if (first_of_group != UINT32_MAX && idx > first_of_group)
        dai_show_formation_move(sh, idx, (int)first_of_group - (int)idx);
    dai_show_plan_destroy(sh->plan);
    sh->plan = nullptr;
    sh->conflicts.clear();
    sh->tr_stats.clear();
    return (first_of_group != UINT32_MAX) ? first_of_group : idx;
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
    out->xf          = f.xf;
    out->pivot       = f.pivot;
    out->colour_override = f.colour_override;
    out->colour[0] = f.ovr[0]; out->colour[1] = f.ovr[1];
    out->colour[2] = f.ovr[2]; out->colour[3] = f.ovr[3];
    out->group = f.group;
    return 1;
}

int dai_show_drone_point_at(const dai_show *sh, uint32_t drone, float t,
                            uint32_t *formation, uint32_t *point, int *settled) {
    if (formation) *formation = UINT32_MAX;
    if (point)     *point     = UINT32_MAX;
    if (settled)   *settled   = 0;
    if (!sh || !sh->plan || drone >= sh->plan->drones) return 0;
    if (sh->key_src.size() != sh->plan->keys.size()) return 0;
    const dai_show_plan *p = sh->plan;
    uint32_t b = p->first[drone], e = p->first[drone + 1];
    if (e == b) return 0;

    // The same bracket dai_show_plan_sample finds, so the answer names the
    // very segment the drawn dot was interpolated on. Anything else would
    // point at a different drone than the one under the cursor.
    uint32_t lo, hi;
    if (t <= p->keys[b].t)          { lo = b;     hi = b; }
    else if (t >= p->keys[e - 1].t) { lo = e - 1; hi = e - 1; }
    else {
        lo = b; hi = e - 1;
        while (lo + 1 < hi) {
            uint32_t mid = lo + (hi - lo) / 2;
            if (p->keys[mid].t <= t) lo = mid; else hi = mid;
        }
    }
    const KeySrc &dst = sh->key_src[hi];
    const KeySrc &src = sh->key_src[lo];
    if (dst.form >= sh->forms.size()) return 0;
    if (formation) *formation = dst.form;
    if (point)     *point     = dst.pb;
    // Standing still: both ends of the bracket are the same point of the same
    // figure, fully arrived. Mid-flight the point is a destination, not a
    // place, and the caller has to be told the difference.
    if (settled)
        *settled = (src.form == dst.form && src.pb == dst.pb &&
                    src.s >= 1.0f && dst.s >= 1.0f) ? 1 : 0;
    return 1;
}

uint32_t dai_show_formation_at_time(const dai_show *sh, float t) {
    if (!sh || sh->forms.empty() || !sh->plan) return UINT32_MAX;
    // A moment belongs to the formation it is going TO: the transition into a
    // figure and the hold of that figure are one entry in the storyboard, and
    // that is the entry a director opens when the validator complains. The last
    // formation keeps everything after it, so a conflict at the very end of the
    // show is never left without a place.
    for (size_t i = 0; i < sh->forms.size(); ++i)
        if (t <= sh->forms[i].t_start + sh->forms[i].hold_s + 1e-4f) return (uint32_t)i;
    return (uint32_t)(sh->forms.size() - 1);
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

int dai_show_formation_set_group(dai_show *sh, uint32_t i, int group) {
    if (!sh || i >= sh->forms.size() || group < 0) return 0;
    if (sh->forms[i].group == group) return 1;
    sh->forms[i].group = group;
    dai_show_plan_destroy(sh->plan);
    sh->plan = nullptr;
    sh->conflicts.clear();
    sh->tr_stats.clear();
    return 1;
}

int dai_show_formation_replace_points(dai_show *sh, uint32_t i,
                                      const dai_show_point *pts, uint32_t n) {
    if (!sh || i >= sh->forms.size() || !pts || n == 0) return 0;
    Formation &f = sh->forms[i];
    f.local.assign(pts, pts + n);
    // The pivot was the centroid of the OLD shape; the new one gets its own,
    // or a resampled sphere would scale about the ghost of the old one.
    f.pivot = centroid_of(f.local);
    rebuild(f);
    dai_show_plan_destroy(sh->plan);
    sh->plan = nullptr;
    sh->conflicts.clear();
    sh->tr_stats.clear();
    return 1;
}

/* ---- moving a figure, and colouring it ------------------------------------ */

dai_show_transform dai_show_transform_identity(void) {
    dai_show_transform t;
    t.position     = dai_vec3{ 0.0f, 0.0f, 0.0f };
    t.rotation_deg = dai_vec3{ 0.0f, 0.0f, 0.0f };
    t.scale        = dai_vec3{ 1.0f, 1.0f, 1.0f };
    return t;
}

int dai_show_transform_is_identity(const dai_show_transform *t) {
    if (!t) return 0;
    return t->position.x == 0.0f && t->position.y == 0.0f && t->position.z == 0.0f &&
           t->rotation_deg.x == 0.0f && t->rotation_deg.y == 0.0f && t->rotation_deg.z == 0.0f &&
           t->scale.x == 1.0f && t->scale.y == 1.0f && t->scale.z == 1.0f;
}

int dai_show_formation_get_transform(const dai_show *sh, uint32_t i, dai_show_transform *out) {
    if (!sh || !out || i >= sh->forms.size()) return 0;
    *out = sh->forms[i].xf;
    return 1;
}

int dai_show_formation_set_transform(dai_show *sh, uint32_t i, const dai_show_transform *xf) {
    if (!sh || !xf || i >= sh->forms.size()) return 0;
    // A zero scale is not a very small figure, it is every drone in the fleet
    // at one coordinate - the collision the sampler exists to refuse, arrived
    // at through the inspector. Refused here rather than reported later.
    if (!(xf->scale.x != 0.0f) || !(xf->scale.y != 0.0f) || !(xf->scale.z != 0.0f)) return 0;
    if (xf->scale.x != xf->scale.x || xf->scale.y != xf->scale.y || xf->scale.z != xf->scale.z) return 0;
    if (xf->position.x != xf->position.x || xf->position.y != xf->position.y ||
        xf->position.z != xf->position.z) return 0;
    if (xf->rotation_deg.x != xf->rotation_deg.x || xf->rotation_deg.y != xf->rotation_deg.y ||
        xf->rotation_deg.z != xf->rotation_deg.z) return 0;
    sh->forms[i].xf = *xf;
    rebuild(sh->forms[i]);
    // Where a drone flies has changed, so the plan and its verdict are gone -
    // the same rule a new hold time or a new minimum distance follows.
    dai_show_plan_destroy(sh->plan);
    sh->plan = nullptr;
    sh->conflicts.clear();
    sh->tr_stats.clear();
    return 1;
}

const dai_show_point *dai_show_formation_local_points(const dai_show *sh, uint32_t i) {
    if (!sh || i >= sh->forms.size() || sh->forms[i].local.empty()) return nullptr;
    return sh->forms[i].local.data();
}

float dai_show_formation_min_spacing(const dai_show *sh, uint32_t i) {
    if (!sh || i >= sh->forms.size()) return -1.0f;
    const std::vector<dai_show_point> &p = sh->forms[i].pts;
    const size_t n = p.size();
    if (n < 2) return -1.0f;

    // A uniform grid, cell by cell, so only the 27 neighbouring buckets are ever
    // tested. The same shape the validator uses and for the same reason: this is
    // called while a scale is being DRAGGED, and ten thousand squared per frame
    // is 10^8 distance tests for one number in a panel.
    //
    // The subtlety, and it cost a failing test to find: a grid of cell c can
    // only be TRUSTED about pairs closer than c. Two points three cells apart
    // are never compared, so a figure whose drones stand well beyond the cell
    // size returns "found nothing" - which is not an answer, it is silence
    // wearing an answer's clothes. So the cell starts at the average spacing
    // the point count implies and DOUBLES until the closest pair it finds is
    // inside one cell; at that moment the result is exact, because any pair
    // closer than a cell is guaranteed to have been looked at.
    float lo[3] = { p[0].x, p[0].y, p[0].z }, hi[3] = { p[0].x, p[0].y, p[0].z };
    for (size_t k = 1; k < n; ++k) {
        if (p[k].x < lo[0]) lo[0] = p[k].x;
        if (p[k].x > hi[0]) hi[0] = p[k].x;
        if (p[k].y < lo[1]) lo[1] = p[k].y;
        if (p[k].y > hi[1]) hi[1] = p[k].y;
        if (p[k].z < lo[2]) lo[2] = p[k].z;
        if (p[k].z > hi[2]) hi[2] = p[k].z;
    }
    float ext = hi[0] - lo[0];
    if (hi[1] - lo[1] > ext) ext = hi[1] - lo[1];
    if (hi[2] - lo[2] > ext) ext = hi[2] - lo[2];
    if (!(ext > 0.0f)) return 0.0f;                 // every point in one place

    float cell = ext / (float)std::max(1.0, std::cbrt((double)n));
    if (!(cell > 1e-6f)) cell = ext;

    auto hash_of = [](int32_t x, int32_t y, int32_t z) -> uint64_t {
        return ((uint64_t)(uint32_t)x * 73856093ull) ^
               ((uint64_t)(uint32_t)y * 19349663ull) ^
               ((uint64_t)(uint32_t)z * 83492791ull);
    };
    std::unordered_map<uint64_t, std::vector<uint32_t> > grid;
    std::vector<int32_t> kx(n), ky(n), kz(n);

    // The bound: doubling from the average spacing reaches the diagonal of the
    // figure in a handful of rounds, and at the diagonal every point shares one
    // bucket - so the last round is an exhaustive check rather than a give up.
    for (int round = 0; round < 40; ++round) {
        const float inv = 1.0f / cell;
        grid.clear();
        grid.reserve(n * 2);
        for (size_t k = 0; k < n; ++k) {
            kx[k] = (int32_t)std::floor((p[k].x - lo[0]) * inv);
            ky[k] = (int32_t)std::floor((p[k].y - lo[1]) * inv);
            kz[k] = (int32_t)std::floor((p[k].z - lo[2]) * inv);
            grid[hash_of(kx[k], ky[k], kz[k])].push_back((uint32_t)k);
        }

        double best = -1.0;
        for (size_t k = 0; k < n; ++k) {
            for (int dx = -1; dx <= 1; ++dx)
            for (int dy = -1; dy <= 1; ++dy)
            for (int dz = -1; dz <= 1; ++dz) {
                std::unordered_map<uint64_t, std::vector<uint32_t> >::const_iterator it =
                    grid.find(hash_of(kx[k] + dx, ky[k] + dy, kz[k] + dz));
                if (it == grid.end()) continue;
                const std::vector<uint32_t> &b = it->second;
                for (size_t m = 0; m < b.size(); ++m) {
                    uint32_t j = b[m];
                    if ((size_t)j <= k) continue;     // each pair once, lower index first
                    double ddx = (double)p[j].x - (double)p[k].x;
                    double ddy = (double)p[j].y - (double)p[k].y;
                    double ddz = (double)p[j].z - (double)p[k].z;
                    double d2 = ddx * ddx + ddy * ddy + ddz * ddz;
                    if (best < 0.0 || d2 < best) best = d2;
                }
            }
        }
        // A hash collision only ever ADDS candidates, it never hides a pair, so
        // an answer that is inside one cell is the exact answer and not an
        // estimate of it.
        if (best >= 0.0 && std::sqrt(best) <= (double)cell) return (float)std::sqrt(best);
        if (cell > ext * 2.0f) return best < 0.0 ? -1.0f : (float)std::sqrt(best);
        cell *= 2.0f;
    }
    return -1.0f;
}

int dai_show_formation_get_colour(const dai_show *sh, uint32_t i, int *on, uint8_t rgbw[4]) {
    if (!sh || i >= sh->forms.size()) return 0;
    const Formation &f = sh->forms[i];
    if (on) *on = f.colour_override;
    if (rgbw) { rgbw[0] = f.ovr[0]; rgbw[1] = f.ovr[1]; rgbw[2] = f.ovr[2]; rgbw[3] = f.ovr[3]; }
    return 1;
}

int dai_show_formation_set_colour(dai_show *sh, uint32_t i, int on,
                                  uint8_t r, uint8_t g, uint8_t b, uint8_t w) {
    if (!sh || i >= sh->forms.size()) return 0;
    Formation &f = sh->forms[i];
    f.colour_override = on ? 1 : 0;
    f.ovr[0] = r; f.ovr[1] = g; f.ovr[2] = b; f.ovr[3] = w;
    rebuild(f);
    // Colour changes no geometry, so the PLAN survives - but the colours it
    // carries came from the formation, so they have to be refreshed. Cheaper
    // and more honest than throwing a solve away over a swatch: the keyframes
    // hold the colour, so they are the thing that is updated.
    refresh_colours(sh);
    return 1;
}

int dai_show_formation_set_point_colour(dai_show *sh, uint32_t i, const uint32_t *idx,
                                        uint32_t n, uint8_t r, uint8_t g, uint8_t b, uint8_t w) {
    if (!sh || i >= sh->forms.size()) return 0;
    Formation &f = sh->forms[i];
    const uint32_t count = (uint32_t)f.local.size();
    // Painting under an override is an edit nobody can see. So the override is
    // FLATTENED into the points first and switched off: nothing on screen
    // moves, nothing is lost, and the next click paints what it says it paints.
    if (f.colour_override) {
        for (uint32_t k = 0; k < count; ++k) {
            f.local[k].r = f.ovr[0]; f.local[k].g = f.ovr[1];
            f.local[k].b = f.ovr[2]; f.local[k].w = f.ovr[3];
        }
        f.colour_override = 0;
    }
    if (!idx) {
        for (uint32_t k = 0; k < count; ++k) {
            f.local[k].r = r; f.local[k].g = g; f.local[k].b = b; f.local[k].w = w;
        }
    } else {
        for (uint32_t m = 0; m < n; ++m) {
            uint32_t k = idx[m];
            if (k >= count) continue;
            f.local[k].r = r; f.local[k].g = g; f.local[k].b = b; f.local[k].w = w;
        }
    }
    rebuild(f);
    refresh_colours(sh);
    return 1;
}

int dai_show_formation_set_point_world(dai_show *sh, uint32_t i, uint32_t point,
                                       float wx, float wy, float wz) {
    if (!sh || i >= sh->forms.size()) return 0;
    Formation &f = sh->forms[i];
    if (point >= f.local.size()) return 0;
    if (wx != wx || wy != wy || wz != wz) return 0;
    // World to local is the inverse of what rebuild() runs: subtract the
    // position and the pivot, undo the rotation (R^T, because R is orthogonal),
    // undo the scale, add the pivot back. A scale of zero never reaches this
    // line - set_transform refuses it - but a loaded file is not set_transform,
    // so the division is guarded all the same.
    if (!(f.xf.scale.x != 0.0f) || !(f.xf.scale.y != 0.0f) || !(f.xf.scale.z != 0.0f)) return 0;
    const float deg = (float)(3.14159265358979323846 / 180.0);
    const float rx = f.xf.rotation_deg.x * deg, ry = f.xf.rotation_deg.y * deg,
                rz = f.xf.rotation_deg.z * deg;
    const float cx = std::cos(rx), sx = std::sin(rx);
    const float cy = std::cos(ry), sy = std::sin(ry);
    const float cz = std::cos(rz), sz = std::sin(rz);
    // R = Rz*Ry*Rx, as in rebuild(). R^T undoes it.
    const float m00 =  cz * cy;
    const float m01 =  cz * sy * sx - sz * cx;
    const float m02 =  cz * sy * cx + sz * sx;
    const float m10 =  sz * cy;
    const float m11 =  sz * sy * sx + cz * cx;
    const float m12 =  sz * sy * cx - cz * sx;
    const float m20 = -sy;
    const float m21 =  cy * sx;
    const float m22 =  cy * cx;
    const float dx = wx - f.xf.position.x - f.pivot.x;
    const float dy = wy - f.xf.position.y - f.pivot.y;
    const float dz = wz - f.xf.position.z - f.pivot.z;
    dai_show_point &l = f.local[point];
    l.x = f.pivot.x + (m00 * dx + m10 * dy + m20 * dz) / f.xf.scale.x;
    l.y = f.pivot.y + (m01 * dx + m11 * dy + m21 * dz) / f.xf.scale.y;
    l.z = f.pivot.z + (m02 * dx + m12 * dy + m22 * dz) / f.xf.scale.z;
    rebuild(f);
    dai_show_plan_destroy(sh->plan);
    sh->plan = nullptr;
    sh->conflicts.clear();
    sh->tr_stats.clear();
    return 1;
}

int dai_show_formation_paint_at(dai_show *sh, uint32_t i, float t,
                                const uint32_t *idx, uint32_t n,
                                uint8_t r, uint8_t g, uint8_t b, uint8_t w) {
    if (!sh || i >= sh->forms.size() || !idx || !n || !(t >= 0.0f)) return 0;
    Formation &f = sh->forms[i];
    Formation::Stroke st;
    st.t = t;
    st.rgba[0] = r; st.rgba[1] = g; st.rgba[2] = b; st.rgba[3] = w;
    st.idx.assign(idx, idx + n);
    // Out-of-range points are dropped now rather than at every recolour.
    uint32_t count = (uint32_t)f.local.size();
    st.idx.erase(std::remove_if(st.idx.begin(), st.idx.end(),
                                [count](uint32_t k) { return k >= count; }),
                 st.idx.end());
    if (st.idx.empty()) return 0;
    f.strokes.push_back(st);
    // The plan SURVIVES a stroke - no geometry moved - but its colours are
    // re-mixed, the same deal a repaint always had.
    refresh_colours(sh);
    return 1;
}

int dai_show_formation_clear_strokes(dai_show *sh, uint32_t i) {
    if (!sh || i >= sh->forms.size()) return 0;
    if (sh->forms[i].strokes.empty()) return 1;
    sh->forms[i].strokes.clear();
    refresh_colours(sh);
    return 1;
}

int dai_show_formation_set_sample_mode(dai_show *sh, uint32_t i, int mode) {
    if (!sh || i >= sh->forms.size()) return 0;
    sh->forms[i].sample_mode = mode;
    return 1;   // a label about how the points were made; geometry untouched
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
    // ---- the group audit ----------------------------------------------------
    // The storyboard is one list, but the show it describes can be several
    // timelines at once: formations that share a group id fly one after
    // another, as they always have, and two different groups fly at the same
    // time, each over its own slice of the fleet. The slices are handed out
    // in group id order, so which drone numbers a group owns stays stable
    // from solve to solve as long as the counts do.
    std::vector<int> gids;
    for (size_t i = 0; i < sh->forms.size(); ++i) {
        int g = sh->forms[i].group;
        if (std::find(gids.begin(), gids.end(), g) == gids.end()) gids.push_back(g);
    }
    std::sort(gids.begin(), gids.end());

    std::vector<std::vector<uint32_t> > members(gids.size());
    std::vector<uint32_t> goff(gids.size()), gcount(gids.size());
    uint32_t accounted = 0;
    for (size_t gi = 0; gi < gids.size(); ++gi) {
        for (uint32_t i = 0; i < (uint32_t)sh->forms.size(); ++i)
            if (sh->forms[i].group == gids[gi]) members[gi].push_back(i);
        uint32_t c = (uint32_t)sh->forms[members[gi][0]].pts.size();
        for (size_t k = 1; k < members[gi].size(); ++k) {
            const Formation &mf = sh->forms[members[gi][k]];
            if (mf.pts.size() != c) {
                fail(err, err_len,
                     "'%s' has %u points, but group %d flies %u - one group, one count",
                     mf.name.c_str(), (unsigned)mf.pts.size(), gids[gi], (unsigned)c);
                return DAI_ERR_STATE;
            }
        }
        goff[gi]   = accounted;
        gcount[gi] = c;
        accounted += c;
    }
    if (accounted != n) {
        fail(err, err_len, "the groups fly %u drones between them, the fleet is %u",
             (unsigned)accounted, (unsigned)n);
        return DAI_ERR_STATE;
    }

    std::vector<std::vector<dai_show_key> > keys((size_t)n);
    // Parallel to `keys`, and filled at every single push below: where each
    // key's colour was mixed from. It costs one struct per keyframe and it buys
    // a colour edit that does not throw the solve away.
    std::vector<std::vector<KeySrc> >       ksrc((size_t)n);
    sh->key_src.clear();

    uint32_t unresolved_total = 0, endpoint_total = 0;
    int      endpoint_named   = 0;
    char     endpoint_note[192] = { 0 };

    // Every group is its own little show over its own drone numbers, with its
    // own cursor on the timeline - which is what "the groups fly at the same
    // time" means in practice: nobody waits for anybody.
    for (size_t gi = 0; gi < gids.size(); ++gi) {
    const uint32_t off = goff[gi];
    const uint32_t ng  = gcount[gi];
    const std::vector<uint32_t> &mem = members[gi];

    std::vector<uint32_t>       cur_pt(ng);     // which point of the current figure
    std::vector<dai_show_point> cur(sh->forms[mem[0]].pts);
    std::vector<dai_show_point> nxt(ng);
    std::vector<uint32_t>       perm(ng);
    std::vector<dai_show_leg>   legs(ng);

    for (uint32_t d = 0; d < ng; ++d) {
        keys[off + d].reserve(mem.size() * 3 + 2);
        ksrc[off + d].reserve(mem.size() * 3 + 2);
        cur_pt[d] = d;                  // a group's first figure is flown in order
        dai_show_key k;
        k.t = 0.0f; k.p = cur[d]; k.profile = DAI_SHOW_PROFILE_LINEAR;
        keys[off + d].push_back(k);
        ksrc[off + d].push_back(KeySrc{ mem[0], mem[0], d, d, 1.0f });
    }

    float t_cursor = sh->forms[mem[0]].hold_s;
    sh->forms[mem[0]].t_start = 0.0f;
    if (t_cursor > 0.0f) {
        for (uint32_t d = 0; d < ng; ++d) {
            dai_show_key k;
            k.t = t_cursor; k.p = cur[d]; k.profile = DAI_SHOW_PROFILE_LINEAR;
            keys[off + d].push_back(k);
            ksrc[off + d].push_back(KeySrc{ mem[0], mem[0], d, d, 1.0f });
        }
    }

    for (size_t mi = 1; mi < mem.size(); ++mi) {
        const uint32_t i = mem[mi];
        const uint32_t fprev = mem[mi - 1];
        Formation &f = sh->forms[i];
        for (uint32_t d = 0; d < ng; ++d) nxt[d] = f.pts[d];

        dai_show_transition tr = f.tr;

        dai_show_assign_stats astats;
        std::memset(&astats, 0, sizeof(astats));
        double t0 = now_ms();
        dai_result ar = dai_show_assign(cur.data(), nxt.data(), ng, tr.assign_method,
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
        for (uint32_t d = 0; d < ng; ++d) {
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
        dai_result lr = dai_show_layer(cur.data(), nxt.data(), ng, perm.data(), &tr,
                                       &sh->s, t_cursor, legs.data(), &lstats);
        t1 = now_ms();
        sh->timings.layer_ms += t1 - t0;
        lstats.solve_ms = t1 - t0;
        sh->tr_stats[i].layer = lstats;
        if (lr == DAI_ERR_STATE) unresolved_total += lstats.unresolved;
        // A formation that parks pairs inside min_distance is not a transition
        // the separator failed at - but it is a show that must not go out, so
        // it is carried up to the caller as its own answer with the place it
        // has to be fixed named: the formation, and the first pair in it.
        if (lstats.endpoint_pairs > 0) {
            endpoint_total += lstats.endpoint_pairs;
            if (!endpoint_named) {
                uint32_t a = 0, b = 0; float gap = 0.0f;
                if (first_endpoint_pair(nxt.data(), ng, sh->s.min_distance_m, &a, &b, &gap)) {
                    endpoint_named = 1;
                    std::snprintf(endpoint_note, sizeof(endpoint_note),
                                  "drones %u and %u stand %.2f m apart in '%s' - the floor is "
                                  "%.2f m", (unsigned)a, (unsigned)b, (double)gap,
                                  f.name.c_str(), (double)sh->s.min_distance_m);
                }
            }
        }

        // Stage 4 turns the legs into keyframes. The detour is a curve, so it
        // is cut into pieces small enough that the line the plan interpolates
        // is the curve the separator proved - a straight leg needs one key.
        t0 = now_ms();
        float t_arrive = legs[0].t_end;
        for (uint32_t d = 0; d < ng; ++d) t_arrive = std::max(t_arrive, legs[d].t_end);
        for (uint32_t d = 0; d < ng; ++d) {
            const dai_show_leg &g = legs[d];
            const dai_show_point a = cur[d];
            const dai_show_point b = nxt[perm[d]];
            const uint32_t pa = cur_pt[d], pb = perm[d];
            const uint32_t fi = (uint32_t)i;
            const float    span = g.t_end - g.t_start;
            dai_show_key k;
            if (g.t_start > keys[off + d].back().t) {    // staggered or delayed: wait first
                k.t = g.t_start; k.p = a; k.profile = DAI_SHOW_PROFILE_LINEAR;
                keys[off + d].push_back(k);
                ksrc[off + d].push_back(KeySrc{ fi, fprev, pa, pb, 0.0f });
            }
            uint32_t steps = daishow::leg_key_count(&g, &sh->s);
            for (uint32_t st = 1; st <= steps; ++st) {
                float u = (float)st / (float)steps;
                k.t = g.t_start + (g.t_end - g.t_start) * u;
                if (st == steps) { k.t = g.t_end; k.p = b; }
                else daishow::leg_point(&g, &a, &b, k.t, &k.p);
                k.profile = (steps == 1) ? g.profile : DAI_SHOW_PROFILE_LINEAR;
                keys[off + d].push_back(k);
                // The same fraction leg_point mixed the colour at, from the
                // same function - not a second opinion about the easing.
                float uu = (span > 0.0f) ? (k.t - g.t_start) / span : 1.0f;
                if (uu < 0.0f) uu = 0.0f;
                if (uu > 1.0f) uu = 1.0f;
                float mix = (st == steps) ? 1.0f : daishow::ease_profile(g.profile, uu);
                ksrc[off + d].push_back(KeySrc{ fi, fprev, pa, pb, mix });
            }
            if (t_arrive > g.t_end) {                    // hold formation until the last one lands
                k.t = t_arrive; k.p = b; k.profile = DAI_SHOW_PROFILE_LINEAR;
                keys[off + d].push_back(k);
                ksrc[off + d].push_back(KeySrc{ fi, fprev, pa, pb, 1.0f });
            }
            if (f.hold_s > 0.0f) {
                k.t = t_arrive + f.hold_s; k.p = b; k.profile = DAI_SHOW_PROFILE_LINEAR;
                keys[off + d].push_back(k);
                ksrc[off + d].push_back(KeySrc{ fi, fprev, pa, pb, 1.0f });
            }
            cur[d]    = b;
            cur_pt[d] = pb;
        }
        sh->timings.profile_ms += now_ms() - t0;

        f.t_start = t_arrive;
        t_cursor  = t_arrive + f.hold_s;
    }
    }                                          // the group's little show is planned

    aggregate(&sh->timings, sh->tr_stats);

    std::vector<uint32_t>      counts(n);
    std::vector<dai_show_key>  flat;
    size_t total = 0;
    for (uint32_t d = 0; d < n; ++d) { counts[d] = (uint32_t)keys[d].size(); total += counts[d]; }
    flat.reserve(total);
    sh->key_src.reserve(total);
    for (uint32_t d = 0; d < n; ++d) {
        flat.insert(flat.end(), keys[d].begin(), keys[d].end());
        sh->key_src.insert(sh->key_src.end(), ksrc[d].begin(), ksrc[d].end());
    }
    sh->plan = dai_show_plan_from_keys(n, counts.data(), flat.data());
    // Strokes are colour over time; the plan was built from static point
    // colours, so the colours are re-mixed against the clock and the stroke
    // key pairs go in. Without a single stroke the re-mix lands on exactly
    // the bytes the legs already carry - an unpainted show is untouched.
    refresh_colours(sh);

    if (unresolved_total > 0) {
        fail(err, err_len, "%u transition conflicts could not be separated", (unsigned)unresolved_total);
        return DAI_ERR_STATE;
    }
    if (endpoint_total > 0) {
        fail(err, err_len, "%u formation fault%s: %s", (unsigned)endpoint_total,
             (endpoint_total == 1) ? "" : "s",
             endpoint_named ? endpoint_note : "a formation stands inside min_distance");
        return DAI_SHOW_LAYER_FORMATION_FAULT;
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
        if (f.group != 0)      put(out, "  group %d\n", f.group);
        if (f.sample_mode != DAI_SHOW_SAMPLE_SURFACE) put(out, "  sample-mode %d\n", f.sample_mode);
        if (i > 0 && !tr_equal(f.tr, td))
            put(out, "  transition %s %d %d %s %d\n", fstr(f.tr.duration_s).c_str(),
                f.tr.profile, f.tr.timing, fstr(f.tr.stagger_s).c_str(), f.tr.assign_method);
        // Where the figure stands, and only when it has been moved: an
        // untouched formation writes no transform line at all, so every show
        // file written before this field existed re-reads identical.
        if (!dai_show_transform_is_identity(&f.xf)) {
            put(out, "  transform %s %s %s %s %s %s %s %s %s\n",
                fstr(f.xf.position.x).c_str(), fstr(f.xf.position.y).c_str(),
                fstr(f.xf.position.z).c_str(),
                fstr(f.xf.rotation_deg.x).c_str(), fstr(f.xf.rotation_deg.y).c_str(),
                fstr(f.xf.rotation_deg.z).c_str(),
                fstr(f.xf.scale.x).c_str(), fstr(f.xf.scale.y).c_str(),
                fstr(f.xf.scale.z).c_str());
            // Saved rather than recomputed, because the pivot is part of every
            // world coordinate a moved figure has: a centroid that shifted by
            // one ulp between two builds would move the whole show.
            put(out, "  pivot %s %s %s\n", fstr(f.pivot.x).c_str(),
                fstr(f.pivot.y).c_str(), fstr(f.pivot.z).c_str());
        }
        if (f.colour_override)
            put(out, "  colour %u %u %u %u\n", f.ovr[0], f.ovr[1], f.ovr[2], f.ovr[3]);
        for (size_t si = 0; si < f.strokes.size(); ++si) {
            const Formation::Stroke &st = f.strokes[si];
            put(out, "  stroke %s %u %u %u %u %u\n", fstr(st.t).c_str(),
                st.rgba[0], st.rgba[1], st.rgba[2], st.rgba[3], (unsigned)st.idx.size());
            std::string line;
            for (size_t k = 0; k < st.idx.size(); ++k) {
                char num[16];
                std::snprintf(num, sizeof(num), "%u ", (unsigned)st.idx[k]);
                line += num;
            }
            put(out, "%s\n", line.c_str());
        }
        // The points as a block, LOCAL - what came off the mesh. The world
        // positions are the transform applied to these and are never stored:
        // a file that carried both would be a file that can disagree with
        // itself. Ten thousand of them per formation is the one place in this
        // engine where a line of prose per property would be silly - seven
        // fields, one point, no keys.
        put(out, "  points %u\n", (unsigned)f.local.size());
        for (size_t k = 0; k < f.local.size(); ++k) {
            const dai_show_point &p = f.local[k];
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
        int   group = 0;
        int   mode = DAI_SHOW_SAMPLE_SURFACE;
        dai_show_transition tr = { 0.0f, 0, 0, 0.0f, 0 };
        bool  has_tr = false;
        std::vector<dai_show_point> pts;
        dai_show_transform xf = { { 0.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 0.0f }, { 1.0f, 1.0f, 1.0f } };
        bool     has_xf = false;
        dai_vec3 pivot = { 0.0f, 0.0f, 0.0f };
        bool     has_pivot = false;
        int      colour_on = 0;
        uint8_t  ovr[4] = { 255, 255, 255, 0 };
        struct LStroke { float t; uint8_t rgba[4]; std::vector<uint32_t> idx; };
        std::vector<LStroke> strokes;
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
        else if (tok == "group")       fm.group  = atoi(skip_ws(lp));
        else if (tok == "sample-mode") fm.mode   = atoi(skip_ws(lp));
        else if (tok == "transform") {
            char *e = nullptr;
            const char *q = skip_ws(lp);
            float v[9];
            int ok = 1;
            for (int c = 0; c < 9; ++c) {
                v[c] = (float)strtod(q, &e);
                if (e == q) { ok = 0; break; }
                q = e;
            }
            if (ok) {
                fm.xf.position     = dai_vec3{ v[0], v[1], v[2] };
                fm.xf.rotation_deg = dai_vec3{ v[3], v[4], v[5] };
                fm.xf.scale        = dai_vec3{ v[6], v[7], v[8] };
                fm.has_xf = true;
            }
        }
        else if (tok == "pivot") {
            char *e1 = nullptr, *e2 = nullptr;
            fm.pivot.x = (float)strtod(skip_ws(lp), &e1);
            fm.pivot.y = (float)strtod(e1, &e2);
            fm.pivot.z = (float)strtod(e2, nullptr);
            fm.has_pivot = true;
        }
        else if (tok == "colour") {
            char *e1 = nullptr, *e2 = nullptr, *e3 = nullptr;
            long a = strtol(skip_ws(lp), &e1, 10);
            long b = strtol(e1, &e2, 10);
            long c = strtol(e2, &e3, 10);
            long w = strtol(e3, nullptr, 10);
            fm.ovr[0] = (uint8_t)a; fm.ovr[1] = (uint8_t)b;
            fm.ovr[2] = (uint8_t)c; fm.ovr[3] = (uint8_t)w;
            fm.colour_on = 1;
        }
        else if (tok == "stroke") {
            char *e1 = nullptr, *e2 = nullptr, *e3 = nullptr, *e4 = nullptr, *e5 = nullptr;
            Loaded::LStroke st;
            st.t         = (float)strtod(skip_ws(lp), &e1);
            st.rgba[0]   = (uint8_t)strtol(e1, &e2, 10);
            st.rgba[1]   = (uint8_t)strtol(e2, &e3, 10);
            st.rgba[2]   = (uint8_t)strtol(e3, &e4, 10);
            st.rgba[3]   = (uint8_t)strtol(e4, &e5, 10);
            size_t count = strtoul(e5, nullptr, 10);
            if (++li >= lines.size()) {
                fail(err, err_len, "formation '%s': the stroke ends early", fm.name.c_str());
                return nullptr;
            }
            const char *q = lines[li].c_str();
            char *e = nullptr;
            for (size_t k = 0; k < count; ++k) {
                long v = strtol(q, &e, 10);
                if (e == q) break;
                st.idx.push_back((uint32_t)v);
                q = e;
            }
            fm.strokes.push_back(st);
        }
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
        sh->forms[idx].group       = fm.group;
        if (fm.has_tr) sh->forms[idx].tr = fm.tr;
        for (size_t si = 0; si < fm.strokes.size(); ++si) {
            const Loaded::LStroke &ls = fm.strokes[si];
            dai_show_formation_paint_at(sh, idx, ls.t, ls.idx.data(),
                                        (uint32_t)ls.idx.size(),
                                        ls.rgba[0], ls.rgba[1], ls.rgba[2], ls.rgba[3]);
        }
        // dai_show_formation_add already put the points in `local` and the
        // pivot at their centroid, which is exactly right for a file that
        // carries no transform - a version 1 show, or one nobody moved.
        if (fm.has_pivot) sh->forms[idx].pivot = fm.pivot;
        if (fm.has_xf)    sh->forms[idx].xf    = fm.xf;
        if (fm.colour_on) {
            sh->forms[idx].colour_override = 1;
            for (int c = 0; c < 4; ++c) sh->forms[idx].ovr[c] = fm.ovr[c];
        }
        if (fm.has_xf || fm.has_pivot || fm.colour_on) rebuild(sh->forms[idx]);
    }
    return sh;
}

} // extern "C"
