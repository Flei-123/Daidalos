// The drone show panels: storyboard, parameters, validation, timeline.
//
// Implements include/dai_show_ui.h. This is the one file that knows about both
// dai_show and dai_ui - the same division dai_editor / dai_editor_ui exists
// for, and for the same reason: the pipeline has to be drivable from a script,
// a test or another frontend without dragging an interface in.
//
// Immediate mode, like everything else here. The panels are rebuilt from the
// document every frame, so a formation deleted in the storyboard cannot leave
// a stale row in the validation list - which in this program is not a cosmetic
// bug but a conflict that has stopped being shown.

#include "dai_show_ui.h"

#include "dai_dock.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

inline uint32_t rgba(uint32_t r, uint32_t g, uint32_t b, uint32_t a) {
    return (a << 24) | (b << 16) | (g << 8) | r;
}

const uint32_t COL_BG       = rgba(0x10, 0x12, 0x18, 255);
const uint32_t COL_GRID     = rgba(0x2A, 0x30, 0x3C, 255);
const uint32_t COL_HORIZON  = rgba(0x3A, 0x46, 0x5C, 255);
const uint32_t COL_CONFLICT = rgba(0xFF, 0x3B, 0x30, 255);
const uint32_t COL_OK       = rgba(0x4C, 0xD9, 0x64, 255);
const uint32_t COL_WARN     = rgba(0xFF, 0xB0, 0x2E, 255);

const char *const SAMPLE_MODES[3] = { "Surface", "Volume", "Silhouette" };
const char *const ASSIGN_METHODS[4] = { "Auto", "Exact", "Auction", "Cluster" };
const char *const PROFILES[4] = { "Linear", "Smooth", "Ease out", "Ease in" };
const char *const TIMINGS[2]  = { "Sync", "Staggered" };
const char *const FIGURES[3]  = { "Sphere", "Cube", "Ring" };

const char *conflict_word(int kind) {
    switch (kind) {
    case DAI_SHOW_CONFLICT_DISTANCE: return "too close";
    case DAI_SHOW_CONFLICT_VMAX:     return "too fast";
    case DAI_SHOW_CONFLICT_AMAX:     return "accelerates too hard";
    case DAI_SHOW_CONFLICT_FENCE:    return "outside the fence";
    default:                         return "too low";
    }
}

// The built-in figures. A fresh droneshow project has no imported model yet,
// and a storyboard whose only button is greyed out teaches nobody anything -
// so three shapes are generated here, from the same triangle soup a glTF
// import would hand over. Same code path, no special case in the pipeline.
struct Soup {
    std::vector<float>    pos;
    std::vector<uint32_t> idx;
    void tri(const float *a, const float *b, const float *c) {
        const float *v[3] = { a, b, c };
        for (int i = 0; i < 3; ++i) {
            idx.push_back((uint32_t)(pos.size() / 3));
            pos.push_back(v[i][0]); pos.push_back(v[i][1]); pos.push_back(v[i][2]);
        }
    }
};

void figure_sphere(Soup &s, int rings, int segs) {
    for (int i = 0; i < rings; ++i) {
        float p0 = (float)M_PI * (float)i / (float)rings;
        float p1 = (float)M_PI * (float)(i + 1) / (float)rings;
        for (int j = 0; j < segs; ++j) {
            float t0 = 2.0f * (float)M_PI * (float)j / (float)segs;
            float t1 = 2.0f * (float)M_PI * (float)(j + 1) / (float)segs;
            float a[3] = { std::sin(p0) * std::cos(t0), std::cos(p0), std::sin(p0) * std::sin(t0) };
            float b[3] = { std::sin(p1) * std::cos(t0), std::cos(p1), std::sin(p1) * std::sin(t0) };
            float c[3] = { std::sin(p1) * std::cos(t1), std::cos(p1), std::sin(p1) * std::sin(t1) };
            float d[3] = { std::sin(p0) * std::cos(t1), std::cos(p0), std::sin(p0) * std::sin(t1) };
            s.tri(a, b, c); s.tri(a, c, d);
        }
    }
}

void figure_cube(Soup &s) {
    const float h = 1.0f;
    const float v[8][3] = {
        { -h, -h, -h }, {  h, -h, -h }, {  h,  h, -h }, { -h,  h, -h },
        { -h, -h,  h }, {  h, -h,  h }, {  h,  h,  h }, { -h,  h,  h }
    };
    const int f[12][3] = {
        {0,2,1},{0,3,2},{4,5,6},{4,6,7},{0,1,5},{0,5,4},
        {3,7,6},{3,6,2},{0,4,7},{0,7,3},{1,2,6},{1,6,5}
    };
    for (int i = 0; i < 12; ++i) s.tri(v[f[i][0]], v[f[i][1]], v[f[i][2]]);
}

void figure_ring(Soup &s, int segs, float thick) {
    for (int j = 0; j < segs; ++j) {
        float t0 = 2.0f * (float)M_PI * (float)j / (float)segs;
        float t1 = 2.0f * (float)M_PI * (float)(j + 1) / (float)segs;
        for (int k = 0; k < 12; ++k) {
            float u0 = 2.0f * (float)M_PI * (float)k / 12.0f;
            float u1 = 2.0f * (float)M_PI * (float)(k + 1) / 12.0f;
            auto pt = [&](float t, float u, float *o) {
                float rr = 1.0f + thick * std::cos(u);
                o[0] = rr * std::cos(t); o[1] = thick * std::sin(u); o[2] = rr * std::sin(t);
            };
            float a[3], b[3], c[3], d[3];
            pt(t0, u0, a); pt(t1, u0, b); pt(t1, u1, c); pt(t0, u1, d);
            s.tri(a, b, c); s.tri(a, c, d);
        }
    }
}

} // namespace

// ---------------------------------------------------------------------------

struct dai_show_ui {
    dai_show *sh = nullptr;

    // Playback. The clock is a number in this struct, fed from outside, and it
    // reaches nothing that computes - the fleet at time t is a pure lookup.
    float time    = 0.0f;
    int   playing = 0;

    // The orbit the preview is watched from. A show is watched from ONE
    // direction, so the camera starts where the audience stands: south of the
    // origin, looking slightly up.
    float yaw = 0.0f, pitch = 0.18f, dist = 260.0f;
    float focus_y = 60.0f;
    int   dragging = 0;
    float drag_x = 0.0f, drag_y = 0.0f;
    int   framed_for = -1;                 // drone count the camera was fitted to

    int      sel_formation = 0;
    int      sel_conflict  = -1;
    uint32_t sel_drone     = 0xFFFFFFFFu;
    uint32_t sel_drone_b   = 0xFFFFFFFFu;

    // The parameter panel edits a copy and pushes it into the document when a
    // field commits: dai_show_set_settings throws the plan away, and doing that
    // on every keystroke would make the panel unusable.
    dai_show_settings s;
    int      sample_mode   = DAI_SHOW_SAMPLE_SURFACE;
    int      assign_method = DAI_SHOW_ASSIGN_AUTO;
    int      figure        = 0;
    float    figure_size   = 60.0f;

    int                  have_mesh = 0;
    dai_show_sample_desc mesh;
    char                 source[128] = { 0 };

    char status[256] = { 0 };
    int  status_bad  = 0;

    std::vector<dai_show_point> fleet;      // the fleet at `time`, per frame
    std::vector<uint8_t>        flagged;    // in a conflict at `time`
};

namespace {

void say(dai_show_ui *u, int bad, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(u->status, sizeof(u->status), fmt, ap);
    va_end(ap);
    u->status_bad = bad;
}

float show_duration(const dai_show_ui *u) {
    const dai_show_plan *p = dai_show_get_plan(u->sh);
    float d = p ? dai_show_plan_duration(p) : 0.0f;
    return d > 0.0f ? d : 1.0f;
}

// Samples the fleet once per frame and marks everybody who is inside a
// conflict at this instant. The mark is what the viewport paints red - and it
// comes from the conflict list rather than from a second distance test, so
// what is drawn is exactly what was reported.
void refresh_fleet(dai_show_ui *u) {
    const dai_show_plan *p = dai_show_get_plan(u->sh);
    uint32_t n = p ? dai_show_plan_drone_count(p) : 0;
    u->fleet.assign(n, dai_show_point{});
    u->flagged.assign(n, 0);
    if (!n) return;
    dai_show_plan_sample_all(p, u->time, u->fleet.data());
    uint32_t cn = dai_show_conflict_count(u->sh);
    for (uint32_t i = 0; i < cn; ++i) {
        dai_show_conflict c;
        if (!dai_show_conflict_at(u->sh, i, &c)) continue;
        if (std::fabs(c.time_s - u->time) > 0.35f) continue;
        if (c.a < n) u->flagged[c.a] = 1;
        if (c.b < n) u->flagged[c.b] = 1;
    }
}

struct Cam {
    float ex, ey, ez;         // eye
    float rx[3], ry[3], rz[3];// basis: right, up, forward
    float f;                  // focal length in pixels
    float cx, cy;
};

Cam camera_of(const dai_show_ui *u, float x, float y, float w, float h) {
    Cam c;
    float cp = std::cos(u->pitch), sp = std::sin(u->pitch);
    float cy_ = std::cos(u->yaw),  sy = std::sin(u->yaw);
    float fwd[3] = { -sy * cp, -sp, -cy_ * cp };    // looking towards the origin
    c.ex = -fwd[0] * u->dist;
    c.ey = u->focus_y - fwd[1] * u->dist;
    c.ez = -fwd[2] * u->dist;
    float up[3] = { 0.0f, 1.0f, 0.0f };
    float r[3] = { fwd[1] * up[2] - fwd[2] * up[1],
                   fwd[2] * up[0] - fwd[0] * up[2],
                   fwd[0] * up[1] - fwd[1] * up[0] };
    float rl = std::sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
    if (rl < 1e-6f) { r[0] = 1.0f; r[1] = 0.0f; r[2] = 0.0f; rl = 1.0f; }
    for (int i = 0; i < 3; ++i) c.rx[i] = r[i] / rl;
    c.rz[0] = fwd[0]; c.rz[1] = fwd[1]; c.rz[2] = fwd[2];
    c.ry[0] = c.rz[1] * c.rx[2] - c.rz[2] * c.rx[1];
    c.ry[1] = c.rz[2] * c.rx[0] - c.rz[0] * c.rx[2];
    c.ry[2] = c.rz[0] * c.rx[1] - c.rz[1] * c.rx[0];
    c.f  = 0.5f * h / std::tan(0.5f * 50.0f * (float)M_PI / 180.0f);
    c.cx = x + w * 0.5f;
    c.cy = y + h * 0.5f;
    return c;
}

int project(const Cam &c, float px, float py, float pz, float *sx, float *sy, float *depth) {
    float d[3] = { px - c.ex, py - c.ey, pz - c.ez };
    float z = d[0] * c.rz[0] + d[1] * c.rz[1] + d[2] * c.rz[2];
    if (z < 0.5f) return 0;                     // behind the eye, or in it
    float rx = d[0] * c.rx[0] + d[1] * c.rx[1] + d[2] * c.rx[2];
    float ry = d[0] * c.ry[0] + d[1] * c.ry[1] + d[2] * c.ry[2];
    *sx = c.cx + c.f * rx / z;
    *sy = c.cy - c.f * ry / z;
    if (depth) *depth = z;
    return 1;
}

// Fits the orbit to whatever the plan holds, once per plan. A preview that
// opens on an empty patch of sky is a preview nobody trusts.
void frame_plan(dai_show_ui *u) {
    const dai_show_plan *p = dai_show_get_plan(u->sh);
    uint32_t n = p ? dai_show_plan_drone_count(p) : 0;
    if (!n) return;
    float lo[3] = { 1e30f, 1e30f, 1e30f }, hi[3] = { -1e30f, -1e30f, -1e30f };
    for (uint32_t i = 0; i < n; ++i) {
        uint32_t kc = dai_show_plan_keyframe_count(p, i);
        for (uint32_t k = 0; k < kc; ++k) {
            dai_show_key key;
            if (!dai_show_plan_key_at(p, i, k, &key)) continue;
            lo[0] = std::min(lo[0], key.p.x); hi[0] = std::max(hi[0], key.p.x);
            lo[1] = std::min(lo[1], key.p.y); hi[1] = std::max(hi[1], key.p.y);
            lo[2] = std::min(lo[2], key.p.z); hi[2] = std::max(hi[2], key.p.z);
        }
    }
    if (lo[0] > hi[0]) return;
    float ext = std::max(hi[0] - lo[0], std::max(hi[1] - lo[1], hi[2] - lo[2]));
    u->focus_y = 0.5f * (lo[1] + hi[1]);
    u->dist    = std::max(20.0f, ext * 1.8f + 30.0f);
    u->framed_for = (int)n;
}

// The one place a formation is made. Both the mesh button and the built-in
// figures come through here, so a figure from Blender and a figure from this
// file are the same kind of thing to everything downstream.
void add_formation(dai_show_ui *u, const dai_show_sample_desc *desc,
                   const char *name, const char *source) {
    char err[256] = { 0 };
    uint32_t idx = dai_show_formation_from_mesh(u->sh, name, source, desc, err, sizeof(err));
    if (idx == 0xFFFFFFFFu) { say(u, 1, "%s", err[0] ? err : "the figure could not be sampled"); return; }
    u->sel_formation = (int)idx;
    say(u, 0, "formation \"%s\" added: %u points", name, desc->count);
}

void add_builtin(dai_show_ui *u) {
    Soup soup;
    if (u->figure == 0)      figure_sphere(soup, 24, 32);
    else if (u->figure == 1) figure_cube(soup);
    else                     figure_ring(soup, 48, 0.28f);

    dai_show_sample_desc d;
    std::memset(&d, 0, sizeof(d));
    d.positions        = soup.pos.data();
    d.vertex_count     = (uint32_t)(soup.pos.size() / 3);
    d.indices          = soup.idx.data();
    d.index_count      = (uint32_t)soup.idx.size();
    d.base_rgba        = 0xFFFF9040u;
    d.mode             = u->sample_mode;
    d.count            = u->s.drone_count;
    d.min_distance_m   = u->s.min_distance_m;
    d.scale            = u->figure_size * 0.5f;
    d.centre           = dai_vec3{ 0.0f, u->s.takeoff_alt_m + u->figure_size * 0.6f, 0.0f };
    d.view_dir         = dai_vec3{ 0.0f, 0.0f, 1.0f };
    d.relax_iterations = 6;
    d.seed             = u->s.seed + (uint64_t)dai_show_formation_count(u->sh) * 7919ull;

    char name[DAI_SHOW_NAME_MAX];
    std::snprintf(name, sizeof(name), "%s %u", FIGURES[u->figure],
                  dai_show_formation_count(u->sh) + 1u);
    char src[64];
    std::snprintf(src, sizeof(src), "builtin://%s", FIGURES[u->figure]);
    add_formation(u, &d, name, src);
}

void solve_now(dai_show_ui *u) {
    char err[256] = { 0 };
    dai_result r = dai_show_solve(u->sh, err, sizeof(err));
    dai_show_validate_show(u->sh);
    dai_show_timings t = dai_show_get_timings(u->sh);
    uint32_t cn = dai_show_conflict_count(u->sh);
    if (r != DAI_OK)
        say(u, 1, "%s", err[0] ? err : "the show could not be solved");
    else if (t.last_validate.conflicts)
        say(u, 1, "solved in %.0f ms - %u conflicts remain",
            t.sample_ms + t.assign_ms + t.layer_ms + t.validate_ms,
            t.last_validate.conflicts);
    else
        say(u, 0, "solved in %.0f ms - no conflicts (%u shown)",
            t.sample_ms + t.assign_ms + t.layer_ms + t.validate_ms, cn);
    u->framed_for = -1;
    if (u->time > dai_show_plan_duration(dai_show_get_plan(u->sh))) u->time = 0.0f;
}

} // namespace

extern "C" {

dai_show_ui *dai_show_ui_create(dai_show *sh) {
    if (!sh) return nullptr;
    dai_show_ui *u = new dai_show_ui();
    u->sh = sh;
    u->s  = dai_show_get_settings(sh);
    std::snprintf(u->status, sizeof(u->status),
                  "%u drones, %.1f m apart - add a figure to begin",
                  u->s.drone_count, (double)u->s.min_distance_m);
    return u;
}

void      dai_show_ui_destroy(dai_show_ui *u) { delete u; }
dai_show *dai_show_ui_doc(const dai_show_ui *u) { return u ? u->sh : nullptr; }

void dai_show_ui_advance(dai_show_ui *u, float dt) {
    if (!u || !u->playing) return;
    u->time += dt;
    float dur = show_duration(u);
    if (u->time > dur) u->time = 0.0f;          // a show loops in the preview
}
float dai_show_ui_time(const dai_show_ui *u) { return u ? u->time : 0.0f; }
void  dai_show_ui_seek(dai_show_ui *u, float t) {
    if (!u) return;
    u->time = t < 0.0f ? 0.0f : t;
}
int  dai_show_ui_playing(const dai_show_ui *u) { return u ? u->playing : 0; }
void dai_show_ui_play(dai_show_ui *u, int on) { if (u) u->playing = on ? 1 : 0; }

void dai_show_ui_mesh(dai_show_ui *u, const dai_show_sample_desc *desc, const char *source) {
    if (!u) return;
    if (!desc) { u->have_mesh = 0; u->source[0] = 0; return; }
    u->mesh = *desc;
    u->have_mesh = 1;
    std::snprintf(u->source, sizeof(u->source), "%s", source ? source : "");
}

uint32_t dai_show_ui_selected_drone(const dai_show_ui *u) { return u ? u->sel_drone : 0xFFFFFFFFu; }
int      dai_show_ui_selected_conflict(const dai_show_ui *u) { return u ? u->sel_conflict : -1; }

// ---- storyboard ------------------------------------------------------------

void dai_show_ui_storyboard(dai_show_ui *u, dai_ui *ui, float x, float y, float w, float h) {
    if (!u || !ui) return;
    dai_ui_panel_begin(ui, x, y, w, h, nullptr);
    const dai_ui_style *st = dai_ui_style_of(ui);

    dai_ui_section(ui, "Figures");
    dai_ui_seg_buttons(ui, "Shape", &u->figure, FIGURES, 3);
    dai_ui_num_field(ui, "Size (m)", &u->figure_size, 1.0f, 2.0f, 2000.0f, "showfigsize");
    dai_ui_row(ui, 0.0f);
    if (dai_ui_button(ui, "Add figure")) add_builtin(u);
    if (u->have_mesh) {
        if (dai_ui_button(ui, "From selected mesh")) {
            dai_show_sample_desc d = u->mesh;
            d.mode             = u->sample_mode;
            d.count            = u->s.drone_count;
            d.min_distance_m   = u->s.min_distance_m;
            d.relax_iterations = 6;
            d.seed             = u->s.seed + (uint64_t)dai_show_formation_count(u->sh) * 7919ull;
            if (d.scale <= 0.0f) d.scale = u->figure_size * 0.5f;
            const char *base = std::strrchr(u->source, '/');
            const char *leaf = base ? base + 1 : u->source;
            char name[DAI_SHOW_NAME_MAX];
            std::snprintf(name, sizeof(name), "%.*s", (int)sizeof(name) - 1, leaf);
            add_formation(u, &d, name[0] ? name : "mesh", u->source);
        }
    }
    dai_ui_row_end(ui);
    if (!u->have_mesh)
        dai_ui_label(ui, "no mesh selected - pick one in the Project panel");

    dai_ui_section(ui, "Storyboard");
    uint32_t n = dai_show_formation_count(u->sh);
    if (!n) {
        dai_ui_label(ui, "The show is empty. Add a figure above.");
        dai_ui_panel_end(ui);
        return;
    }

    float cx = 0.0f, cyy = 0.0f;
    dai_ui_cursor_pos(ui, &cx, &cyy);
    dai_ui_scroll_begin(ui, "showstory", std::max(60.0f, y + h - cyy - 8.0f));
    for (uint32_t i = 0; i < n; ++i) {
        dai_show_formation_info info;
        if (!dai_show_formation_get(u->sh, i, &info)) continue;

        char head[160];
        std::snprintf(head, sizeof(head), "%u. %s  -  %u pts  -  t %.1fs",
                      i + 1u, info.name, info.point_count, (double)info.t_start);
        int selected = ((int)i == u->sel_formation);
        if (dai_ui_toggle_button(ui, head, selected)) u->sel_formation = (int)i;
        if (!selected) continue;

        if (dai_ui_num_field(ui, "Hold (s)", &info.hold_s, 0.25f, 0.0f, 600.0f, "showhold"))
            dai_show_formation_set_hold(u->sh, i, info.hold_s);

        if (i >= 1) {
            dai_show_transition tr;
            if (dai_show_transition_get(u->sh, i, &tr)) {
                int changed = 0;
                changed |= dai_ui_num_field(ui, "Transit (s)", &tr.duration_s, 0.25f, 0.5f, 600.0f, "showdur");
                changed |= dai_ui_option(ui, "Profile", &tr.profile, PROFILES, 4);
                changed |= dai_ui_option(ui, "Timing", &tr.timing, TIMINGS, 2);
                if (tr.timing == DAI_SHOW_TIMING_STAGGERED)
                    changed |= dai_ui_num_field(ui, "Spread (s)", &tr.stagger_s, 0.1f, 0.0f, 120.0f, "showstag");
                changed |= dai_ui_option(ui, "Assignment", &tr.assign_method, ASSIGN_METHODS, 4);
                if (changed) dai_show_transition_set(u->sh, i, &tr);
            }
        } else {
            dai_ui_label(ui, "the first figure is the take-off grid");
        }

        dai_ui_row(ui, 0.0f);
        if (dai_ui_button_fit(ui, "Up") && dai_show_formation_move(u->sh, i, -1))
            u->sel_formation = (int)i - 1;
        if (dai_ui_button_fit(ui, "Down") && dai_show_formation_move(u->sh, i, +1))
            u->sel_formation = (int)i + 1;
        if (dai_ui_button_fit(ui, "Remove")) {
            dai_show_formation_remove(u->sh, i);
            if (u->sel_formation >= (int)dai_show_formation_count(u->sh))
                u->sel_formation = (int)dai_show_formation_count(u->sh) - 1;
            say(u, 0, "formation removed - solve again");
        }
        dai_ui_row_end(ui);
        dai_ui_separator(ui);
    }
    dai_ui_scroll_end(ui);
    (void)st;
    dai_ui_panel_end(ui);
}

// ---- parameters -------------------------------------------------------------

void dai_show_ui_parameters(dai_show_ui *u, dai_ui *ui, float x, float y, float w, float h) {
    if (!u || !ui) return;
    dai_ui_panel_begin(ui, x, y, w, h, nullptr);
    dai_ui_scroll_begin(ui, "showparams", h - 8.0f);

    dai_ui_section(ui, "Fleet");
    float count = (float)u->s.drone_count;
    int changed = 0;
    changed |= dai_ui_num_field(ui, "Drones", &count, 1.0f, 1.0f, 100000.0f, "showcount");
    u->s.drone_count = (uint32_t)(count + 0.5f);
    changed |= dai_ui_num_field(ui, "Min dist (m)", &u->s.min_distance_m, 0.1f, 0.5f, 100.0f, "showmind");
    changed |= dai_ui_num_field(ui, "v max (m/s)", &u->s.v_max_ms, 0.25f, 0.5f, 60.0f, "showvmax");
    changed |= dai_ui_num_field(ui, "a max (m/s2)", &u->s.a_max_ms2, 0.25f, 0.25f, 40.0f, "showamax");
    float fps = (float)u->s.fps;
    changed |= dai_ui_num_field(ui, "Export fps", &fps, 1.0f, 1.0f, 120.0f, "showfps");
    u->s.fps = (int)(fps + 0.5f);

    dai_ui_section(ui, "Pipeline");
    dai_ui_seg_buttons(ui, "Sampling", &u->sample_mode, SAMPLE_MODES, 3);
    dai_ui_option(ui, "Assignment", &u->assign_method, ASSIGN_METHODS, 4);
    dai_ui_help(ui, "Auto solves exactly up to 2000 drones and switches to the "
                    "clustered auction above - the storyboard shows what ran.");

    dai_ui_section(ui, "Safety volume");
    changed |= dai_ui_num_field(ui, "Fence X (m)", &u->s.fence_half_x, 1.0f, 0.0f, 5000.0f, "showfx");
    changed |= dai_ui_num_field(ui, "Fence Z (m)", &u->s.fence_half_z, 1.0f, 0.0f, 5000.0f, "showfz");
    changed |= dai_ui_num_field(ui, "Ceiling (m)", &u->s.fence_top_m, 1.0f, 0.0f, 3000.0f, "showftop");
    changed |= dai_ui_num_field(ui, "Ground (m)", &u->s.min_ground_m, 0.5f, 0.0f, 200.0f, "showgnd");
    changed |= dai_ui_num_field(ui, "Take-off (m)", &u->s.takeoff_alt_m, 1.0f, 0.0f, 500.0f, "showtoff");

    if (changed) dai_show_set_settings(u->sh, &u->s);

    dai_ui_section(ui, "Solve");
    if (dai_ui_button(ui, "Solve show")) solve_now(u);
    dai_show_timings t = dai_show_get_timings(u->sh);
    if (t.formations) {
        dai_ui_label_fmt(ui, "sample   %8.1f ms", t.sample_ms);
        dai_ui_label_fmt(ui, "assign   %8.1f ms  (%s)", t.assign_ms,
                         ASSIGN_METHODS[(t.last_assign.method_used >= 0 &&
                                         t.last_assign.method_used < 4) ? t.last_assign.method_used : 0]);
        if (t.last_assign.gap_percent >= 0.0f)
            dai_ui_label_fmt(ui, "  %.2f%% over the exact optimum", (double)t.last_assign.gap_percent);
        dai_ui_label_fmt(ui, "layer    %8.1f ms  (%u lifted, %u delayed)", t.layer_ms,
                         t.last_layer.resolved_by_height, t.last_layer.resolved_by_delay);
        dai_ui_label_fmt(ui, "validate %8.1f ms  (%u pairs, %u ticks)", t.validate_ms,
                         t.last_validate.pairs_tested, t.last_validate.ticks_checked);
        const dai_show_plan *p = dai_show_get_plan(u->sh);
        if (p) dai_ui_label_fmt(ui, "plan     %8.2f MB of keyframes",
                                (double)dai_show_plan_bytes(p) / (1024.0 * 1024.0));
    } else {
        dai_ui_label(ui, "not solved yet");
    }

    dai_ui_scroll_end(ui);
    dai_ui_panel_end(ui);
}

// ---- validation --------------------------------------------------------------

void dai_show_ui_validation(dai_show_ui *u, dai_ui *ui, float x, float y, float w, float h) {
    if (!u || !ui) return;
    dai_ui_panel_begin(ui, x, y, w, h, nullptr);
    dai_show_timings t = dai_show_get_timings(u->sh);
    uint32_t shown = dai_show_conflict_count(u->sh);
    uint32_t total = t.last_validate.conflicts;

    dai_ui_row(ui, 0.0f);
    if (dai_ui_button_fit(ui, "Re-check")) {
        dai_show_validate_show(u->sh);
        t = dai_show_get_timings(u->sh);
        say(u, t.last_validate.conflicts != 0,
            "%u conflicts over %u ticks", t.last_validate.conflicts, t.last_validate.ticks_checked);
    }
    dai_ui_row_end(ui);

    float bx = 0.0f, by = 0.0f;
    dai_ui_cursor_pos(ui, &bx, &by);
    const dai_ui_style *st = dai_ui_style_of(ui);
    float th = dai_ui_text_height(ui);
    if (!dai_show_get_plan(u->sh)) {
        dai_ui_label(ui, "no plan yet - solve the show first");
        dai_ui_panel_end(ui);
        return;
    }
    if (total == 0) {
        dai_ui_rect(ui, bx, by + 2.0f, w - 16.0f, th + 8.0f, rgba(0x18, 0x30, 0x1C, 255));
        dai_ui_text(ui, bx + 8.0f, by + 6.0f, "clean - no rule broken anywhere on the timeline", COL_OK);
        dai_ui_advance(ui, w - 16.0f, th + 12.0f);
        dai_ui_label_fmt(ui, "closest approach %.2f m, fastest %.1f m/s",
                         (double)t.last_validate.min_distance_m, (double)t.last_validate.max_speed_ms);
        dai_ui_panel_end(ui);
        return;
    }

    dai_ui_rect(ui, bx, by + 2.0f, w - 16.0f, th + 8.0f, rgba(0x3A, 0x14, 0x12, 255));
    char head[128];
    std::snprintf(head, sizeof(head), "%u conflicts%s - click one to jump to it",
                  total, (shown < total) ? " (first 8192 listed)" : "");
    dai_ui_text(ui, bx + 8.0f, by + 6.0f, head, COL_CONFLICT);
    dai_ui_advance(ui, w - 16.0f, th + 12.0f);

    float cx = 0.0f, cyy = 0.0f;
    dai_ui_cursor_pos(ui, &cx, &cyy);
    dai_ui_scroll_begin(ui, "showconf", std::max(40.0f, y + h - cyy - 8.0f));
    for (uint32_t i = 0; i < shown; ++i) {
        dai_show_conflict c;
        if (!dai_show_conflict_at(u->sh, i, &c)) continue;
        char row[192];
        if (c.a == c.b)
            std::snprintf(row, sizeof(row), "%7.2fs  drone %u %s  %.2f / %.2f",
                          (double)c.time_s, c.a, conflict_word(c.kind),
                          (double)c.value, (double)c.limit);
        else
            std::snprintf(row, sizeof(row), "%7.2fs  drones %u+%u %s  %.2f m (-%.2f)",
                          (double)c.time_s, c.a, c.b, conflict_word(c.kind),
                          (double)c.value, (double)(c.limit - c.value));
        int sel = ((int)i == u->sel_conflict);
        if (dai_ui_toggle_button(ui, row, sel)) {
            // The whole reason this list is clickable: the timeline goes to the
            // moment and the drones involved are the ones drawn red.
            u->sel_conflict = (int)i;
            u->sel_drone    = c.a;
            u->sel_drone_b  = (c.b == c.a) ? 0xFFFFFFFFu : c.b;
            u->time         = c.time_s;
            u->playing      = 0;
        }
    }
    dai_ui_scroll_end(ui);
    (void)st;
    dai_ui_panel_end(ui);
}

// ---- the viewport and its timeline -------------------------------------------

void dai_show_ui_viewport(dai_show_ui *u, dai_ui *ui, float x, float y, float w, float h) {
    if (!u || !ui || w < 40.0f || h < 60.0f) return;
    const float TL = 40.0f;                       // the timeline strip below
    float vh = h - TL;

    refresh_fleet(u);
    if (u->framed_for != (int)u->fleet.size()) frame_plan(u);

    dai_ui_rect(ui, x, y, w, vh, COL_BG);
    dai_ui_clip_begin(ui, x, y, w, vh);
    Cam cam = camera_of(u, x, y, w, vh);

    // A ground grid, so height reads as height. 20 m squares out to the fence,
    // which is also the cheapest way to show where the fence is.
    float half = std::max(40.0f, std::max(u->s.fence_half_x, u->s.fence_half_z));
    float step = (half > 400.0f) ? 100.0f : (half > 150.0f ? 50.0f : 20.0f);
    for (float g = -half; g <= half + 0.01f; g += step) {
        float ax, ay, bx2, by2;
        if (project(cam, g, 0.0f, -half, &ax, &ay, nullptr) &&
            project(cam, g, 0.0f,  half, &bx2, &by2, nullptr))
            dai_ui_line(ui, ax, ay, bx2, by2, 1.0f, COL_GRID);
        if (project(cam, -half, 0.0f, g, &ax, &ay, nullptr) &&
            project(cam,  half, 0.0f, g, &bx2, &by2, nullptr))
            dai_ui_line(ui, ax, ay, bx2, by2, 1.0f, COL_GRID);
    }
    // The fence, drawn as the box it is.
    if (u->s.fence_half_x > 0.0f && u->s.fence_top_m > 0.0f) {
        const float fx = u->s.fence_half_x, fz = u->s.fence_half_z, ft = u->s.fence_top_m;
        const float corner[4][2] = { { -fx, -fz }, { fx, -fz }, { fx, fz }, { -fx, fz } };
        for (int i = 0; i < 4; ++i) {
            int j = (i + 1) & 3;
            float ax, ay, bx2, by2;
            if (project(cam, corner[i][0], ft, corner[i][1], &ax, &ay, nullptr) &&
                project(cam, corner[j][0], ft, corner[j][1], &bx2, &by2, nullptr))
                dai_ui_line(ui, ax, ay, bx2, by2, 1.0f, COL_HORIZON);
            if (project(cam, corner[i][0], 0.0f, corner[i][1], &ax, &ay, nullptr) &&
                project(cam, corner[i][0], ft,   corner[i][1], &bx2, &by2, nullptr))
                dai_ui_line(ui, ax, ay, bx2, by2, 1.0f, COL_HORIZON);
        }
    }

    // The fleet. Drawn in its LED colour, sized by distance so the figure has
    // depth, and never more than a few thousand quads: past that the points
    // are smaller than a pixel anyway and the honest thing is to say the view
    // is decimated rather than to spend a frame drawing a grey blob.
    uint32_t n = (uint32_t)u->fleet.size();
    uint32_t stride = 1;
    while (n / stride > 6000u) ++stride;
    for (uint32_t i = 0; i < n; i += stride) {
        const dai_show_point &p = u->fleet[i];
        float sx, sy, depth;
        if (!project(cam, p.x, p.y, p.z, &sx, &sy, &depth)) continue;
        float size = std::max(2.0f, std::min(9.0f, 260.0f / depth));
        int bad = u->flagged[i];
        uint32_t col = bad ? COL_CONFLICT
                           : rgba(std::max<uint32_t>(p.r, 24), std::max<uint32_t>(p.g, 24),
                                  std::max<uint32_t>(p.b, 24), 255);
        if (bad) size = std::max(size, 5.0f);
        dai_ui_rect(ui, sx - size * 0.5f, sy - size * 0.5f, size, size, col);
        if (i == u->sel_drone || i == u->sel_drone_b)
            dai_ui_rect_outline(ui, sx - size - 2.0f, sy - size - 2.0f,
                                size * 2.0f + 4.0f, size * 2.0f + 4.0f, 1.0f, COL_WARN);
    }

    // And the pairs that are too close, joined by the line an operator is
    // going to point at in the debrief.
    uint32_t cn = dai_show_conflict_count(u->sh);
    for (uint32_t i = 0; i < cn; ++i) {
        dai_show_conflict c;
        if (!dai_show_conflict_at(u->sh, i, &c)) continue;
        if (std::fabs(c.time_s - u->time) > 0.35f) continue;
        if (c.a == c.b || c.a >= n || c.b >= n) continue;
        float ax, ay, bx2, by2;
        if (project(cam, u->fleet[c.a].x, u->fleet[c.a].y, u->fleet[c.a].z, &ax, &ay, nullptr) &&
            project(cam, u->fleet[c.b].x, u->fleet[c.b].y, u->fleet[c.b].z, &bx2, &by2, nullptr))
            dai_ui_line(ui, ax, ay, bx2, by2, 1.5f, COL_CONFLICT);
    }
    dai_ui_clip_end(ui);

    // Orbit and zoom. The gesture is the scene view's, because a second
    // convention for turning a camera is a second thing to learn for nothing.
    float mx = 0.0f, my = 0.0f;
    int down = 0, pressed = 0;
    dai_ui_mouse(ui, &mx, &my, &down, &pressed);
    int inside = (mx >= x && mx < x + w && my >= y && my < y + vh);
    if (pressed && inside) { u->dragging = 1; u->drag_x = mx; u->drag_y = my; }
    if (!down) u->dragging = 0;
    if (u->dragging) {
        u->yaw   += (mx - u->drag_x) * 0.008f;
        u->pitch += (my - u->drag_y) * 0.006f;
        u->pitch  = std::max(-1.4f, std::min(1.4f, u->pitch));
        u->drag_x = mx; u->drag_y = my;
    }
    if (inside) {
        float wheel = dai_ui_wheel(ui);
        if (wheel != 0.0f) u->dist = std::max(10.0f, u->dist * (1.0f - 0.1f * wheel));
    }

    // ---- the timeline ------------------------------------------------------
    const dai_ui_style *st = dai_ui_style_of(ui);
    float ty = y + vh;
    dai_ui_rect(ui, x, ty, w, TL, st->chrome);
    dai_ui_rect(ui, x, ty, w, 1.0f, st->panel_border);

    float bw = 54.0f, bh = 22.0f;
    float bx = x + 6.0f, by = ty + (TL - bh) * 0.5f;
    int over_btn = (mx >= bx && mx < bx + bw && my >= by && my < by + bh);
    dai_ui_rect(ui, bx, by, bw, bh, over_btn ? st->button_hover : st->button);
    const char *label = u->playing ? "Pause" : "Play";
    dai_ui_text(ui, bx + (bw - dai_ui_text_width(ui, label)) * 0.5f,
                by + (bh - dai_ui_text_height(ui)) * 0.5f, label, st->text);
    if (over_btn && pressed) u->playing = !u->playing;

    char tstr[48];
    float dur = show_duration(u);
    std::snprintf(tstr, sizeof(tstr), "%6.2f / %.2f s", (double)u->time, (double)dur);
    float tw = dai_ui_text_width(ui, tstr);
    dai_ui_text(ui, x + w - tw - 8.0f, by + (bh - dai_ui_text_height(ui)) * 0.5f, tstr, st->text_dim);

    float sx0 = bx + bw + 10.0f;
    float sx1 = x + w - tw - 16.0f;
    float sy0 = ty + TL * 0.5f - 5.0f;
    if (sx1 > sx0 + 20.0f) {
        dai_ui_rect(ui, sx0, sy0, sx1 - sx0, 10.0f, st->track);
        // Where the figures are. A storyboard with no marks on the timeline is
        // a storyboard you have to count seconds against.
        uint32_t fn = dai_show_formation_count(u->sh);
        for (uint32_t i = 0; i < fn; ++i) {
            dai_show_formation_info info;
            if (!dai_show_formation_get(u->sh, i, &info)) continue;
            float fx = sx0 + (sx1 - sx0) * std::min(1.0f, info.t_start / dur);
            dai_ui_rect(ui, fx, sy0 - 4.0f, 1.0f, 18.0f, st->accent);
        }
        // And where it goes wrong.
        for (uint32_t i = 0; i < cn; ++i) {
            dai_show_conflict c;
            if (!dai_show_conflict_at(u->sh, i, &c)) continue;
            float fx = sx0 + (sx1 - sx0) * std::min(1.0f, c.time_s / dur);
            dai_ui_rect(ui, fx, sy0, 2.0f, 10.0f, COL_CONFLICT);
        }
        float px = sx0 + (sx1 - sx0) * std::min(1.0f, u->time / dur);
        dai_ui_rect(ui, px - 1.0f, sy0 - 6.0f, 3.0f, 22.0f, st->text);
        int over_bar = (mx >= sx0 && mx <= sx1 && my >= ty && my < ty + TL);
        if (over_bar && down) {
            u->time    = dur * (mx - sx0) / (sx1 - sx0);
            u->playing = 0;
        }
    }
}

// ---- the status line ----------------------------------------------------------

void dai_show_ui_status(dai_show_ui *u, dai_ui *ui, float x, float y, float w, float h) {
    if (!u || !ui) return;
    const dai_ui_style *st = dai_ui_style_of(ui);
    dai_show_timings t = dai_show_get_timings(u->sh);
    float ty = y + (h - dai_ui_text_height(ui)) * 0.5f;

    char left[256];
    std::snprintf(left, sizeof(left),
                  "%u drones   %u figures   closest %.2f m   fastest %.1f m/s   solve %.0f ms",
                  u->s.drone_count, dai_show_formation_count(u->sh),
                  (double)t.last_validate.min_distance_m, (double)t.last_validate.max_speed_ms,
                  t.sample_ms + t.assign_ms + t.layer_ms + t.validate_ms);
    dai_ui_text(ui, x + 8.0f, ty, left, st->text_dim);

    if (u->status[0]) {
        float sw = dai_ui_text_width(ui, u->status);
        dai_ui_text(ui, x + w - sw - 10.0f, ty, u->status,
                    u->status_bad ? COL_CONFLICT : COL_OK);
    }
}

// ---- all of it, in a dock -------------------------------------------------------

void dai_show_ui_panels(dai_show_ui *u, dai_ui *ui, struct dai_dock *dock) {
    if (!u || !ui || !dock) return;
    // Idempotent, like every other registration in this editor: the first call
    // places the panel, the rest are free, and wherever the user dragged it
    // afterwards is where it stays.
    dai_dock_add(dock, "Storyboard", DAI_DOCK_LEFT, 0.22f);
    dai_dock_add(dock, "Show Parameters", DAI_DOCK_RIGHT, 0.22f);
    dai_dock_add(dock, "Validation", DAI_DOCK_BOTTOM, 0.26f);

    float px, py, pw, ph;
    if (dai_dock_panel(dock, "Storyboard", &px, &py, &pw, &ph)) {
        dai_show_ui_storyboard(u, ui, px, py, pw, ph);
        dai_dock_panel_end(dock);
    }
    if (dai_dock_panel(dock, "Show Parameters", &px, &py, &pw, &ph)) {
        dai_show_ui_parameters(u, ui, px, py, pw, ph);
        dai_dock_panel_end(dock);
    }
    if (dai_dock_panel(dock, "Validation", &px, &py, &pw, &ph)) {
        dai_show_ui_validation(u, ui, px, py, pw, ph);
        dai_dock_panel_end(dock);
    }
}

} // extern "C"
