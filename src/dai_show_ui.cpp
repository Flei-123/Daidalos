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

// ---- fitting text to the column it has --------------------------------------
//
// These panels are read at 0.22 of the frame when they are docked and at full
// width when they are photographed, and the same row has to be legible in
// both. Two rules follow, and both are measured rather than guessed:
//
//   * a field label column is as wide as the widest label in its section, so
//     "a max (m/s2)" keeps its unit and its bracket instead of ending in the
//     value box, and
//   * a list row that does not fit is cut at the END with an ellipsis, never
//     at the front - the running number is the thing the eye looks for first
//     and it is the first thing a left-side clip would take away.

// The row pitch the layout actually uses, from the style rather than from a
// constant that would rot the moment the UI scale changes.
float row_pitch(dai_ui *ui) {
    const dai_ui_style *st = dai_ui_style_of(ui);
    return dai_ui_text_height(ui) + st->row_pad + st->spacing;
}

// The label column, measured. Capped at 55% of the panel so a long label
// cannot squeeze the value box it belongs to out of the panel entirely; the
// floor is the style default, because a column narrower than that reads as a
// ragged left edge. The caller restores what it found - the inspector's own
// fit_label_column keeps its value, and the show panels are guests in the
// same dai_ui.
float fit_labels(dai_ui *ui, const char *const *labels, int count) {
    dai_ui_style *st = dai_ui_style_of(ui);
    float was = st->label_w;
    float w = 62.0f;
    for (int i = 0; i < count; ++i)
        w = std::max(w, dai_ui_text_width(ui, labels[i]) + 10.0f);
    float cap = dai_ui_panel_width(ui) * 0.55f;
    if (cap > 40.0f && w > cap) w = cap;
    st->label_w = w;
    return was;
}

// A choice that shows all of its options at once - while they still fit.
//
// A strip is the better control for three short words: every option is visible
// and picking one costs a single click. But the strip divides the row it is
// given, not the words it holds, and in a docked panel of 180 px that leaves
// forty pixels for "Silhouette". dai_ui_seg_buttons shortens what it is handed
// so nothing escapes its button, and that is the right last line of defence -
// but "Sil..." next to "Sur..." is a control that has stopped saying what it
// chooses. So the width is worked out here BEFORE the strip is asked for, the
// same way the strip works it out, and when the longest option no longer fits
// its cell the row becomes an ordinary dropdown, which always has room for one
// whole word.
//
// The scroll bar's lane is taken off the top: the parameter panel scrolls, and
// a segment that ends under the bar has lost its last letter to it.
int seg_row(dai_ui *ui, const char *label, int *value, const char *const *items, int count) {
    const dai_ui_style *st = dai_ui_style_of(ui);
    float full = dai_ui_panel_width(ui) - st->padding * 2.0f;
    float lw   = (label && *label) ? (st->label_w > 0.0f ? st->label_w : 62.0f) : 0.0f;
    if (lw > full * 0.55f) lw = full * 0.55f;
    float cell = (full - lw - 14.0f) / (float)count;
    float need = 0.0f;
    for (int i = 0; i < count; ++i)
        need = std::max(need, dai_ui_text_width(ui, items[i] ? items[i] : "") + 8.0f);
    if (cell >= need) return dai_ui_seg_buttons(ui, label, value, items, count);
    return dai_ui_option(ui, label, value, items, count);
}

// Cuts `s` down to `avail` pixels, at the end, with an ellipsis. Written with
// dots rather than U+2026 because the UI font is the editor's ASCII atlas and
// a missing glyph would be a hole exactly where the text was too long.
void ellide(dai_ui *ui, char *s, float avail) {
    if (avail <= 0.0f || dai_ui_text_width(ui, s) <= avail) return;
    size_t n = std::strlen(s);
    while (n > 0) {
        char keep[256];
        size_t k = std::min(n, sizeof(keep) - 4);
        std::memcpy(keep, s, k);
        std::strcpy(keep + k, "...");
        if (dai_ui_text_width(ui, keep) <= avail || n == 1) {
            std::snprintf(s, k + 4, "%s", keep);
            return;
        }
        --n;
    }
}

// A hint that is longer than the panel is wide, broken at spaces onto as many
// label rows as it needs. The alternative - letting it run under the edge - is
// how "no mesh selected - pick one in the Project panel" became "no mesh sele".
// `draw == 0` lays the text out and counts the rows without putting a single
// vertex in the list, which is what the report block needs to know BEFORE it
// commits to a row: how tall this line is going to be.
int wrapped_rows(dai_ui *ui, const char *text, int draw) {
    const dai_ui_style *st = dai_ui_style_of(ui);
    float avail = dai_ui_panel_width(ui) - st->padding * 2.0f - 2.0f;
    if (avail < 40.0f || dai_ui_text_width(ui, text) <= avail) {
        if (draw) dai_ui_label(ui, text);
        return 1;
    }
    int rows = 0;
    std::string line;
    const char *p = text;
    while (*p) {
        const char *sp = std::strchr(p, ' ');
        std::string word(p, sp ? (size_t)(sp - p) : std::strlen(p));
        std::string trial = line.empty() ? word : line + " " + word;
        if (!line.empty() && dai_ui_text_width(ui, trial.c_str()) > avail) {
            if (draw) dai_ui_label(ui, line.c_str());
            ++rows;
            line = word;
        } else {
            line = trial;
        }
        if (!sp) break;
        p = sp + 1;
    }
    if (!line.empty()) {
        if (draw) dai_ui_label(ui, line.c_str());
        ++rows;
    }
    return rows;
}

void wrapped_label(dai_ui *ui, const char *text) {
    wrapped_rows(ui, text, 1);
}

// How tall a row with a control in it is, and whether one still fits above the
// bottom edge of the region it is going into. Both come from the style rather
// than from a constant, so a change of UI scale moves the cut with the text.
float widget_row(dai_ui *ui) {
    return dai_ui_text_height(ui) + dai_ui_style_of(ui)->row_pad;
}

bool fits(dai_ui *ui, float bottom, float row_h) {
    if (!(bottom > 0.0f)) return true;
    float x = 0.0f, y = 0.0f;
    dai_ui_cursor_pos(ui, &x, &y);
    return y + row_h <= bottom + 0.5f;
}

// A button drawn only when the whole of it fits. When it does not, the row is
// still spent: a control that moves up into the place another one was about to
// occupy is a click that does something the reader did not ask for.
int row_button(dai_ui *ui, float bottom, const char *label) {
    float hgt = widget_row(ui);
    if (!fits(ui, bottom, hgt)) { dai_ui_advance(ui, dai_ui_panel_width(ui), hgt); return 0; }
    return dai_ui_button(ui, label);
}

// A read-only "label   value" row, on the same label column every field in the
// panel uses. Not a text field with editing switched off: a box that looks
// editable and is not is the more expensive lie, and half of what an inspector
// shows about a solved show - where a drone is, what colour it burns - is a
// result rather than a setting.
// `bottom` is the lower edge of the region the row lives in, and a row that
// would be cut by it is not drawn at all - the same all-or-nothing rule the
// report block and the conflict list follow. It still advances the layout, so
// the wheel brings it in whole.
void field_row(dai_ui *ui, const char *label, const char *value, float bottom) {
    const dai_ui_style *st = dai_ui_style_of(ui);
    float x = 0.0f, y = 0.0f;
    dai_ui_cursor_pos(ui, &x, &y);
    const float lh   = dai_ui_text_height(ui);
    const float full = dai_ui_panel_width(ui) - st->padding * 2.0f;
    // The value column starts past the WIDEST of the two claims on it: the
    // shared label column, and this row's own label. A column that is narrower
    // than the word in front of it is how "Keyframes" and "8" ended up printed
    // as one word.
    float lw = std::max(st->label_w > 0.0f ? st->label_w : 62.0f,
                        dai_ui_text_width(ui, label) + 8.0f);
    char cut[192];
    std::snprintf(cut, sizeof(cut), "%s", value ? value : "");

    // Docked at a fifth of the frame there is no room for both on one line.
    // Then the value goes UNDER its label, indented, rather than being cut to
    // "25.3 ..." - a number with its end taken off is not a shorter number.
    const bool one_row = (dai_ui_text_width(ui, cut) <= full - lw - 4.0f);
    const float need = one_row ? lh : lh * 2.0f + st->spacing;
    if (bottom > 0.0f && y + need > bottom) { dai_ui_advance(ui, full, need); return; }

    dai_ui_text(ui, x, y, label, st->text_dim);
    if (one_row) {
        dai_ui_text(ui, x + lw, y, cut, st->text);
        dai_ui_advance(ui, full, lh);
        return;
    }
    dai_ui_advance(ui, full, lh);
    ellide(ui, cut, full - 10.0f);
    dai_ui_cursor_pos(ui, &x, &y);
    dai_ui_text(ui, x + 8.0f, y, cut, st->text);
    dai_ui_advance(ui, full, lh);
}

// What a full width row has for its own text: the widget takes three paddings
// of chrome, and a scroll bar may be sitting on the right.
float row_text_width(dai_ui *ui) {
    const dai_ui_style *st = dai_ui_style_of(ui);
    return dai_ui_panel_width(ui) - st->padding * 5.0f - 12.0f;
}

// A measured statistics row: formatted, then broken at spaces rather than run
// under the panel edge. These lines are read at a fifth of the frame's width
// when the panel is docked, and a number that ends mid digit is worse than no
// number at all.
//
// `bottom` is the lower edge of the scroll region the block lives in. A row
// that starts above it and ends below it used to be drawn anyway and the clip
// cut it through the middle of the glyphs - a line of text sliced lengthwise,
// at the exact place a reader looks for the last number. Such a row is skipped
// here, whole, but it still ADVANCES the layout: the scroll region measures its
// content from the cursor, and a row that takes no space is a row the wheel can
// never bring into view.
void stat_line(dai_ui *ui, float bottom, const char *fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    float cx = 0.0f, cy = 0.0f;
    dai_ui_cursor_pos(ui, &cx, &cy);
    int rows = wrapped_rows(ui, buf, 0);
    float need = row_pitch(ui) * (float)rows;
    if (bottom > 0.0f && cy + need > bottom) {
        dai_ui_advance(ui, 0.0f, need - dai_ui_style_of(ui)->spacing);
        return;
    }
    wrapped_rows(ui, buf, 1);
}

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

// A circle, out of the line segments the 2D canvas has. Sixteen of them is
// round to the eye at any size this marker is drawn at, and a shape the canvas
// already knows beats a new primitive in dai_ui for the sake of one panel.
void ring(dai_ui *ui, float cx, float cy, float r, float thick, uint32_t col) {
    const int SEG = 16;
    float px = cx + r, py = cy;
    for (int i = 1; i <= SEG; ++i) {
        float a = 2.0f * (float)M_PI * (float)i / (float)SEG;
        float qx = cx + r * std::cos(a), qy = cy + r * std::sin(a);
        dai_ui_line(ui, px, py, qx, qy, thick, col);
        px = qx; py = qy;
    }
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
    // No opening line is written here on purpose: dai_show_ui_status derives
    // what it says from the document, and a string cached at create time is a
    // string that is wrong from the first click onwards.
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
    const char *const LABELS[] = { "Shape", "Size (m)", "Hold (s)", "Transit (s)",
                                   "Profile", "Timing", "Spread (s)", "Assignment" };
    float label_was = fit_labels(ui, LABELS, 8);

    dai_ui_section(ui, "Figures");
    seg_row(ui, "Shape", &u->figure, FIGURES, 3);
    dai_ui_num_field(ui, "Size (m)", &u->figure_size, 1.0f, 2.0f, 2000.0f, "showfigsize");
    dai_ui_row(ui, 0.0f);
    if (dai_ui_button(ui, "Add figure")) add_builtin(u);
    if (u->have_mesh) {
        // Two buttons in one row, in a column that is a fifth of the frame:
        // the long caption is the first thing the panel edge cuts in half, so
        // below a certain width the button says the short version of the same
        // sentence instead of "From selecte".
        const char *from_label = dai_ui_panel_width(ui) < 260.0f ? "From mesh"
                                                                 : "From selected mesh";
        if (dai_ui_button(ui, from_label)) {
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
        wrapped_label(ui, "no mesh selected - pick one in the Project panel");

    dai_ui_section(ui, "Storyboard");
    uint32_t n = dai_show_formation_count(u->sh);
    if (!n) {
        wrapped_label(ui, "The show is empty. Add a figure above.");
        dai_ui_style_of(ui)->label_w = label_was;
        dai_ui_panel_end(ui);
        return;
    }

    float cx = 0.0f, cyy = 0.0f;
    dai_ui_cursor_pos(ui, &cx, &cyy);
    dai_ui_scroll_begin(ui, "showstory", std::max(60.0f, y + h - cyy - 8.0f));
    for (uint32_t i = 0; i < n; ++i) {
        dai_show_formation_info info;
        if (!dai_show_formation_get(u->sh, i, &info)) continue;

        char head[192];
        std::snprintf(head, sizeof(head), "%u. %s - %u pts - t %.1fs",
                      i + 1u, info.name, info.point_count, (double)info.t_start);
        // Docked, this panel is a fifth of the frame wide. The row keeps its
        // number and loses its tail - the half a reader can rebuild - instead
        // of being centred and clipped at both ends by the button it sits in.
        ellide(ui, head, row_text_width(ui));
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
            wrapped_label(ui, "the first figure is the take-off grid");
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
    dai_ui_style_of(ui)->label_w = label_was;
    dai_ui_panel_end(ui);
}

// ---- parameters -------------------------------------------------------------

void dai_show_ui_parameters(dai_show_ui *u, dai_ui *ui, float x, float y, float w, float h) {
    if (!u || !ui) return;
    dai_ui_panel_begin(ui, x, y, w, h, nullptr);
    // Where the scrolled area ends, in the same coordinates the layout cursor
    // uses. Every stat_line below is measured against it, so the block stops at
    // a whole row instead of being cut through the middle of one.
    float view_x = 0.0f, view_y = 0.0f;
    dai_ui_cursor_pos(ui, &view_x, &view_y);
    const float view_h = h - 8.0f;
    const float report_bottom = view_y + view_h;
    dai_ui_scroll_begin(ui, "showparams", view_h);
    // One column for the whole panel rather than one per section: a label
    // column that changes width halfway down reads as two panels stacked.
    const char *const LABELS[] = { "Drones", "Min dist (m)", "v max (m/s)", "a max (m/s2)",
                                   "Export fps", "Sampling", "Assignment", "Fence X (m)",
                                   "Fence Z (m)", "Ceiling (m)", "Ground (m)", "Take-off (m)" };
    float label_was = fit_labels(ui, LABELS, 12);

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
    seg_row(ui, "Sampling", &u->sample_mode, SAMPLE_MODES, 3);
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
        // Everything under this heading is the WHOLE SHOW: counts summed over
        // every transition, the gap and the detour at their worst case, the
        // least exact method that ran anywhere. The alternative - printing
        // whichever move happened to be solved last - is how "0 lifted, 0
        // delayed" came to stand next to eleven conflicts, which reads as a
        // contradiction and is in fact two different transitions.
        dai_ui_label(ui, "show total");
        stat_line(ui, report_bottom, "sample   %8.1f ms", t.sample_ms);
        stat_line(ui, report_bottom, "assign   %8.1f ms  (%s)", t.assign_ms,
                  ASSIGN_METHODS[(t.last_assign.method_used >= 0 &&
                                  t.last_assign.method_used < 4) ? t.last_assign.method_used : 0]);
        if (t.last_assign.gap_percent >= 0.0f)
            stat_line(ui, report_bottom, "  worst move %.2f%% over the exact optimum",
                      (double)t.last_assign.gap_percent);
        stat_line(ui, report_bottom, "layer    %8.1f ms", t.layer_ms);
        // The fourth number belongs next to the other three: a formation fault
        // is not a crossing the separator lost, but it is a reason the show
        // does not fly, and a panel that reports "0 left over" and nothing else
        // reads as a sign-off. The Validation panel names the figure it stands
        // in; here it is counted - on a line of its own, because this panel is
        // a fifth of the window wide and a number that wraps is a number that
        // gets read as part of the line above it.
        stat_line(ui, report_bottom, "  %u crossings: %u lifted, %u delayed, %u left over",
                  t.last_layer.crossings_found, t.last_layer.resolved_by_height,
                  t.last_layer.resolved_by_delay, t.last_layer.unresolved);
        stat_line(ui, report_bottom, "  %u formation fault%s", t.last_layer.endpoint_pairs,
                  (t.last_layer.endpoint_pairs == 1) ? "" : "s");
        stat_line(ui, report_bottom, "validate %8.1f ms", t.validate_ms);
        stat_line(ui, report_bottom, "  %u pairs over %u ticks",
                  t.last_validate.pairs_tested, t.last_validate.ticks_checked);
        const dai_show_plan *p = dai_show_get_plan(u->sh);
        if (p) stat_line(ui, report_bottom, "plan     %8.2f MB of keyframes",
                         (double)dai_show_plan_bytes(p) / (1024.0 * 1024.0));

        // And the move the storyboard has selected, on its own - a sum says
        // that something is wrong, a per transition row says WHERE, and the fix
        // (a longer duration, another profile, another assignment) is always
        // made on one transition.
        dai_show_assign_stats sa;
        dai_show_layer_stats  sl;
        dai_show_formation_info info;
        if (u->sel_formation >= 1 &&
            dai_show_transition_stats(u->sh, (uint32_t)u->sel_formation, &sa, &sl) &&
            dai_show_formation_get(u->sh, (uint32_t)u->sel_formation, &info)) {
            dai_ui_separator(ui);
            char head[160];
            std::snprintf(head, sizeof(head), "the move into %d. %s",
                          u->sel_formation + 1, info.name);
            ellide(ui, head, row_text_width(ui));
            dai_ui_label(ui, head);
            stat_line(ui, report_bottom, "assign   %8.1f ms  (%s)", sa.solve_ms,
                      ASSIGN_METHODS[(sa.method_used >= 0 && sa.method_used < 4) ? sa.method_used : 0]);
            if (sa.gap_percent >= 0.0f)
                stat_line(ui, report_bottom, "  %.2f%% over the exact optimum", (double)sa.gap_percent);
            stat_line(ui, report_bottom, "  %.0f m flown in total", sa.total_cost_m);
            stat_line(ui, report_bottom, "layer    %8.1f ms", sl.solve_ms);
            stat_line(ui, report_bottom, "  %u crossings: %u lifted, %u delayed, %u left over",
                      sl.crossings_found, sl.resolved_by_height,
                      sl.resolved_by_delay, sl.unresolved);
            stat_line(ui, report_bottom, "  %u formation fault%s", sl.endpoint_pairs,
                      (sl.endpoint_pairs == 1) ? "" : "s");
            if (sl.endpoint_pairs)
                wrapped_label(ui, "  the fault stands IN this figure - two points are "
                                  "closer than the minimum distance, no transition can fix that");
            stat_line(ui, report_bottom, "  %u layers, up to %.1f m of detour",
                      sl.layers_used, (double)sl.max_extra_height_m);
        }
    } else {
        wrapped_label(ui, "not solved yet");
    }

    dai_ui_scroll_end(ui);
    dai_ui_style_of(ui)->label_w = label_was;
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

    // The list is clipped to WHOLE rows. A conflict list that ends in a row
    // cut across the middle is the one place in this editor where a reader
    // cannot tell whether he has seen everything - and here that question is
    // the difference between a show that flies and one that does not. What
    // does not fit is reached with the scroll bar, so the rows below still
    // advance the layout even when they are not drawn.
    float cx = 0.0f, cyy = 0.0f;
    dai_ui_cursor_pos(ui, &cx, &cyy);
    const float pitch  = row_pitch(ui);
    const float space  = std::max(pitch, y + h - cyy - 8.0f);
    const int   visible = (int)std::floor(space / pitch);
    const float list_h  = (float)visible * pitch;
    const float list_top = cyy, list_bottom = cyy + list_h;
    dai_ui_scroll_begin(ui, "showconf", list_h);
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
        // Which figure the moment belongs to, on the row itself. A time stamp
        // says WHEN, and a director fixes a show by opening the figure it
        // happens in - so the row carries the place too, and clicking it
        // selects that figure in the Storyboard as well as the drones.
        uint32_t where = dai_show_formation_at_time(u->sh, c.time_s);
        dai_show_formation_info wi;
        if (where != 0xFFFFFFFFu && dai_show_formation_get(u->sh, where, &wi)) {
            size_t used = std::strlen(row);
            std::snprintf(row + used, sizeof(row) - used, "   in %u. %s",
                          (unsigned)(where + 1u), wi.name);
        }
        ellide(ui, row, row_text_width(ui));
        float rx = 0.0f, ry = 0.0f;
        dai_ui_cursor_pos(ui, &rx, &ry);
        if (ry < list_top - 0.5f || ry + pitch > list_bottom + 0.5f) {
            dai_ui_advance(ui, w - 16.0f, pitch - dai_ui_style_of(ui)->spacing);
            continue;
        }
        int sel = ((int)i == u->sel_conflict);
        if (dai_ui_toggle_button(ui, row, sel)) {
            // The whole reason this list is clickable: the timeline goes to the
            // moment and the drones involved are the ones drawn red.
            u->sel_conflict = (int)i;
            u->sel_drone    = c.a;
            u->sel_drone_b  = (c.b == c.a) ? 0xFFFFFFFFu : c.b;
            u->time         = c.time_s;
            u->playing      = 0;
            // ...and the figure it happens in, so the Show Parameters panel
            // shows the move that produced it and the Storyboard highlights
            // the row the fix belongs to.
            if (where != 0xFFFFFFFFu) u->sel_formation = (int)where;
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
        if (bad) size = std::min(size, 4.0f);   // the ring below is the marker
        dai_ui_rect(ui, sx - size * 0.5f, sy - size * 0.5f, size, size, col);
        if (i == u->sel_drone || i == u->sel_drone_b)
            dai_ui_rect_outline(ui, sx - size - 2.0f, sy - size - 2.0f,
                                size * 2.0f + 4.0f, size * 2.0f + 4.0f, 1.0f, COL_WARN);
    }

    // And what is wrong, marked so it survives being photographed: a red ring
    // around each drone involved, the line between the pair, and the number.
    //
    // The ring is drawn at twelve pixels across whatever the perspective says,
    // because the point of a marker is to be found in a sky of ten thousand
    // dots - a red dot among coloured dots is not found, it is looked for. The
    // label is the pair and how close they came, which is the sentence the
    // debrief argues about, and it is placed once per marker: labels that pile
    // up on each other say less than the one that would have been legible, so
    // a label whose box overlaps one already written is left off and the ring
    // - which cannot lie about position - carries the news alone.
    const float RING_R = 7.0f;                  // 14 px across, empty in the middle
    uint32_t cn = dai_show_conflict_count(u->sh);
    struct LabelBox { float x0, y0, x1, y1; };
    std::vector<LabelBox> taken;
    for (uint32_t i = 0; i < cn; ++i) {
        dai_show_conflict c;
        if (!dai_show_conflict_at(u->sh, i, &c)) continue;
        if (std::fabs(c.time_s - u->time) > 0.35f) continue;
        if (c.a >= n) continue;
        const int pair = (c.b != c.a && c.b < n);
        float ax, ay, bx2 = 0.0f, by2 = 0.0f;
        if (!project(cam, u->fleet[c.a].x, u->fleet[c.a].y, u->fleet[c.a].z, &ax, &ay, nullptr))
            continue;
        if (pair && !project(cam, u->fleet[c.b].x, u->fleet[c.b].y, u->fleet[c.b].z,
                             &bx2, &by2, nullptr))
            continue;
        // Two drones a metre apart, seen from two hundred, are four pixels
        // apart: two rings there are one red blob and the line between them is
        // invisible. So a pair that close gets ONE ring around both, which is
        // the honest picture - what is wrong is the pair, not either drone.
        float gapx = pair ? (bx2 - ax) : 0.0f, gapy = pair ? (by2 - ay) : 0.0f;
        float gap  = std::sqrt(gapx * gapx + gapy * gapy);
        if (pair && gap > 2.2f * RING_R) {
            ring(ui, ax, ay, RING_R, 2.0f, COL_CONFLICT);
            ring(ui, bx2, by2, RING_R, 2.0f, COL_CONFLICT);
            dai_ui_line(ui, ax, ay, bx2, by2, 1.5f, COL_CONFLICT);
        } else if (pair) {
            ring(ui, 0.5f * (ax + bx2), 0.5f * (ay + by2), RING_R + gap * 0.5f,
                 2.0f, COL_CONFLICT);
        } else {
            ring(ui, ax, ay, RING_R, 2.0f, COL_CONFLICT);
        }

        char tag[64];
        if (pair)
            std::snprintf(tag, sizeof(tag), "%u+%u  %.2f m", c.a, c.b, (double)c.value);
        else if (c.kind == DAI_SHOW_CONFLICT_VMAX)
            std::snprintf(tag, sizeof(tag), "%u  %.1f m/s", c.a, (double)c.value);
        else if (c.kind == DAI_SHOW_CONFLICT_AMAX)
            std::snprintf(tag, sizeof(tag), "%u  %.1f m/s2", c.a, (double)c.value);
        else
            std::snprintf(tag, sizeof(tag), "%u  %.2f m", c.a, (double)c.value);
        // The label sits BESIDE the marker, not on it. Written centred over
        // the ring it covered the drones it was about with its own plate; a
        // reader could see that something was wrong there but not what the
        // figure looked like at the moment it went wrong. So: a fixed 20 px
        // offset out of the cloud, a leader line back to the ring - the line
        // is what keeps the offset honest - and on a narrow window, where 20 px
        // still lands in the middle of the same cloud, the label folds all the
        // way out to the near edge of the view and the leader gets longer.
        const float OFF   = 20.0f;      // marker centre -> label box
        const float PAD_X = 5.0f, PAD_Y = 3.0f;
        const float EDGE  = 4.0f;
        float anchor_x = pair ? 0.5f * (ax + bx2) : ax;
        float anchor_y = pair ? 0.5f * (ay + by2) : ay;
        float anchor_r = (pair && gap <= 2.2f * RING_R) ? RING_R + gap * 0.5f : RING_R;

        float lw = dai_ui_text_width(ui, tag), lh = dai_ui_text_height(ui);
        // The WINDOW's width, not the panel's: the viewport is drawn straight
        // onto the frame, so this is the number the rule is about.
        float frame_w = dai_ui_panel_width(ui);
        if (frame_w < w) frame_w = w;
        const int narrow = (frame_w < 1200.0f);
        float lx, ly;
        if (narrow) {
            int right = (anchor_x > x + w * 0.5f);
            lx = right ? x + w - lw - PAD_X - EDGE : x + PAD_X + EDGE;
            ly = anchor_y - lh * 0.5f;
        } else {
            lx = anchor_x + OFF;
            ly = anchor_y - OFF - lh;
            if (lx + lw + PAD_X > x + w - EDGE) lx = anchor_x - OFF - lw;  // fold left
            if (ly - PAD_Y < y + EDGE)          ly = anchor_y + OFF;       // fold down
        }
        lx = std::max(x + PAD_X + EDGE, std::min(lx, x + w - lw - PAD_X - EDGE));
        ly = std::max(y + PAD_Y + EDGE, std::min(ly, y + vh - lh - PAD_Y - EDGE));
        LabelBox box{ lx - PAD_X, ly - PAD_Y, lx + lw + PAD_X, ly + lh + PAD_Y };
        bool clash = false;
        for (size_t k = 0; k < taken.size() && !clash; ++k)
            clash = !(box.x1 < taken[k].x0 || taken[k].x1 < box.x0 ||
                      box.y1 < taken[k].y0 || taken[k].y1 < box.y0);
        if (clash) continue;
        taken.push_back(box);

        // The leader, drawn first so the plate covers its last pixel: from the
        // edge of the box towards the marker, stopping at the ring rather than
        // inside it.
        float bcx = 0.5f * (box.x0 + box.x1), bcy = 0.5f * (box.y0 + box.y1);
        float dx = anchor_x - bcx, dy = anchor_y - bcy;
        float len = std::sqrt(dx * dx + dy * dy);
        if (len > anchor_r + 4.0f) {
            float ux = dx / len, uy = dy / len;
            float tx = (std::fabs(ux) > 1e-4f)
                           ? ((ux > 0.0f ? box.x1 : box.x0) - bcx) / ux : 1e9f;
            float ty = (std::fabs(uy) > 1e-4f)
                           ? ((uy > 0.0f ? box.y1 : box.y0) - bcy) / uy : 1e9f;
            float te = std::min(tx, ty);
            float sx0 = bcx + ux * te, sy0 = bcy + uy * te;
            float ex  = anchor_x - ux * (anchor_r + 2.0f);
            float ey  = anchor_y - uy * (anchor_r + 2.0f);
            if ((ex - sx0) * ux + (ey - sy0) * uy > 1.0f)
                dai_ui_line(ui, sx0, sy0, ex, ey, 1.0f, rgba(0xFF, 0x3B, 0x30, 170));
        }
        // A plate under the text: the sky behind a marker is whatever colour
        // the figure happens to be, and red on cyan is not a readable label.
        dai_ui_rect(ui, box.x0, box.y0, box.x1 - box.x0, box.y1 - box.y0,
                    rgba(0x10, 0x10, 0x14, 225));
        dai_ui_rect_outline(ui, box.x0, box.y0, box.x1 - box.x0, box.y1 - box.y0,
                            1.0f, rgba(0xFF, 0x3B, 0x30, 140));
        dai_ui_text(ui, lx, ly, tag, COL_CONFLICT);
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

// ---- the two panels a show inherits from the game layout ----------------------
//
// A droneshow project has no scene graph and no components, so the Hierarchy
// and the Inspector had nothing to say and stood in the dock as two empty
// rectangles. An empty panel in the default layout is a promise the program
// does not keep, and there were only two honest ways out of it: take the two
// panels off the show's layout, or give them what a show DOES have a hierarchy
// and a detail view of. It has both - the figures in order, and one drone at
// one instant - so they are filled rather than removed, and the layout a
// director learned in the game editor is the layout he keeps.

void dai_show_ui_figures(dai_show_ui *u, dai_ui *ui, float x, float y, float w, float h) {
    if (!u || !ui) return;
    dai_ui_panel_begin(ui, x, y, w, h, nullptr);
    const dai_ui_style *st = dai_ui_style_of(ui);
    uint32_t n = dai_show_formation_count(u->sh);

    dai_ui_section(ui, "Figures");
    if (!n) {
        wrapped_label(ui, "no figures yet - add one in the Storyboard panel");
        dai_ui_panel_end(ui);
        return;
    }

    float cx = 0.0f, cy = 0.0f;
    dai_ui_cursor_pos(ui, &cx, &cy);
    const float pitch = row_pitch(ui);
    // Whole rows only, and room kept under the list for the drone picker: the
    // same rule the validation list follows, for the same reason.
    float space = y + h - cy - 10.0f - pitch * 3.0f;
    if (space < pitch) space = pitch;
    const int   visible = (int)std::floor(space / pitch);
    const float list_h  = (float)visible * pitch;
    const float top = cy, bottom = cy + list_h;
    float t = u->time;
    dai_ui_scroll_begin(ui, "showfigs", list_h);
    for (uint32_t i = 0; i < n; ++i) {
        dai_show_formation_info info;
        if (!dai_show_formation_get(u->sh, i, &info)) continue;
        // The lit row is the figure the TIMELINE is standing in, not the one
        // the storyboard happens to be editing - two panels lighting up two
        // different rows for two meanings of "selected" is how a reader stops
        // believing either. Clicking a row seeks to it, so the light follows
        // the click as well as the playhead.
        dai_show_formation_info next;
        int has_next = (i + 1u < n) && dai_show_formation_get(u->sh, i + 1u, &next);
        int here = (t >= info.t_start - 0.001f) && (!has_next || t < next.t_start);
        // Detail is dropped from the RIGHT before anything is cut: docked at a
        // tenth of the frame the row still has to say which figure it is, and
        // "1. Sp..." says nothing at all. The count and the timestamp are the
        // parts a reader can get from the storyboard; the name is not.
        char row[192];
        // Measured against the toggle button this row IS - panel width less
        // its padding and the text inset - rather than against the whole-row
        // estimate the wider panels use. In a tenth of the frame the two
        // differ by twenty pixels, which is the difference between "1. Sphere"
        // and "1. Sp...".
        const float avail = dai_ui_panel_width(ui) - st->padding * 2.0f - 10.0f;
        std::snprintf(row, sizeof(row), "%u. %s  %u drones  %.1f s",
                      i + 1u, info.name, info.point_count, (double)info.t_start);
        if (dai_ui_text_width(ui, row) > avail)
            std::snprintf(row, sizeof(row), "%u. %s  %.1f s",
                          i + 1u, info.name, (double)info.t_start);
        if (dai_ui_text_width(ui, row) > avail)
            std::snprintf(row, sizeof(row), "%u. %s", i + 1u, info.name);
        ellide(ui, row, avail);
        float rx = 0.0f, ry = 0.0f;
        dai_ui_cursor_pos(ui, &rx, &ry);
        if (ry < top - 0.5f || ry + pitch > bottom + 0.5f) {
            dai_ui_advance(ui, w - 16.0f, pitch - st->spacing);
            continue;
        }
        if (dai_ui_toggle_button(ui, row, here)) {
            u->sel_formation = (int)i;
            u->time          = info.t_start;
            u->playing       = 0;
        }
    }
    dai_ui_scroll_end(ui);

    dai_ui_separator(ui);
    // The fleet is ten thousand identical rows, so it is not a list: a drone is
    // picked by its number, which is what the conflict list, every export and
    // every ground station calls it by anyway.
    const dai_show_plan *plan = dai_show_get_plan(u->sh);
    uint32_t count = plan ? dai_show_plan_drone_count(plan) : u->s.drone_count;
    dai_ui_style *mst = dai_ui_style_of(ui);
    float label_was = mst->label_w;
    mst->label_w = std::max(62.0f, dai_ui_text_width(ui, "Drone") + 10.0f);
    float pick = (u->sel_drone == 0xFFFFFFFFu) ? 0.0f : (float)u->sel_drone;
    if (dai_ui_num_field(ui, "Drone", &pick, 1.0f, 0.0f,
                         (float)(count ? count - 1u : 0u), "showpick")) {
        u->sel_drone   = (uint32_t)(pick + 0.5f);
        u->sel_drone_b = 0xFFFFFFFFu;
    }
    mst->label_w = label_was;
    char fleet[96];
    std::snprintf(fleet, sizeof(fleet), "%u drones in the fleet", count);
    wrapped_label(ui, fleet);
    dai_ui_panel_end(ui);
}

void dai_show_ui_inspector(dai_show_ui *u, dai_ui *ui, float x, float y, float w, float h) {
    if (!u || !ui) return;
    dai_ui_panel_begin(ui, x, y, w, h, nullptr);
    const dai_show_plan *plan = dai_show_get_plan(u->sh);
    const char *const LABELS[] = { "Position", "Colour", "Keyframes", "Nearest" };
    float label_was = fit_labels(ui, LABELS, 4);

    // Everything below scrolls, and everything below stops at `bot`. The panel
    // is a fifth of the frame wide when a show is open, which is narrow enough
    // that four fields and a conflict do not always fit - and a panel whose
    // last line is sliced through the middle by its own edge is the defect this
    // whole file is careful about, not a detail of this one panel.
    float cx = 0.0f, cy = 0.0f;
    dai_ui_cursor_pos(ui, &cx, &cy);
    const float view_h = std::max(row_pitch(ui), y + h - cy - 6.0f);
    const float bot    = cy + view_h;
    dai_ui_scroll_begin(ui, "showinsp", view_h);
    // One exit, so the scroll region and the label column are always put back.
    struct Done {
        dai_ui *ui; float was;
        ~Done() { dai_ui_scroll_end(ui); dai_ui_style_of(ui)->label_w = was; dai_ui_panel_end(ui); }
    } done{ ui, label_was };

    // The conflict the validation list has open, in full. A list row is one
    // line because there can be eight thousand of them; this is where the whole
    // sentence fits, and it is the sentence the debrief argues about.
    int ci = u->sel_conflict;
    if (ci >= 0 && (uint32_t)ci < dai_show_conflict_count(u->sh)) {
        dai_show_conflict c;
        if (dai_show_conflict_at(u->sh, (uint32_t)ci, &c)) {
            dai_ui_section(ui, "Conflict");
            char line[192];
            if (c.a == c.b)
                std::snprintf(line, sizeof(line), "drone %u %s", c.a, conflict_word(c.kind));
            else
                std::snprintf(line, sizeof(line), "drones %u and %u %s",
                              c.a, c.b, conflict_word(c.kind));
            stat_line(ui, bot, "%s", line);
            std::snprintf(line, sizeof(line), "%.2f against a limit of %.2f, at %.2f s",
                          (double)c.value, (double)c.limit, (double)c.time_s);
            stat_line(ui, bot, "%s", line);
            if (c.kind == DAI_SHOW_CONFLICT_DISTANCE) {
                std::snprintf(line, sizeof(line), "%.2f m too close",
                              (double)(c.limit - c.value));
                stat_line(ui, bot, "%s", line);
            }
            // The long label when the column is wide enough for it, the short
            // one when it is not. A button whose text runs out over its own
            // edges is a button that has stopped looking like one.
            const char *go = "Go to the moment";
            if (dai_ui_text_width(ui, go) > dai_ui_panel_width(ui) -
                                            dai_ui_style_of(ui)->padding * 4.0f)
                go = "Go to it";
            if (row_button(ui, bot, go)) {
                u->time    = c.time_s;
                u->playing = 0;
            }
        }
    }

    dai_ui_section(ui, "Drone");
    uint32_t n = plan ? dai_show_plan_drone_count(plan) : 0;
    if (!plan || !n) {
        stat_line(ui, bot, "no plan yet - press Solve in Show Parameters");
        return;
    }
    if (u->sel_drone >= n) {
        stat_line(ui, bot, "no drone selected - click a row in Validation, or "
                            "pick a number in the figure list");
        return;
    }

    // Sampled here rather than read out of the viewport's copy: this panel is
    // drawn before the preview, and a position one frame old is a position that
    // disagrees with the picture beside it.
    refresh_fleet(u);
    const uint32_t d = u->sel_drone;
    const dai_show_point p = u->fleet[d];

    char val[128];
    std::snprintf(val, sizeof(val), "drone %u of %u at %.2f s", d, n, (double)u->time);
    stat_line(ui, bot, "%s", val);

    // Metres, and the decimal only while there is room for it: a coordinate
    // that ends in an ellipsis has lost the axis a reader was looking for.
    std::snprintf(val, sizeof(val), "%.1f  %.1f  %.1f m", (double)p.x, (double)p.y, (double)p.z);
    if (dai_ui_text_width(ui, val) > dai_ui_panel_width(ui) - 20.0f)
        std::snprintf(val, sizeof(val), "%.0f %.0f %.0f m",
                      (double)p.x, (double)p.y, (double)p.z);
    field_row(ui, "Position", val, bot);

    // The LED as a colour, not only as four numbers: this is the one value in
    // the panel a director checks against what he can see in the sky.
    float sx = 0.0f, sy = 0.0f;
    dai_ui_cursor_pos(ui, &sx, &sy);
    std::snprintf(val, sizeof(val), "%u %u %u   w %u", p.r, p.g, p.b, p.w);
    if (dai_ui_text_width(ui, val) > dai_ui_panel_width(ui) - 20.0f)
        std::snprintf(val, sizeof(val), "%u %u %u", p.r, p.g, p.b);
    field_row(ui, "Colour", val, bot);
    {
        const dai_ui_style *s2 = dai_ui_style_of(ui);
        float sw = dai_ui_text_height(ui);
        float bx = x + w - sw - s2->padding - 6.0f;
        dai_ui_rect(ui, bx, sy, sw, sw, rgba(p.r, p.g, p.b, 255));
        dai_ui_rect_outline(ui, bx, sy, sw, sw, 1.0f, s2->panel_border);
    }

    std::snprintf(val, sizeof(val), "%u", dai_show_plan_keyframe_count(plan, d));
    field_row(ui, "Keyframes", val, bot);

    // The nearest neighbour at this instant. One linear scan for ONE drone -
    // the panel asks about one, so the grid the validator needs for every pair
    // would be a page of code to save a tenth of a millisecond.
    uint32_t best = 0xFFFFFFFFu;
    float    bestd = 0.0f;
    for (uint32_t i = 0; i < n; ++i) {
        if (i == d) continue;
        float dx = u->fleet[i].x - p.x, dy = u->fleet[i].y - p.y, dz = u->fleet[i].z - p.z;
        float dd = dx * dx + dy * dy + dz * dz;
        if (best == 0xFFFFFFFFu || dd < bestd) { best = i; bestd = dd; }
    }
    if (best != 0xFFFFFFFFu) {
        float dist = std::sqrt(bestd);
        std::snprintf(val, sizeof(val), "drone %u, %.2f m", best, (double)dist);
        field_row(ui, "Nearest", val, bot);
        if (dist < u->s.min_distance_m) {
            char warn[128];
            std::snprintf(warn, sizeof(warn), "inside the %.2f m minimum",
                          (double)u->s.min_distance_m);
            float wx = 0.0f, wy = 0.0f;
            dai_ui_cursor_pos(ui, &wx, &wy);
            if (fits(ui, bot, dai_ui_text_height(ui)))
                dai_ui_text(ui, wx, wy, warn, COL_CONFLICT);
            dai_ui_advance(ui, w - 16.0f, dai_ui_text_height(ui));
        }
        if (row_button(ui, bot, "Select the neighbour")) u->sel_drone_b = best;
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

    // What the separation cost, in the one line that is always on screen:
    // "16 crossings: 11 lifted, 7 delayed, 0 left over, 1 formation fault".
    // The last number is the one this line used to leave out, and leaving it
    // out is how a figure with two points inside the minimum distance passed as
    // solved - nothing was left over, because the fault was never a crossing.
    // Appended only while it fits beside the verdict, which is the one thing
    // here that may never be pushed off the end.
    char layer_note[160] = { 0 };
    if (dai_show_get_plan(u->sh) && t.formations)
        std::snprintf(layer_note, sizeof(layer_note),
                      "   %u crossings: %u lifted, %u delayed, %u left over, %u formation fault%s",
                      t.last_layer.crossings_found, t.last_layer.resolved_by_height,
                      t.last_layer.resolved_by_delay, t.last_layer.unresolved,
                      t.last_layer.endpoint_pairs,
                      (t.last_layer.endpoint_pairs == 1) ? "" : "s");

    // The verdict is DERIVED, every frame, from the document - never a string
    // frozen when the panel was created. A status line that still says "add a
    // figure to begin" over a show with eleven conflicts in it is worse than
    // no status line: it is a status line that has been caught lying, and
    // after that nobody reads the green one either.
    char verdict[128];
    uint32_t verdict_col;
    uint32_t figures = dai_show_formation_count(u->sh);
    if (figures == 0) {
        std::snprintf(verdict, sizeof(verdict), "%u drones, %.1f m apart - add a figure to begin",
                      u->s.drone_count, (double)u->s.min_distance_m);
        verdict_col = st->text_dim;
    } else if (t.formations == 0 || !dai_show_get_plan(u->sh)) {
        std::snprintf(verdict, sizeof(verdict), "%u figures - not solved yet", figures);
        verdict_col = COL_WARN;
    } else if (t.last_validate.conflicts) {
        std::snprintf(verdict, sizeof(verdict), "%u conflicts", t.last_validate.conflicts);
        verdict_col = COL_CONFLICT;
    } else if (t.last_layer.endpoint_pairs) {
        // The validator normally finds these too - but if a fault stands
        // between two ticks it would not, and "no conflicts" over a figure the
        // separator has already named as faulted is the one lie this line must
        // not tell.
        std::snprintf(verdict, sizeof(verdict), "%u formation fault%s",
                      t.last_layer.endpoint_pairs,
                      (t.last_layer.endpoint_pairs == 1) ? "" : "s");
        verdict_col = COL_CONFLICT;
    } else {
        std::snprintf(verdict, sizeof(verdict), "no conflicts");
        verdict_col = COL_OK;
    }
    float vw = dai_ui_text_width(ui, verdict);
    dai_ui_text(ui, x + w - vw - 10.0f, ty, verdict, verdict_col);

    if (layer_note[0] &&
        dai_ui_text_width(ui, left) + dai_ui_text_width(ui, layer_note) + vw + 40.0f <= w) {
        size_t used = std::strlen(left);
        std::snprintf(left + used, sizeof(left) - used, "%s", layer_note);
    }
    dai_ui_text(ui, x + 8.0f, ty, left, st->text_dim);

    // What just happened, to the left of the verdict and only while it fits:
    // an event ("formation added", "the figure could not be sampled") is news,
    // and news never overwrites the state.
    if (u->status[0]) {
        char note[256];
        std::snprintf(note, sizeof(note), "%s", u->status);
        float lw = dai_ui_text_width(ui, left);
        float room = w - vw - lw - 40.0f;
        ellide(ui, note, room);
        if (room > 40.0f)
            dai_ui_text(ui, x + w - vw - 24.0f - dai_ui_text_width(ui, note), ty, note,
                        u->status_bad ? COL_CONFLICT : st->text_dim);
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
