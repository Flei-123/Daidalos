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

// The circle constant, written down rather than included. M_PI is POSIX, not
// C++: glibc hands it over anyway, mingw does not without _USE_MATH_DEFINES,
// and the Windows cross build stopped on four lines that had compiled on Linux
// for weeks. A constant that appears or does not appear depending on which
// <cmath> answered is not a constant.
constexpr float PI = 3.14159265358979323846f;


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
// The hierarchy's add menu: the three builtins, and the image the storyboard
// has a path for.
const dai_ui_menu_item ADD_ITEMS[10] = {
    { nullptr, "Into this step", nullptr, 1 },   // a caption, not a command
    { nullptr, "Sphere", nullptr, 0 },
    { nullptr, "Cube", nullptr, 0 },
    { nullptr, "Ring", nullptr, 0 },
    { nullptr, "From an image...", nullptr, 0 },
    { nullptr, "Takeoff grid (start)", nullptr, 0 },
    { nullptr, "New step (flies in parallel)", nullptr, 0 },
    { nullptr, "This figure", nullptr, 1 },      // a caption, not a command
    { nullptr, "Duplicate", nullptr, 0 },
    { nullptr, "Delete", nullptr, 0 },
};
const int ADD_ITEM_COUNT = 10;

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

// The same cut, but in the MIDDLE, so the END of the line survives it.
//
// A storyboard row is "4. Sphere (near miss) - 420 pts - t 80.6s", and the two
// things a director needs off it are the number at the front and the second at
// the back. Cutting at the end keeps the half he can rebuild from the figure
// list and throws away the half he cannot - which is how "4. Sphere (nea..."
// came to stand in a panel whose whole job is saying WHEN a figure begins.
// `head` may lose its tail; `tail` never does. If even "..." plus the tail is
// wider than the row, the whole thing falls back to the end cut, because a
// timestamp with no figure in front of it belongs to nothing.
// `min_head` is the floor under the trade: keeping the timestamp is only worth
// it while enough of the head survives to say WHICH figure it belongs to. Below
// that the row would read "1.... 0.0 s" - a time with no owner, which is worse
// than the name with no time - so the tail is dropped instead and the head cut
// at the end, the way a one-column list has always done it.
void ellide_middle(dai_ui *ui, char *out, size_t cap, const char *head,
                   const char *tail, float avail, size_t min_head) {
    std::snprintf(out, cap, "%s%s", head, tail);
    if (avail <= 0.0f || dai_ui_text_width(ui, out) <= avail) return;

    char probe[256];
    size_t n = std::strlen(head);
    while (n > min_head) {
        std::snprintf(probe, sizeof(probe), "%.*s...%s", (int)n, head, tail);
        if (dai_ui_text_width(ui, probe) <= avail) {
            std::snprintf(out, cap, "%.*s...%s", (int)n, head, tail);
            return;
        }
        --n;
    }
    std::snprintf(out, cap, "%s", head);   // the name alone, cut at its end
    ellide(ui, out, avail);
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

// The three interactive rows this panel needs, under the same all-or-nothing
// rule field_row and row_button already follow: a control that would be cut by
// the bottom edge is not drawn, but it still spends its row, so the wheel
// brings it in whole rather than sliding another control under the pointer.
int vec3_row(dai_ui *ui, float bottom, const char *label, float *xyz, float step) {
    float hgt = widget_row(ui);
    if (!fits(ui, bottom, hgt)) { dai_ui_advance(ui, dai_ui_panel_width(ui), hgt); return 0; }
    return dai_ui_num_vec3(ui, label, xyz, step);
}

int colour_row(dai_ui *ui, float bottom, const char *label, float *rgb, const char *id) {
    float hgt = widget_row(ui);
    if (!fits(ui, bottom, hgt)) { dai_ui_advance(ui, dai_ui_panel_width(ui), hgt); return 0; }
    return dai_ui_color(ui, label, rgb, id);
}

int check_row(dai_ui *ui, float bottom, const char *label, int *value) {
    float hgt = widget_row(ui);
    if (!fits(ui, bottom, hgt)) { dai_ui_advance(ui, dai_ui_panel_width(ui), hgt); return 0; }
    return dai_ui_checkbox(ui, label, value);
}

uint8_t to_u8(float v) {
    float x = v * 255.0f + 0.5f;
    if (!(x > 0.0f)) x = 0.0f;
    if (x > 255.0f)  x = 255.0f;
    return (uint8_t)x;
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

// "1 conflicts" is the kind of small wrongness that makes a reader distrust the
// number in front of it - and this panel's whole job is being believed about
// exactly that number. One place decides it, so the list header, the status
// line and the message after a re-check can never disagree.
const char *plural(uint32_t n) { return (n == 1u) ? "" : "s"; }

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
        float p0 = PI * (float)i / (float)rings;
        float p1 = PI * (float)(i + 1) / (float)rings;
        for (int j = 0; j < segs; ++j) {
            float t0 = 2.0f * PI * (float)j / (float)segs;
            float t1 = 2.0f * PI * (float)(j + 1) / (float)segs;
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
        float t0 = 2.0f * PI * (float)j / (float)segs;
        float t1 = 2.0f * PI * (float)(j + 1) / (float)segs;
        for (int k = 0; k < 12; ++k) {
            float u0 = 2.0f * PI * (float)k / 12.0f;
            float u1 = 2.0f * PI * (float)(k + 1) / 12.0f;
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
    // origin, looking slightly up. The focus is a POINT, not a height over the
    // origin: flying (dai_show_ui_nav) walks it through the sky, and the eye
    // follows at `dist` behind it.
    float yaw = 0.0f, pitch = 0.18f, dist = 260.0f;
    float focus_x = 0.0f, focus_y = 60.0f, focus_z = 0.0f;
    int   dragging = 0;
    float drag_x = 0.0f, drag_y = 0.0f;
    // The flying camera's memory: where the pointer was when a look or a pan
    // started, and whether the buttons were down last frame. Deltas, not
    // absolutes - an absolute look accumulates whatever the pointer did over
    // a panel on the way in.
    int   nav_rmb = 0, nav_mmb = 0, nav_f = 0;
    float nav_mx = 0.0f, nav_my = 0.0f;
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

    int  want_export = 0;        // 0 none; 1 skyc, 2 dsx, 3 csv, 4 json

    std::vector<dai_show_point> fleet;      // the fleet at `time`, per frame
    std::vector<uint8_t>        flagged;    // in a conflict at `time`

    // ---- the gizmo -------------------------------------------------------
    // A figure is moved by dragging one axis at a time, which is what every 3D
    // editor does and the only thing that can be aimed with a mouse in a
    // perspective view. The transform the drag STARTED from is kept, and each
    // frame recomputes the whole offset from it rather than accumulating a
    // delta - an accumulated drag drifts, and a figure that ends up somewhere
    // other than where the pointer said is a figure nobody trusts the gizmo on.
    int                gizmo_axis = -1;             // 0 X, 1 Y, 2 Z, -1 idle
    float              gizmo_mx = 0.0f, gizmo_my = 0.0f;
    dai_show_transform gizmo_from;


    // ---- painting --------------------------------------------------------
    // Colour belongs to the POINT, never to the drone number: which drone flies
    // to which point is the assignment's answer and it moves whenever the
    // figure, the fleet or the minimum distance changes. A red drone 99 would
    // wander across the figure every time Solve was pressed.
    int      pick_on   = 0;                  // clicking the preview picks points
    int      paint_on  = 0;                  // ...and dragging paints them
    uint32_t sel_point = 0xFFFFFFFFu;
    float    brush_px  = 22.0f;
    float    paint_rgb[3]  = { 1.0f, 0.25f, 0.20f };
    // The swatch the figure section shows, cached per formation so the colour
    // wheel has somewhere stable to live between frames.
    // Where the three handles were drawn last frame, so a host - or a test -
    // can grab one at the place the user sees it.
    float    giz_sx[3] = { 0.0f, 0.0f, 0.0f };
    float    giz_sy[3] = { 0.0f, 0.0f, 0.0f };
    int      giz_drawn[3] = { 0, 0, 0 };
    int      colour_for = -1;
    int      figure_on  = 0;
    float    figure_rgb[3] = { 1.0f, 1.0f, 1.0f };
    uint8_t  figure_w      = 0;

    // ---- the 2D view ------------------------------------------------------
    // Top-down, like the planning sheet a show is drawn on first: one button
    // swaps the orbit for a map, and back. The 3D angles are kept, not
    // recomputed - a view you return to should be the view you left.
    int   view_2d = 0;
    float pre2d_yaw = 0.0f, pre2d_pitch = 0.18f;

    // ---- adding figures ----------------------------------------------------
    dai_ui_popup add_menu = {};              // the hierarchy's + / right-click
    dai_ui_popup img_menu = {};              // the image import's own window
    // The image a figure can be sampled from. A path, a width, and how bright
    // a pixel has to be to earn a drone - all three plain fields, because a
    // file dialog is the host's luxury, not a panel's.
    char  image_path[256]  = { 0 };
    float image_width_m    = 60.0f;
    float image_threshold  = 128.0f;
    float image_points     = 0.0f;           // 0 = the whole fleet
    // ---- what the host knows and dai_ui does not --------------------------
    // Held modifier keys. They decide what a LEFT drag means, and a panel that
    // guesses would guess wrong in the one editor everybody already knows:
    // in a scene view a plain left drag SELECTS and Alt+drag orbits.
    int   key_alt = 0, key_ctrl = 0, key_shift = 0;

    // ---- undo, as snapshots ------------------------------------------------
    // Snapshots of the whole document, taken where a GESTURE begins - one step
    // per drag, not one per frame. Capped, because a snapshot of ten thousand
    // points is not free; the oldest is dropped, which is the bargain every
    // editor with a bounded history makes.
    std::vector<dai_show_state *> undo;
    std::vector<dai_show_state *> redo;
    char  undo_what[64] = { 0 };
    // A drag through a number field changes the document every frame; one undo
    // step per frame would mean forty Ctrl+Z for one slider. The guard says
    // "a push already happened while this button has been down".
    int   undo_guard = 0;

    // The host's file dialog, asked for from a panel that has no window.
    int   want_browse = 0;        // 0 none, 1 the import window, 2 the inspector

    // ---- the point handle --------------------------------------------------
    // A point is MOVED the way a figure is: select it, then drag one axis
    // handle. Dragging the dot itself was the old way, and it moved figures
    // nobody meant to touch - a click and a drag began identically.
    int      pt_f = -1;                      // formation of the selected point
    uint32_t pt_i = 0xFFFFFFFFu;             // the point inside it
    int      pt_axis = -1;                   // 0 X, 1 Y, 2 Z while dragging
    float    pt_mx = 0.0f, pt_my = 0.0f;     // where the axis drag began
    float    pt_from[3] = { 0.0f, 0.0f, 0.0f };
    float    pt_gsx[3] = { 0.0f, 0.0f, 0.0f };
    float    pt_gsy[3] = { 0.0f, 0.0f, 0.0f };
    int      pt_gdrawn[3] = { 0, 0, 0 };

    // ---- the dope sheet ----------------------------------------------------
    // Blender's, one row per step: a bar per figure, a diamond at each end.
    // Dragging the left diamond is the transit into the figure, dragging the
    // right one is how long it holds - so the two numbers a show is made of
    // are edited where they are SEEN, not typed into a panel in seconds.
    int   tl_drag_f = -1;                    // the figure being dragged
    int   tl_drag_end = 0;                   // 0 = its start, 1 = its end
    float tl_drag_mx = 0.0f;                 // pointer x when the drag began
    float tl_drag_v0 = 0.0f;                 // the value it began at
    int   tl_scrub = 0;                      // the playhead is being dragged

    // The inspector's own copy, for the figure it is looking at: unpacked
    // from that figure's source once, so typing does not fight the document.
    int   img_edit_for     = -1;
    char  img_edit_path[256] = { 0 };
    float img_edit_w       = 60.0f;
    float img_edit_th      = 128.0f;
};

// The image readers live in the renderer's library (dai_inflate / dai_jpeg);
// declared here the way rhi_vulkan_texture.cpp declares them.
namespace daiimg {
bool read_png_file(const char *path, std::vector<uint8_t> &rgba, uint32_t *w, uint32_t *h,
                   char *err, size_t err_len);
bool read_jpeg_file(const char *path, std::vector<uint8_t> &rgba, uint32_t *w, uint32_t *h,
                    char *err, size_t err_len);
}

namespace {

void say(dai_show_ui *u, int bad, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(u->status, sizeof(u->status), fmt, ap);
    va_end(ap);
    u->status_bad = bad;
}

// ---- undo, and the one rule that makes it work ----------------------------
//
// push_undo is called BEFORE a change, at the moment a gesture begins. Two
// pushes with no edit between them would put two identical states on the stack
// and cost the user two Ctrl+Z for one mistake, so a push whose state already
// matches the top is dropped. The comparison is the DOCUMENT's, not this
// file's - a panel that decided for itself what "unchanged" means would
// disagree with the document the day a field it had not heard of was added.
const size_t UNDO_MAX = 40;

void push_undo(dai_show_ui *u, const char *what) {
    if (!u || !u->sh) return;
    if (!u->undo.empty() && dai_show_state_equal(u->sh, u->undo.back())) {
        if (what) std::snprintf(u->undo_what, sizeof(u->undo_what), "%s", what);
        return;
    }
    dai_show_state *st = dai_show_snapshot(u->sh);
    if (!st) return;
    u->undo.push_back(st);
    while (u->undo.size() > UNDO_MAX) {
        dai_show_state_destroy(u->undo.front());
        u->undo.erase(u->undo.begin());
    }
    for (size_t i = 0; i < u->redo.size(); ++i) dai_show_state_destroy(u->redo[i]);
    u->redo.clear();
    if (what) std::snprintf(u->undo_what, sizeof(u->undo_what), "%s", what);
}

// One undo step per GESTURE, for widgets that change the document every frame
// they are dragged. The first change pushes; the rest of the drag does not.
void edit_begin(dai_show_ui *u, dai_ui *ui, const char *what) {
    int down = 0;
    dai_ui_mouse(ui, nullptr, nullptr, &down, nullptr);
    if (down && u->undo_guard) return;
    push_undo(u, what);
    if (down) u->undo_guard = 1;
}

// After an undo the selection may point at a figure that no longer exists - or
// at a point of a figure that has fewer of them now.
void clamp_selection(dai_show_ui *u) {
    uint32_t n = dai_show_formation_count(u->sh);
    if (!n) { u->sel_formation = 0; u->pt_f = -1; u->pt_i = 0xFFFFFFFFu; return; }
    if (u->sel_formation < 0) u->sel_formation = 0;
    if ((uint32_t)u->sel_formation >= n) u->sel_formation = (int)n - 1;
    if (u->pt_f >= 0) {
        dai_show_formation_info pi;
        if ((uint32_t)u->pt_f >= n ||
            !dai_show_formation_get(u->sh, (uint32_t)u->pt_f, &pi) ||
            u->pt_i >= pi.point_count) {
            u->pt_f = -1; u->pt_i = 0xFFFFFFFFu;
        }
    }
    u->colour_for = -1;
    u->img_edit_for = -1;
}

// The verdict at the right hand end of the status line, as text. Split out of
// dai_show_ui_status so the same sentence can be READ (dai_show_ui_verdict) as
// well as drawn - a test that re-formats the string itself would agree with
// itself rather than with the panel. The return value says how to colour it:
// 0 dim, 1 warning, 2 conflict, 3 ok.
int verdict_text(const dai_show_ui *u, char *buf, size_t cap) {
    dai_show_timings t = dai_show_get_timings(u->sh);
    uint32_t figures = dai_show_formation_count(u->sh);
    if (figures == 0) {
        std::snprintf(buf, cap, "%u drones, %.1f m apart - add a figure to begin",
                      u->s.drone_count, (double)u->s.min_distance_m);
        return 0;
    }
    if (t.formations == 0 || !dai_show_get_plan(u->sh)) {
        std::snprintf(buf, cap, "%u figure%s - not solved yet", figures, plural(figures));
        return 1;
    }
    if (t.last_validate.conflicts) {
        std::snprintf(buf, cap, "%u conflict%s", t.last_validate.conflicts,
                      plural(t.last_validate.conflicts));
        return 2;
    }
    // The validator normally finds these too - but if a fault stands between
    // two ticks it would not, and "no conflicts" over a figure the separator
    // has already named as faulted is the one lie this line must not tell.
    if (t.last_layer.endpoint_pairs) {
        std::snprintf(buf, cap, "%u formation fault%s", t.last_layer.endpoint_pairs,
                      plural(t.last_layer.endpoint_pairs));
        return 2;
    }
    std::snprintf(buf, cap, "no conflicts");
    return 3;
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
    if (!p) {
        // No plan: either nothing has been solved yet, or an edit just threw
        // the last one away. Either way the preview falls back to the FIGURE
        // being edited, which is the only honest picture available and the one
        // a user dragging a gizmo needs to see. Before this the window went
        // black the moment a figure was moved, which made the gizmo unusable
        // for the one thing it is for.
        uint32_t fc = dai_show_formation_count(u->sh);
        if (!fc) { u->fleet.clear(); u->flagged.clear(); return; }
        uint32_t fi = (u->sel_formation >= 0 && (uint32_t)u->sel_formation < fc)
                          ? (uint32_t)u->sel_formation : 0u;
        dai_show_formation_info info;
        const dai_show_point *pts = dai_show_formation_points(u->sh, fi);
        if (!pts || !dai_show_formation_get(u->sh, fi, &info)) {
            u->fleet.clear(); u->flagged.clear(); return;
        }
        u->fleet.assign(pts, pts + info.point_count);
        u->flagged.assign(info.point_count, 0);
        return;
    }
    uint32_t n = dai_show_plan_drone_count(p);
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
    float fwd[3] = { -sy * cp, -sp, -cy_ * cp };    // looking towards the focus
    c.ex = u->focus_x - fwd[0] * u->dist;
    c.ey = u->focus_y - fwd[1] * u->dist;
    c.ez = u->focus_z - fwd[2] * u->dist;
    float up[3] = { 0.0f, 1.0f, 0.0f };
    float r[3] = { fwd[1] * up[2] - fwd[2] * up[1],
                   fwd[2] * up[0] - fwd[0] * up[2],
                   fwd[0] * up[1] - fwd[1] * up[0] };
    float rl = std::sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
    if (rl < 1e-6f) { r[0] = 1.0f; r[1] = 0.0f; r[2] = 0.0f; rl = 1.0f; }
    for (int i = 0; i < 3; ++i) c.rx[i] = r[i] / rl;
    c.rz[0] = fwd[0]; c.rz[1] = fwd[1]; c.rz[2] = fwd[2];
    // right x forward, NOT forward x right. The other way round is exactly
    // minus the world up - (fwd x up) x fwd = up, fwd x (fwd x up) = -up -
    // so the whole preview was drawn upside down: the ground grid above the
    // figures, the fence box under the floor, and a drag up moving a figure
    // down. It looked plausible enough to survive for weeks, because a
    // mirrored sky is still a sky.
    c.ry[0] = c.rx[1] * c.rz[2] - c.rx[2] * c.rz[1];
    c.ry[1] = c.rx[2] * c.rz[0] - c.rx[0] * c.rz[2];
    c.ry[2] = c.rx[0] * c.rz[1] - c.rx[1] * c.rz[0];
    c.f  = 0.5f * h / std::tan(0.5f * 50.0f * PI / 180.0f);
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

// Which drone stands on which point of formation `fi` at the current second.
// The preview draws the FLEET - where the drones are now - while a figure's
// own points are its resting pose. Mid-transition the two are hundreds of
// metres apart, and a click answered against the resting pose picks a drone
// that is not under the cursor. This is the bridge: point -> drone, so every
// pick can be answered where the dot was actually drawn.
void map_points_to_drones(dai_show_ui *u, uint32_t fi, uint32_t count,
                          std::vector<uint32_t> &out) {
    out.assign(count, 0xFFFFFFFFu);
    const dai_show_plan *p = dai_show_get_plan(u->sh);
    if (!p) return;                     // no plan: the preview IS the figure
    uint32_t n = std::min<uint32_t>(dai_show_plan_drone_count(p), (uint32_t)u->fleet.size());
    for (uint32_t d = 0; d < n; ++d) {
        uint32_t f2 = 0, pt = 0;
        int settled = 0;
        if (!dai_show_drone_point_at(u->sh, d, u->time, &f2, &pt, &settled)) continue;
        if (f2 != fi || pt >= count) continue;
        out[pt] = d;
    }
}

// The inverse of project(), at a known depth: where the pointer's ray crosses
// the plane `depth` metres in front of the eye. A point drag slides on exactly
// that plane - the depth the grab started at - so a dragged point neither
// falls towards the camera nor runs away from it.
void unproject(const Cam &c, float sx, float sy, float depth,
               float *wx, float *wy, float *wz) {
    float rx = (sx - c.cx) / c.f;
    float ry = (c.cy - sy) / c.f;
    *wx = c.ex + depth * (rx * c.rx[0] + ry * c.ry[0] + c.rz[0]);
    *wy = c.ey + depth * (rx * c.rx[1] + ry * c.ry[1] + c.rz[1]);
    *wz = c.ez + depth * (rx * c.rx[2] + ry * c.ry[2] + c.rz[2]);
}

// How far a pointer is from a drawn handle. Screen space, so the answer is in
// pixels and the tolerance is the same at every zoom - a gizmo whose grab
// radius shrinks with perspective is a gizmo that stops working when you back
// off to see the whole show.
float dist_to_segment(float px, float py, float ax, float ay, float bx, float by) {
    float vx = bx - ax, vy = by - ay;
    float len2 = vx * vx + vy * vy;
    float t = 0.0f;
    if (len2 > 1e-6f) {
        t = ((px - ax) * vx + (py - ay) * vy) / len2;
        t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    }
    float qx = ax + vx * t - px, qy = ay + vy * t - py;
    return std::sqrt(qx * qx + qy * qy);
}

// A circle, out of the line segments the 2D canvas has. Sixteen of them is
// round to the eye at any size this marker is drawn at, and a shape the canvas
// already knows beats a new primitive in dai_ui for the sake of one panel.
void ring(dai_ui *ui, float cx, float cy, float r, float thick, uint32_t col) {
    const int SEG = 16;
    float px = cx + r, py = cy;
    for (int i = 1; i <= SEG; ++i) {
        float a = 2.0f * PI * (float)i / (float)SEG;
        float qx = cx + r * std::cos(a), qy = cy + r * std::sin(a);
        dai_ui_line(ui, px, py, qx, qy, thick, col);
        px = qx; py = qy;
    }
}

// Fits the orbit to the points currently on screen. Used when there is no plan
// to walk - before the first solve, and while a figure is being dragged.
void frame_points(dai_show_ui *u) {
    if (u->fleet.empty()) return;
    float lo[3] = { 1e30f, 1e30f, 1e30f }, hi[3] = { -1e30f, -1e30f, -1e30f };
    for (size_t i = 0; i < u->fleet.size(); ++i) {
        const dai_show_point &q = u->fleet[i];
        lo[0] = std::min(lo[0], q.x); hi[0] = std::max(hi[0], q.x);
        lo[1] = std::min(lo[1], q.y); hi[1] = std::max(hi[1], q.y);
        lo[2] = std::min(lo[2], q.z); hi[2] = std::max(hi[2], q.z);
    }
    if (lo[0] > hi[0]) return;
    float ext = std::max(hi[0] - lo[0], std::max(hi[1] - lo[1], hi[2] - lo[2]));
    u->focus_x = 0.5f * (lo[0] + hi[0]);
    u->focus_y = 0.5f * (lo[1] + hi[1]);
    u->focus_z = 0.5f * (lo[2] + hi[2]);
    u->dist    = std::max(20.0f, ext * 1.8f + 30.0f);
    u->framed_for = (int)u->fleet.size();
}

// Fits the orbit to whatever the plan holds, once per plan. A preview that
// opens on an empty patch of sky is a preview nobody trusts.
void frame_plan(dai_show_ui *u) {
    const dai_show_plan *p = dai_show_get_plan(u->sh);
    uint32_t n = p ? dai_show_plan_drone_count(p) : 0;
    if (!n) { frame_points(u); return; }
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
    u->focus_x = 0.5f * (lo[0] + hi[0]);
    u->focus_y = 0.5f * (lo[1] + hi[1]);
    u->focus_z = 0.5f * (lo[2] + hi[2]);
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

// The three builtin shapes as a descriptor, shared by "add" and by "the same
// figure, but another point count": only the count differs between the two.
void builtin_desc(dai_show_ui *u, int shape, uint32_t count,
                  Soup &soup, dai_show_sample_desc *d) {
    if (shape == 0)      figure_sphere(soup, 24, 32);
    else if (shape == 1) figure_cube(soup);
    else                 figure_ring(soup, 48, 0.28f);
    std::memset(d, 0, sizeof(*d));
    d->positions        = soup.pos.data();
    d->vertex_count     = (uint32_t)(soup.pos.size() / 3);
    d->indices          = soup.idx.data();
    d->index_count      = (uint32_t)soup.idx.size();
    d->base_rgba        = 0xFFFF9040u;
    d->mode             = u->sample_mode;
    d->count            = count;
    d->min_distance_m   = u->s.min_distance_m;
    d->scale            = u->figure_size * 0.5f;
    d->centre           = dai_vec3{ 0.0f, u->s.takeoff_alt_m + u->figure_size * 0.6f, 0.0f };
    d->view_dir         = dai_vec3{ 0.0f, 0.0f, 1.0f };
    d->relax_iterations = 6;
    d->seed             = u->s.seed + (uint64_t)dai_show_formation_count(u->sh) * 7919ull;
}

// The same figure with another point count, or another sampling mode. Only
// the builtins can be re-sampled - their source IS the shape - so a mesh
// keeps its points and the field says why it is grey.
int resample_builtin(dai_show_ui *u, uint32_t fi, int shape, uint32_t count, int mode) {
    Soup soup;
    dai_show_sample_desc d;
    builtin_desc(u, shape, count, soup, &d);
    d.mode = mode;
    std::vector<dai_show_point> pts(count);
    char err[256] = { 0 };
    uint32_t got = dai_show_sample(&d, pts.data(), count, err, sizeof(err));
    if (got != count) {
        say(u, 1, "%s", err[0] ? err : "the figure does not fit that many drones");
        return 0;
    }
    if (!dai_show_formation_replace_points(u->sh, fi, pts.data(), got)) return 0;
    dai_show_formation_set_sample_mode(u->sh, fi, mode);
    say(u, 0, "formation re-sampled: %u points, %s", got, SAMPLE_MODES[mode]);
    return 1;
}

// The same picture with another point count: the path and the parameters ride
// inside the source string, so the file is simply read again.
int resample_image(dai_show_ui *u, uint32_t fi, const char *source, uint32_t count) {
    if (std::strncmp(source, "image://", 8) != 0) return 0;
    char path[256];
    float w_m = 60.0f; int th = 128;
    const char *q = source + 8;
    const char *qm = std::strchr(q, '?');
    size_t plen = qm ? (size_t)(qm - q) : std::strlen(q);
    if (plen >= sizeof(path)) plen = sizeof(path) - 1;
    std::memcpy(path, q, plen); path[plen] = 0;
    if (qm) std::sscanf(qm + 1, "w=%f&th=%d", &w_m, &th);

    std::vector<uint8_t> px;
    uint32_t w = 0, h = 0;
    char err[256] = { 0 };
    const char *dot = std::strrchr(path, '.');
    int jpeg = dot && (!strcmp(dot, ".jpg") || !strcmp(dot, ".jpeg") ||
                       !strcmp(dot, ".JPG") || !strcmp(dot, ".JPEG"));
    int ok = jpeg ? daiimg::read_jpeg_file(path, px, &w, &h, err, sizeof(err))
                  : daiimg::read_png_file(path, px, &w, &h, err, sizeof(err));
    if (!ok) { say(u, 1, "%s", err[0] ? err : "the image could not be read"); return 0; }
    std::vector<dai_show_point> pts(count);
    uint32_t got = dai_show_image_sample(u->sh, px.data(), w, h, w_m, (uint8_t)th,
                                         count, pts.data(), err, sizeof(err));
    if (!got) { say(u, 1, "%s", err[0] ? err : "the image gave no points"); return 0; }
    if (!dai_show_formation_replace_points(u->sh, fi, pts.data(), got)) return 0;
    say(u, 0, "image re-sampled: %u points", got);
    return 1;
}

// A picture becomes a figure: decode it (the vk library's reader answers for
// PNG and JPEG alike), then let the core decide which pixels earn a drone.
void add_image(dai_show_ui *u) {
    if (!u->image_path[0]) { say(u, 1, "no image path - type one first"); return; }
    std::vector<uint8_t> px;
    uint32_t w = 0, h = 0;
    char err[256] = { 0 };
    const char *dot = std::strrchr(u->image_path, '.');
    int jpeg = dot && (!strcmp(dot, ".jpg") || !strcmp(dot, ".jpeg") ||
                       !strcmp(dot, ".JPG") || !strcmp(dot, ".JPEG"));
    int ok = jpeg ? daiimg::read_jpeg_file(u->image_path, px, &w, &h, err, sizeof(err))
                  : daiimg::read_png_file(u->image_path, px, &w, &h, err, sizeof(err));
    if (!ok) { say(u, 1, "%s", err[0] ? err : "the image could not be read"); return; }
    const char *base = std::strrchr(u->image_path, '/');
    const char *leaf = base ? base + 1 : u->image_path;
    char name[DAI_SHOW_NAME_MAX];
    std::snprintf(name, sizeof(name), "%.*s", (int)sizeof(name) - 1, leaf);
    char src[300];
    // The parameters travel inside the source string, so "the same picture,
    // but 300 drones" can re-read them instead of asking again.
    std::snprintf(src, sizeof(src), "image://%s?w=%.1f&th=%d", u->image_path,
                  (double)u->image_width_m, (int)u->image_threshold);
    uint8_t th = (uint8_t)std::max(0.0f, std::min(255.0f, u->image_threshold));
    uint32_t want = (u->image_points >= 1.0f) ? (uint32_t)(u->image_points + 0.5f)
                                              : u->s.drone_count;
    uint32_t idx = dai_show_formation_from_image(u->sh, name, src, px.data(), w, h,
                                                 u->image_width_m, th, want,
                                                 err, sizeof(err));
    if (idx == 0xFFFFFFFFu) { say(u, 1, "%s", err[0] ? err : "the image gave no figure"); return; }
    u->sel_formation = (int)idx;
    dai_show_formation_info info;
    dai_show_formation_get(u->sh, idx, &info);
    say(u, 0, "formation \"%s\" added: %u points from %ux%u", name, info.point_count, w, h);
}

// Everything a figure has been edited into, undone at once: the transform, the
// painted strokes, the one-colour override and the points themselves. The
// points are the part that needs the source - a dragged point has no memory of
// where it was, and the only honest "as it was" is the figure the sampler
// makes from the same source with the same count.
int reset_object(dai_show_ui *u, uint32_t fi) {
    dai_show_formation_info info;
    if (!dai_show_formation_get(u->sh, fi, &info)) return 0;
    dai_show_transform id = dai_show_transform_identity();
    dai_show_formation_set_transform(u->sh, fi, &id);
    dai_show_formation_clear_strokes(u->sh, fi);
    dai_show_formation_set_colour(u->sh, fi, 0, 255, 255, 255, 0);
    if (!std::strcmp(info.source, "builtin://takeoff")) {
        std::vector<dai_show_point> gp(info.point_count);
        if (dai_show_takeoff_points(u->sh, info.point_count, 0.0f, gp.data()))
            dai_show_formation_replace_points(u->sh, fi, gp.data(), info.point_count);
        return 1;
    }
    if (!std::strncmp(info.source, "builtin://", 10)) {
        int shape = 0;
        const char *bn = info.source + 10;
        for (int k = 0; k < 3; ++k) if (!std::strcmp(bn, FIGURES[k])) shape = k;
        return resample_builtin(u, fi, shape, info.point_count, info.sample_mode);
    }
    if (!std::strncmp(info.source, "image://", 8))
        return resample_image(u, fi, info.source, info.point_count);
    // A mesh figure's triangles are the host's, not ours: the transform, the
    // strokes and the colour are back, and the panel says what it could not do
    // rather than pretending the points were rebuilt.
    say(u, 1, "transform and colours reset - the points came from a mesh and stay");
    return 1;
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
        say(u, 1, "solved in %.0f ms - %u conflict%s remain%s",
            t.sample_ms + t.assign_ms + t.layer_ms + t.validate_ms,
            t.last_validate.conflicts, plural(t.last_validate.conflicts),
            (t.last_validate.conflicts == 1u) ? "s" : "");
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

void      dai_show_ui_destroy(dai_show_ui *u) {
    if (!u) return;
    for (size_t i = 0; i < u->undo.size(); ++i) dai_show_state_destroy(u->undo[i]);
    for (size_t i = 0; i < u->redo.size(); ++i) dai_show_state_destroy(u->redo[i]);
    delete u;
}
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

void dai_show_ui_nav(dai_show_ui *u, const dai_show_nav_input *in) {
    if (!u || !in) return;
    float dt = (in->dt > 0.0001f && in->dt < 1.0f) ? in->dt : 1.0f / 60.0f;

    // The basis the movement keys walk along. camera_of computes it from the
    // orbit numbers alone; the rectangle it is also fed only matters to the
    // projection, which a move does not use.
    Cam cam = camera_of(u, 0.0f, 0.0f, 100.0f, 100.0f);

    // Look: hold the right button and the mouse steers yaw and pitch. The
    // first frame of a hold only REMEMBERS the pointer - counting its journey
    // onto the viewport as a turn is how a camera ends up looking at its own
    // feet. The sign matches the left-drag orbit below: down looks down.
    // Alt + right drag is the scene view's zoom: the eye walks in and out along
    // its own line of sight, which is a different thing from the wheel (steps)
    // and from flying (moves the focus).
    if (in->mouse_right && in->key_alt) {
        if (u->nav_rmb) {
            float dz = (in->mouse_x - u->nav_mx) + (u->nav_my - in->mouse_y);
            u->dist = std::max(2.0f, u->dist * (1.0f - dz * 0.004f));
        }
        u->nav_mx  = in->mouse_x;
        u->nav_my  = in->mouse_y;
        u->nav_rmb = 1;
        u->nav_mmb = in->mouse_middle;
        if (in->key_focus && !u->nav_f) u->framed_for = -1;
        u->nav_f = in->key_focus;
        return;
    }
    if (in->mouse_right) {
        if (u->nav_rmb) {
            u->yaw   += (in->mouse_x - u->nav_mx) * 0.005f;
            u->pitch += (in->mouse_y - u->nav_my) * 0.005f;
            u->pitch  = std::max(-1.5f, std::min(1.5f, u->pitch));
        }
        u->nav_mx = in->mouse_x;
        u->nav_my = in->mouse_y;
    }

    // Fly: only while looking, the scene view's rule - W A S D without the
    // right button is typing, not walking. The step scales with `dist`, so a
    // show framed from 300 m crosses the sky briskly and a figure inspected
    // from 20 m creeps. Shift hurries, as everywhere.
    if (in->mouse_right) {
        float fw = (float)(in->key_w - in->key_s);
        float sd = (float)(in->key_d - in->key_a);
        float up = (float)(in->key_e - in->key_q);
        if (fw != 0.0f || sd != 0.0f || up != 0.0f) {
            float speed = std::max(6.0f, u->dist * 0.6f) * (in->key_shift ? 3.0f : 1.0f);
            float step  = speed * dt;
            u->focus_x += (cam.rz[0] * fw + cam.rx[0] * sd + cam.ry[0] * up) * step;
            u->focus_y += (cam.rz[1] * fw + cam.rx[1] * sd + cam.ry[1] * up) * step;
            u->focus_z += (cam.rz[2] * fw + cam.rx[2] * sd + cam.ry[2] * up) * step;
            if (u->focus_y < 0.5f) u->focus_y = 0.5f;   // the floor is the floor
        }
    }
    u->nav_rmb = in->mouse_right;

    // Pan: the middle button slides the focus across the view plane, scaled by
    // distance so one pixel of drag is about one pixel of world at the focus.
    if (in->mouse_middle) {
        if (u->nav_mmb) {
            float k = u->dist * 0.0016f;
            float dx = (in->mouse_x - u->nav_mx) * k;
            float dy = (in->mouse_y - u->nav_my) * k;
            u->focus_x -= cam.rx[0] * dx - cam.ry[0] * dy;
            u->focus_y -= cam.rx[1] * dx - cam.ry[1] * dy;
            u->focus_z -= cam.rx[2] * dx - cam.ry[2] * dy;
        }
        u->nav_mx = in->mouse_x;
        u->nav_my = in->mouse_y;
    }
    u->nav_mmb = in->mouse_middle;

    // F: frame the whole show again. Edge triggered here because the host
    // reports the key held, and a held F re-framing every frame is a camera
    // you can never leave.
    if (in->key_focus && !u->nav_f) u->framed_for = -1;
    u->nav_f = in->key_focus;
}

void dai_show_ui_mesh(dai_show_ui *u, const dai_show_sample_desc *desc, const char *source) {
    if (!u) return;
    if (!desc) { u->have_mesh = 0; u->source[0] = 0; return; }
    u->mesh = *desc;
    u->have_mesh = 1;
    std::snprintf(u->source, sizeof(u->source), "%s", source ? source : "");
}

void dai_show_ui_select_formation(dai_show_ui *u, uint32_t i) {
    if (!u) return;
    if (i >= dai_show_formation_count(u->sh)) return;
    u->sel_formation = (int)i;
    u->sel_point     = 0xFFFFFFFFu;
    u->colour_for    = -1;                 // the swatch re-reads this figure
}

uint32_t dai_show_ui_selected_formation(const dai_show_ui *u) {
    return u ? (uint32_t)(u->sel_formation < 0 ? 0 : u->sel_formation) : 0u;
}

int dai_show_ui_gizmo_handle(const dai_show_ui *u, int axis, float *sx, float *sy) {
    if (!u || axis < 0 || axis > 2 || !u->giz_drawn[axis]) return 0;
    if (sx) *sx = u->giz_sx[axis];
    if (sy) *sy = u->giz_sy[axis];
    return 1;
}

void dai_show_ui_pick_mode(dai_show_ui *u, int pick_on, int paint_on) {
    if (!u) return;
    u->pick_on  = pick_on ? 1 : 0;
    u->paint_on = paint_on ? 1 : 0;
}

void dai_show_ui_brush(dai_show_ui *u, float radius_px, uint8_t r, uint8_t g, uint8_t b) {
    if (!u) return;
    if (radius_px >= 1.0f) u->brush_px = radius_px;
    u->paint_rgb[0] = (float)r / 255.0f;
    u->paint_rgb[1] = (float)g / 255.0f;
    u->paint_rgb[2] = (float)b / 255.0f;
}

uint32_t dai_show_ui_picked_point(const dai_show_ui *u) { return u ? u->sel_point : 0xFFFFFFFFu; }

uint32_t dai_show_ui_selected_drone(const dai_show_ui *u) { return u ? u->sel_drone : 0xFFFFFFFFu; }
int      dai_show_ui_selected_conflict(const dai_show_ui *u) { return u ? u->sel_conflict : -1; }

int dai_show_ui_take_export(dai_show_ui *u) {
    if (!u) return 0;
    int f = u->want_export;
    u->want_export = 0;
    return f;
}

void dai_show_ui_modifiers(dai_show_ui *u, int alt, int ctrl, int shift) {
    if (!u) return;
    u->key_alt   = alt   ? 1 : 0;
    u->key_ctrl  = ctrl  ? 1 : 0;
    u->key_shift = shift ? 1 : 0;
}

void dai_show_ui_begin_edit(dai_show_ui *u, const char *what) { push_undo(u, what); }

int dai_show_ui_undo(dai_show_ui *u) {
    if (!u || u->undo.empty()) return 0;
    dai_show_state *now = dai_show_snapshot(u->sh);
    dai_show_state *st  = u->undo.back();
    u->undo.pop_back();
    dai_show_restore(u->sh, st);
    dai_show_state_destroy(st);
    if (now) u->redo.push_back(now);
    clamp_selection(u);
    say(u, 0, "undo%s%s", u->undo_what[0] ? ": " : "", u->undo_what);
    return 1;
}

int dai_show_ui_redo(dai_show_ui *u) {
    if (!u || u->redo.empty()) return 0;
    dai_show_state *now = dai_show_snapshot(u->sh);
    dai_show_state *st  = u->redo.back();
    u->redo.pop_back();
    dai_show_restore(u->sh, st);
    dai_show_state_destroy(st);
    if (now) u->undo.push_back(now);
    clamp_selection(u);
    say(u, 0, "redo");
    return 1;
}

uint32_t dai_show_ui_undo_depth(const dai_show_ui *u) { return u ? (uint32_t)u->undo.size() : 0u; }
uint32_t dai_show_ui_redo_depth(const dai_show_ui *u) { return u ? (uint32_t)u->redo.size() : 0u; }

int dai_show_ui_delete_selected(dai_show_ui *u) {
    if (!u) return 0;
    uint32_t n = dai_show_formation_count(u->sh);
    if (!n || u->sel_formation < 0 || (uint32_t)u->sel_formation >= n) return 0;
    dai_show_formation_info info;
    char name[64] = { 0 };
    if (dai_show_formation_get(u->sh, (uint32_t)u->sel_formation, &info))
        std::snprintf(name, sizeof(name), "%s", info.name);
    push_undo(u, "delete");
    if (!dai_show_formation_remove(u->sh, (uint32_t)u->sel_formation)) return 0;
    if (u->sel_formation > 0) --u->sel_formation;
    u->pt_f = -1; u->pt_i = 0xFFFFFFFFu;
    u->sel_point = 0xFFFFFFFFu;
    clamp_selection(u);
    say(u, 0, "%s deleted", name[0] ? name : "figure");
    return 1;
}

int dai_show_ui_take_browse(dai_show_ui *u) {
    if (!u) return 0;
    int b = u->want_browse;
    u->want_browse = 0;
    return b;
}

void dai_show_ui_set_image_path(dai_show_ui *u, const char *path) {
    if (!u || !path || !path[0]) return;
    std::snprintf(u->image_path, sizeof(u->image_path), "%s", path);
    std::snprintf(u->img_edit_path, sizeof(u->img_edit_path), "%s", path);
}

void dai_show_ui_note(dai_show_ui *u, int bad, const char *text) {
    if (!u || !text) return;
    say(u, bad, "%s", text);
}

uint32_t dai_show_ui_verdict(const dai_show_ui *u, char *buf, size_t cap) {
    if (!buf || !cap) return 0;
    buf[0] = 0;
    if (!u) return 0;
    verdict_text(u, buf, cap);
    return (uint32_t)std::strlen(buf);
}

// ---- parameters -------------------------------------------------------------

void dai_show_ui_parameters(dai_show_ui *u, dai_ui *ui, float x, float y, float w, float h) {
    if (!u || !ui) return;
    dai_ui_panel_begin(ui, x, y, w, h, nullptr);
    // TWO scrolled areas, not one, and both of them end at the bottom edge of
    // the PANEL rather than h pixels below where the cursor happened to stand.
    //
    // One region was the bug: the settings above it are twenty rows tall, so
    // in a docked column at 1100x700 the solve report began below the fold and
    // the only way to it was a wheel over a panel that gave no sign it had
    // anything left. Worse, the region was started at the layout cursor and
    // given the WHOLE panel height, so it reached past the panel's own edge -
    // the last row was clipped by the panel instead of by the region, and the
    // scroll bar the region draws at its bottom right was outside the panel and
    // never appeared. The five numbers a director solves a show to read
    // (sample, assign, layer, validate, plan) now live in a block of their own
    // that keeps its place at the bottom of the panel whatever the settings
    // above it do, with its own bar.
    float view_x = 0.0f, view_y = 0.0f;
    dai_ui_cursor_pos(ui, &view_x, &view_y);
    const float pitch  = row_pitch(ui);
    const float avail  = (y + h) - view_y - 8.0f;
    dai_show_timings t = dai_show_get_timings(u->sh);
    // The report keeps the button, its heading and the five timing rows - and
    // never more than half the panel, because settings that cannot be reached
    // are as bad as a report that cannot.
    float report_h = 0.0f;
    if (avail > pitch * 6.0f) {
        report_h = std::min(avail * 0.5f, pitch * 8.0f + 10.0f);
        if (report_h < pitch * 3.0f) report_h = pitch * 3.0f;
    }
    const float settings_h = avail - report_h;
    // The lower edge of the settings region, in the coordinates the layout
    // cursor uses. Every row below is measured against it and is drawn only if
    // the WHOLE of it fits: a number box sliced lengthwise by the edge of its
    // own region is the defect this panel was rebuilt for, and it is worse on a
    // field than on a label - half a value box still looks editable. A row that
    // does not fit keeps its place in the layout, so the wheel brings it in
    // whole and the region's scroll extent stays right.
    const float settings_bottom = view_y + settings_h;
    dai_ui_scroll_begin(ui, "showparams", settings_h);
    auto num_row = [&](const char *label, float *v, float step, float lo, float hi,
                       const char *id) -> int {
        float rh = widget_row(ui);
        if (!fits(ui, settings_bottom, rh)) {
            dai_ui_advance(ui, dai_ui_panel_width(ui), rh);
            return 0;
        }
        return dai_ui_num_field(ui, label, v, step, lo, hi, id);
    };
    auto section_row = [&](const char *title) {
        float rh = dai_ui_text_height(ui) + 12.0f;
        if (!fits(ui, settings_bottom, rh)) { dai_ui_advance(ui, 0.0f, rh); return; }
        dai_ui_section(ui, title);
    };
    auto choice_row = [&](const char *label, int *value, const char *const *items,
                          int count, int as_strip) {
        float rh = widget_row(ui);
        if (!fits(ui, settings_bottom, rh)) {
            dai_ui_advance(ui, dai_ui_panel_width(ui), rh);
            return;
        }
        if (as_strip) seg_row(ui, label, value, items, count);
        else          dai_ui_option(ui, label, value, items, count);
    };
    // One column for the whole panel rather than one per section: a label
    // column that changes width halfway down reads as two panels stacked.
    const char *const LABELS[] = { "Drones", "Min dist (m)", "v max (m/s)", "a max (m/s2)",
                                   "Export fps", "Sampling", "Assignment", "Fence X (m)",
                                   "Fence Z (m)", "Ceiling (m)", "Ground (m)", "Take-off (m)" };
    float label_was = fit_labels(ui, LABELS, 12);

    section_row("Fleet");
    float count = (float)u->s.drone_count;
    int changed = 0;
    changed |= num_row("Drones", &count, 1.0f, 1.0f, 100000.0f, "showcount");
    u->s.drone_count = (uint32_t)(count + 0.5f);
    changed |= num_row("Min dist (m)", &u->s.min_distance_m, 0.1f, 0.5f, 100.0f, "showmind");
    changed |= num_row("v max (m/s)", &u->s.v_max_ms, 0.25f, 0.5f, 60.0f, "showvmax");
    changed |= num_row("a max (m/s2)", &u->s.a_max_ms2, 0.25f, 0.25f, 40.0f, "showamax");
    float fps = (float)u->s.fps;
    changed |= num_row("Export fps", &fps, 1.0f, 1.0f, 120.0f, "showfps");
    u->s.fps = (int)(fps + 0.5f);

    section_row("Pipeline");
    choice_row("Sampling", &u->sample_mode, SAMPLE_MODES, 3, 1);
    choice_row("Assignment", &u->assign_method, ASSIGN_METHODS, 4, 0);
    // A hover tooltip, not a row: it takes no space in the layout, so it needs
    // no room made for it and cannot be cut by the edge of the region.
    dai_ui_help(ui, "Auto solves exactly up to 2000 drones and switches to the "
                    "clustered auction above - the storyboard shows what ran.");

    section_row("Safety volume");
    changed |= num_row("Fence X (m)", &u->s.fence_half_x, 1.0f, 0.0f, 5000.0f, "showfx");
    changed |= num_row("Fence Z (m)", &u->s.fence_half_z, 1.0f, 0.0f, 5000.0f, "showfz");
    changed |= num_row("Ceiling (m)", &u->s.fence_top_m, 1.0f, 0.0f, 3000.0f, "showftop");
    changed |= num_row("Ground (m)", &u->s.min_ground_m, 0.5f, 0.0f, 200.0f, "showgnd");
    changed |= num_row("Take-off (m)", &u->s.takeoff_alt_m, 1.0f, 0.0f, 500.0f, "showtoff");

    if (changed) dai_show_set_settings(u->sh, &u->s);

    // Everything above lives in the settings region; the report has its own
    // below, so this one ends here.
    dai_ui_scroll_end(ui);
    dai_ui_style_of(ui)->label_w = label_was;

    // ---- the solve block, always where the reader left it -------------------
    //
    // A separator, the button, and then a region of its own. The button is
    // drawn OUTSIDE any scroll region: the one control this panel exists for
    // may never be a thing you have to go looking for.
    if (report_h <= 0.0f) { dai_ui_panel_end(ui); return; }
    const float rep_y = view_y + settings_h;
    // A panel of its own, ended and begun rather than drawn by hand: that is
    // what puts the layout cursor at the top of the block, gives it its own
    // clip rectangle and lets its scroll region measure against its own edge.
    dai_ui_panel_end(ui);
    dai_ui_panel_begin(ui, x, rep_y, w, (y + h) - rep_y, nullptr);
    dai_ui_rect(ui, x + 6.0f, rep_y, w - 12.0f, 1.0f, dai_ui_style_of(ui)->panel_border);
    if (dai_ui_button(ui, "Solve show")) solve_now(u);
    // The export row, beside Solve rather than under it: the one thing the
    // whole panel is for may never scroll away. A click only RECORDS the wish -
    // the host writes the file, because the host is the one that knows the
    // project's folder. Disabled in spirit when there is no plan: an unsolved
    // show exports nothing but an empty archive.
    {
        const dai_show_plan *plan = dai_show_get_plan(u->sh);
        dai_ui_style *mst = dai_ui_style_of(ui);
        float row_y = 0.0f, row_x = 0.0f;
        dai_ui_cursor_pos(ui, &row_x, &row_y);
        const float bw = (dai_ui_panel_width(ui) - mst->padding * 2.0f - mst->spacing * 3.0f) / 4.0f;
        const float bh = widget_row(ui);
        struct Btn { const char *label; int fmt; };
        const Btn B[4] = { { "SKYC", 1 }, { "DSX", 2 }, { "CSV", 3 }, { "JSON", 4 } };
        for (int i = 0; i < 4; ++i) {
            float bx = row_x + i * (bw + mst->spacing);
            float mx2 = 0.0f, my2 = 0.0f; int dn2 = 0, pr2 = 0;
            dai_ui_mouse(ui, &mx2, &my2, &dn2, &pr2);
            int over = (mx2 >= bx && mx2 < bx + bw && my2 >= row_y && my2 < row_y + bh);
            dai_ui_rect(ui, bx, row_y, bw, bh,
                        over ? (plan ? mst->button_hover : mst->button) : mst->button);
            dai_ui_text(ui, bx + (bw - dai_ui_text_width(ui, B[i].label)) * 0.5f,
                        row_y + (bh - dai_ui_text_height(ui)) * 0.5f, B[i].label,
                        plan ? mst->text : mst->text_dim);
            if (over && pr2 && plan) u->want_export = B[i].fmt;
        }
        dai_ui_advance(ui, dai_ui_panel_width(ui), bh);
    }
    float bx2 = 0.0f, by2 = 0.0f;
    dai_ui_cursor_pos(ui, &bx2, &by2);
    const float report_view_h = std::max(pitch, (y + h) - by2 - 6.0f);
    const float report_bottom = by2 + report_view_h;
    dai_ui_scroll_begin(ui, "showreport", report_view_h);
    if (t.formations) {
        // Everything under this heading is the WHOLE SHOW: counts summed over
        // every transition, the gap and the detour at their worst case, the
        // least exact method that ran anywhere. The alternative - printing
        // whichever move happened to be solved last - is how "0 lifted, 0
        // delayed" came to stand next to eleven conflicts, which reads as a
        // contradiction and is in fact two different transitions.
        // The FIVE STAGE TIMES first and unbroken - sample, assign, layer,
        // validate, plan. They are what a director reads to know the show was
        // actually solved, and they used to sit interleaved with the detail
        // lines, which put "plan" eight rows down and out of a docked column.
        // Everything that qualifies a number now stands below all five.
        dai_ui_label(ui, "show total");
        stat_line(ui, report_bottom, "sample   %8.1f ms", t.sample_ms);
        stat_line(ui, report_bottom, "assign   %8.1f ms", t.assign_ms);
        stat_line(ui, report_bottom, "layer    %8.1f ms", t.layer_ms);
        stat_line(ui, report_bottom, "validate %8.1f ms", t.validate_ms);
        const dai_show_plan *p = dai_show_get_plan(u->sh);
        if (p) stat_line(ui, report_bottom, "plan     %8.2f MB of keyframes",
                         (double)dai_show_plan_bytes(p) / (1024.0 * 1024.0));

        // The detail, below the five: which assignment ran and what it cost,
        // what the layering had to do, how much the validator looked at. A
        // formation fault is not a crossing the separator lost, but it is a
        // reason the show does not fly - so it is counted here rather than
        // left out, on a line of its own, because this panel is a fifth of the
        // window wide and a number that wraps gets read as part of the line
        // above it.
        stat_line(ui, report_bottom, "  assignment: %s",
                  ASSIGN_METHODS[(t.last_assign.method_used >= 0 &&
                                  t.last_assign.method_used < 4) ? t.last_assign.method_used : 0]);
        if (t.last_assign.gap_percent >= 0.0f)
            stat_line(ui, report_bottom, "  worst move %.2f%% over the exact optimum",
                      (double)t.last_assign.gap_percent);
        stat_line(ui, report_bottom, "  %u crossings: %u lifted, %u delayed, %u left over",
                  t.last_layer.crossings_found, t.last_layer.resolved_by_height,
                  t.last_layer.resolved_by_delay, t.last_layer.unresolved);
        stat_line(ui, report_bottom, "  %u formation fault%s", t.last_layer.endpoint_pairs,
                  plural(t.last_layer.endpoint_pairs));
        stat_line(ui, report_bottom, "  %u pairs over %u ticks",
                  t.last_validate.pairs_tested, t.last_validate.ticks_checked);

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
                      plural(sl.endpoint_pairs));
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
            "%u conflict%s over %u ticks", t.last_validate.conflicts,
            plural(t.last_validate.conflicts), t.last_validate.ticks_checked);
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
    std::snprintf(head, sizeof(head), "%u conflict%s%s - click one to jump to it",
                  total, plural(total), (shown < total) ? " (first 8192 listed)" : "");
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
    // The strip is a dope sheet now, not a slider: it needs a ruler and a row
    // per step. Sized from the viewport rather than fixed, so a short window
    // does not end up all timeline and no sky.
    const float TL = std::min(124.0f, std::max(58.0f, h * 0.30f));
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

    // The selected figure's OUTLINE. Ten thousand selected dots are still ten
    // thousand dots; what a director means by "the figure" is its silhouette.
    // So the hull of its points, in screen space, as one closed line - the
    // monotonic chain, O(n log n), on a decimated set when the figure is huge:
    // past a few thousand points the hull stops changing and only the cost
    // grows.
    {
        uint32_t fc_o = dai_show_formation_count(u->sh);
        if (fc_o && u->sel_formation >= 0 && (uint32_t)u->sel_formation < fc_o) {
            const dai_show_point *op = dai_show_formation_points(u->sh, (uint32_t)u->sel_formation);
            dai_show_formation_info oi;
            if (op && dai_show_formation_get(u->sh, (uint32_t)u->sel_formation, &oi) &&
                oi.point_count >= 3) {
                std::vector<std::pair<float, float>> sp;
                uint32_t ostride = 1;
                while (oi.point_count / ostride > 3000u) ++ostride;
                sp.reserve(oi.point_count / ostride + 1);
                // Around where the figure IS, not where it rests: during a
                // transition an outline drawn from the resting pose sits in
                // an empty part of the sky.
                std::vector<uint32_t> omap;
                map_points_to_drones(u, (uint32_t)u->sel_formation, oi.point_count, omap);
                const uint32_t ofn = (uint32_t)u->fleet.size();
                for (uint32_t k = 0; k < oi.point_count; k += ostride) {
                    float hx, hy;
                    const dai_show_point &q = (k < omap.size() && omap[k] < ofn)
                                                  ? u->fleet[omap[k]] : op[k];
                    if (project(cam, q.x, q.y, q.z, &hx, &hy, nullptr))
                        sp.push_back(std::make_pair(hx, hy));
                }
                if (sp.size() >= 3) {
                    std::sort(sp.begin(), sp.end());
                    std::vector<std::pair<float, float>> H(sp.size() * 2);
                    int hn = 0;
                    for (int pass = 0; pass < 2; ++pass) {
                        int base = hn;
                        for (size_t k = 0; k < sp.size(); ++k) {
                            const std::pair<float, float> &pt =
                                pass ? sp[sp.size() - 1 - k] : sp[k];
                            while (hn - base >= 2) {
                                float cr = (H[hn - 1].first - H[hn - 2].first) *
                                               (pt.second - H[hn - 2].second) -
                                           (H[hn - 1].second - H[hn - 2].second) *
                                               (pt.first - H[hn - 2].first);
                                if (cr <= 0.0f) --hn; else break;
                            }
                            H[hn++] = pt;
                        }
                        --hn;
                    }
                    const uint32_t OC = rgba(0xF5, 0xD7, 0x60, 220);
                    for (int k = 0; k + 1 < hn; ++k)
                        dai_ui_line(ui, H[k].first, H[k].second,
                                    H[k + 1].first, H[k + 1].second, 1.5f, OC);
                    if (hn > 1)
                        dai_ui_line(ui, H[hn - 1].first, H[hn - 1].second,
                                    H[0].first, H[0].second, 1.5f, OC);
                }
            }
        }
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

    // ---- the gizmo, and picking points ------------------------------------
    //
    // Three axis handles at the selected figure's pivot, drawn at a fixed
    // number of PIXELS rather than a fixed number of metres: a handle that
    // shrinks with perspective is a handle that cannot be grabbed once you back
    // off far enough to see the show, which is exactly when you want to move a
    // figure.
    float mx = 0.0f, my = 0.0f;
    int down = 0, pressed = 0;
    dai_ui_mouse(ui, &mx, &my, &down, &pressed);
    const int inside = (mx >= x && mx < x + w && my >= y && my < y + vh);
    int consumed = 0;                       // the pointer belonged to a tool

    const uint32_t fcount = dai_show_formation_count(u->sh);
    int have_fig = (fcount && u->sel_formation >= 0 && (uint32_t)u->sel_formation < fcount);
    dai_show_formation_info ginfo;
    if (have_fig && !dai_show_formation_get(u->sh, (uint32_t)u->sel_formation, &ginfo))
        have_fig = 0;

    if (have_fig) {
        const uint32_t fi = (uint32_t)u->sel_formation;
        // The pivot travels with the position - that is what "the figure was
        // moved" means - so the handle sits where the figure is now, not where
        // it was sampled.
        const dai_show_transform &cx = ginfo.xf;
        float ox = ginfo.pivot.x + cx.position.x;
        float oy = ginfo.pivot.y + cx.position.y;
        float oz = ginfo.pivot.z + cx.position.z;
        float osx = 0.0f, osy = 0.0f, odepth = 0.0f;
        if (project(cam, ox, oy, oz, &osx, &osy, &odepth)) {
            const float HANDLE_PX = 84.0f;
            const float wlen = HANDLE_PX * odepth / cam.f;
            const uint32_t AXIS_COL[3] = { rgba(0xE5, 0x53, 0x4B, 255),
                                           rgba(0x62, 0xC5, 0x54, 255),
                                           rgba(0x4A, 0x90, 0xE2, 255) };
            float tipx[3], tipy[3];
            int   drawn[3] = { 0, 0, 0 };
            for (int a = 0; a < 3; ++a) {
                float ax = ox + (a == 0 ? wlen : 0.0f);
                float ay = oy + (a == 1 ? wlen : 0.0f);
                float az = oz + (a == 2 ? wlen : 0.0f);
                drawn[a] = project(cam, ax, ay, az, &tipx[a], &tipy[a], nullptr);
                u->giz_drawn[a] = drawn[a];
                u->giz_sx[a] = tipx[a];
                u->giz_sy[a] = tipy[a];
            }
            int hot = -1;
            // While the brush is on, the gizmo does NOT grab. Its handles
            // radiate from the figure's own pivot, which is exactly where the
            // middle of a figure is - so painting near the centre of a shape
            // grabbed an axis and moved the whole figure instead of colouring
            // it. A tool that is switched on owns the pointer.
            if (inside && u->gizmo_axis < 0 && !u->pick_on) {
                float best = 9.0f;              // the grab radius, in pixels
                for (int a = 0; a < 3; ++a) {
                    if (!drawn[a]) continue;
                    float d = dist_to_segment(mx, my, osx, osy, tipx[a], tipy[a]);
                    if (d < best) { best = d; hot = a; }
                }
            }
            for (int a = 0; a < 3; ++a) {
                if (!drawn[a]) continue;
                int lit = (u->gizmo_axis == a) || (hot == a);
                dai_ui_line(ui, osx, osy, tipx[a], tipy[a], lit ? 3.0f : 2.0f, AXIS_COL[a]);
                // An ARROWHEAD, not a knob: a square at the tip is the scale
                // gizmo's silhouette in every editor that has one, and a move
                // handle that looks like a resize handle gets trusted like
                // one - which is to say, not. Two chevrons back from the tip.
                float ux = tipx[a] - osx, uy = tipy[a] - osy;
                float ul = std::sqrt(ux * ux + uy * uy);
                if (ul > 1.0f) {
                    ux /= ul; uy /= ul;
                    const float AL = lit ? 13.0f : 11.0f;   // arrow length, px
                    const float AW = 0.45f;                 // how wide it opens
                    float pxn = -uy * AL * AW, pyn = ux * AL * AW;
                    float bx = tipx[a] - ux * AL, by = tipy[a] - uy * AL;
                    dai_ui_line(ui, tipx[a], tipy[a], bx + pxn, by + pyn,
                                lit ? 3.0f : 2.0f, AXIS_COL[a]);
                    dai_ui_line(ui, tipx[a], tipy[a], bx - pxn, by - pyn,
                                lit ? 3.0f : 2.0f, AXIS_COL[a]);
                }
            }
            ring(ui, osx, osy, 4.0f, 1.5f, rgba(0xF5, 0xF5, 0xF5, 200));

            if (pressed && hot >= 0) {
                push_undo(u, "move figure");
                u->gizmo_axis = hot;
                u->gizmo_mx   = mx;
                u->gizmo_my   = my;
                u->gizmo_from = cx;
                consumed = 1;
            }
            if (!down) u->gizmo_axis = -1;
            if (u->gizmo_axis >= 0) {
                consumed = 1;
                // Metres per pixel ALONG THIS AXIS, measured where the drag
                // started: project the origin and one metre out, and project
                // the pointer's travel onto that screen direction. The whole
                // offset is recomputed from the transform the drag began with
                // rather than accumulated, so a drag that goes out of the
                // window and comes back lands where the pointer says.
                const int a = u->gizmo_axis;
                float bx0 = ginfo.pivot.x + u->gizmo_from.position.x;
                float by0 = ginfo.pivot.y + u->gizmo_from.position.y;
                float bz0 = ginfo.pivot.z + u->gizmo_from.position.z;
                float p0x, p0y, p1x, p1y;
                if (project(cam, bx0, by0, bz0, &p0x, &p0y, nullptr) &&
                    project(cam, bx0 + (a == 0 ? 1.0f : 0.0f),
                                 by0 + (a == 1 ? 1.0f : 0.0f),
                                 bz0 + (a == 2 ? 1.0f : 0.0f), &p1x, &p1y, nullptr)) {
                    float dx = p1x - p0x, dy = p1y - p0y;
                    float len2 = dx * dx + dy * dy;
                    if (len2 > 1e-4f) {
                        float metres = ((mx - u->gizmo_mx) * dx + (my - u->gizmo_my) * dy) / len2;
                        dai_show_transform nx = u->gizmo_from;
                        if (a == 0) nx.position.x = u->gizmo_from.position.x + metres;
                        if (a == 1) nx.position.y = u->gizmo_from.position.y + metres;
                        if (a == 2) nx.position.z = u->gizmo_from.position.z + metres;
                        dai_show_formation_set_transform(u->sh, fi, &nx);
                    }
                }
            }
        }

        // ---- picking and painting points --------------------------------
        //
        // The COLOUR belongs to the point, and always will - which drone flies
        // to which point is the assignment's answer and it moves on every
        // solve. But the CLICK has to be answered where the dot was drawn,
        // and the dot is the drone, at this second. So: hit test against the
        // live fleet position of the drone standing on each point, and hand
        // the point index to the painter. Before this the clickable dots sat
        // in the figure's resting pose - which is where they are at t = 0 and
        // nowhere else, so nothing was clickable during playback.
        std::vector<uint32_t> pt_drone;
        map_points_to_drones(u, fi, ginfo.point_count, pt_drone);
        const dai_show_point *fpos = dai_show_formation_points(u->sh, fi);
        const uint32_t fleet_n = (uint32_t)u->fleet.size();
        // Where point `i` of this figure IS on screen right now.
        auto point_screen = [&](uint32_t i, float *sx, float *sy) -> int {
            if (i < pt_drone.size() && pt_drone[i] < fleet_n) {
                const dai_show_point &lp = u->fleet[pt_drone[i]];
                return project(cam, lp.x, lp.y, lp.z, sx, sy, nullptr);
            }
            if (!fpos) return 0;
            return project(cam, fpos[i].x, fpos[i].y, fpos[i].z, sx, sy, nullptr);
        };
        if (u->pick_on && inside && (pressed || (down && u->paint_on)) && !consumed) {
            const dai_show_point *fp = fpos;
            if (fp && ginfo.point_count) {
                std::vector<uint32_t> hit;
                float best = u->brush_px;
                uint32_t best_i = 0xFFFFFFFFu;
                for (uint32_t i = 0; i < ginfo.point_count; ++i) {
                    float px2, py2;
                    if (!point_screen(i, &px2, &py2)) continue;
                    float d = std::sqrt((px2 - mx) * (px2 - mx) + (py2 - my) * (py2 - my));
                    if (u->paint_on && d <= u->brush_px) hit.push_back(i);
                    if (d < best) { best = d; best_i = i; }
                }
                if (u->paint_on && !hit.empty()) {
                    if (pressed) push_undo(u, "paint");
                    // Painted AT the playhead: a stroke, so the colour begins
                    // at the second on the timeline the user is watching and
                    // the show keeps whatever came before it.
                    dai_show_formation_paint_at(
                        u->sh, fi, u->time, hit.data(), (uint32_t)hit.size(),
                        (uint8_t)(u->paint_rgb[0] * 255.0f + 0.5f),
                        (uint8_t)(u->paint_rgb[1] * 255.0f + 0.5f),
                        (uint8_t)(u->paint_rgb[2] * 255.0f + 0.5f), 0);
                    consumed = 1;
                } else if (best_i != 0xFFFFFFFFu && pressed) {
                    u->sel_point = best_i;
                    consumed = 1;
                }
            }
        }
        // The picked point, ringed where the drone on it stands NOW - a ring
        // left behind at the resting pose is a ring pointing at nothing.
        if (u->pick_on && u->sel_point < ginfo.point_count) {
            float px2, py2;
            if (point_screen(u->sel_point, &px2, &py2))
                ring(ui, px2, py2, 8.0f, 2.0f, COL_WARN);
        }
        if (u->pick_on && u->paint_on && inside)
            ring(ui, mx, my, u->brush_px, 1.0f, rgba(0xF5, 0xF5, 0xF5, 110));
    }

    // ---- the handle on one point -------------------------------------------
    //
    // A single drone's point is moved the way a figure is moved: SELECT it
    // with a click, then drag one axis handle. Dragging the dot itself is what
    // this used to do, and a click and a drag began identically - so every
    // click that wandered four pixels moved a point, and every point that was
    // moved had to be aimed in a perspective view with no axis to slide along.
    // The handles are half the figure gizmo's length, so the two are never
    // mistaken for one another when both are on screen.
    if (u->pt_f >= 0 && (uint32_t)u->pt_f < dai_show_formation_count(u->sh)) {
        dai_show_formation_info pinf;
        const dai_show_point *pw = dai_show_formation_points(u->sh, (uint32_t)u->pt_f);
        if (pw && dai_show_formation_get(u->sh, (uint32_t)u->pt_f, &pinf) &&
            u->pt_i < pinf.point_count) {
            const dai_show_point &sp = pw[u->pt_i];
            float psx = 0.0f, psy = 0.0f, pdep = 0.0f;
            if (project(cam, sp.x, sp.y, sp.z, &psx, &psy, &pdep)) {
                const float PHANDLE_PX = 46.0f;
                const float plen = PHANDLE_PX * pdep / cam.f;
                const uint32_t PAXIS[3] = { rgba(0xE5, 0x53, 0x4B, 255),
                                            rgba(0x62, 0xC5, 0x54, 255),
                                            rgba(0x4A, 0x90, 0xE2, 255) };
                float ptx[3], pty[3];
                int   pdrawn[3] = { 0, 0, 0 };
                for (int a2 = 0; a2 < 3; ++a2) {
                    pdrawn[a2] = project(cam,
                                         sp.x + (a2 == 0 ? plen : 0.0f),
                                         sp.y + (a2 == 1 ? plen : 0.0f),
                                         sp.z + (a2 == 2 ? plen : 0.0f),
                                         &ptx[a2], &pty[a2], nullptr);
                    u->pt_gdrawn[a2] = pdrawn[a2];
                    u->pt_gsx[a2] = ptx[a2];
                    u->pt_gsy[a2] = pty[a2];
                }
                int phot = -1;
                if (inside && u->pt_axis < 0 && u->gizmo_axis < 0 && !u->pick_on) {
                    float best = 8.0f;
                    for (int a2 = 0; a2 < 3; ++a2) {
                        if (!pdrawn[a2]) continue;
                        float d = dist_to_segment(mx, my, psx, psy, ptx[a2], pty[a2]);
                        if (d < best) { best = d; phot = a2; }
                    }
                }
                ring(ui, psx, psy, 6.0f, 2.0f, COL_WARN);
                for (int a2 = 0; a2 < 3; ++a2) {
                    if (!pdrawn[a2]) continue;
                    int lit = (u->pt_axis == a2) || (phot == a2);
                    dai_ui_line(ui, psx, psy, ptx[a2], pty[a2], lit ? 3.0f : 2.0f, PAXIS[a2]);
                }
                if (pressed && phot >= 0) {
                    push_undo(u, "move point");
                    u->pt_axis = phot;
                    u->pt_mx = mx; u->pt_my = my;
                    u->pt_from[0] = sp.x; u->pt_from[1] = sp.y; u->pt_from[2] = sp.z;
                    consumed = 1;
                }
                if (!down) u->pt_axis = -1;
                if (u->pt_axis >= 0) {
                    consumed = 1;
                    const int a2 = u->pt_axis;
                    float q0x, q0y, q1x, q1y;
                    if (project(cam, u->pt_from[0], u->pt_from[1], u->pt_from[2],
                                &q0x, &q0y, nullptr) &&
                        project(cam, u->pt_from[0] + (a2 == 0 ? 1.0f : 0.0f),
                                     u->pt_from[1] + (a2 == 1 ? 1.0f : 0.0f),
                                     u->pt_from[2] + (a2 == 2 ? 1.0f : 0.0f),
                                &q1x, &q1y, nullptr)) {
                        float ddx = q1x - q0x, ddy = q1y - q0y;
                        float l2 = ddx * ddx + ddy * ddy;
                        if (l2 > 1e-4f) {
                            float metres = ((mx - u->pt_mx) * ddx +
                                            (my - u->pt_my) * ddy) / l2;
                            float np[3] = { u->pt_from[0], u->pt_from[1], u->pt_from[2] };
                            np[a2] += metres;
                            dai_show_formation_set_point_world(u->sh, (uint32_t)u->pt_f,
                                                               u->pt_i, np[0], np[1], np[2]);
                        }
                    }
                }
            }
        }
    } else {
        u->pt_gdrawn[0] = u->pt_gdrawn[1] = u->pt_gdrawn[2] = 0;
    }
    dai_ui_clip_end(ui);

    // ---- selecting one drone -----------------------------------------------
    //
    // A left click SELECTS, and that is all it does. Nothing is dragged out
    // from under the pointer, so a click that wanders a few pixels is still a
    // click - which is what everybody expects of a left button in a 3D view,
    // and what this did not do: the same gesture used to drag the point the
    // drone was standing on, so selecting a drone and deforming the figure
    // were one motion.
    //
    // What gets selected is the drone AND, when that drone is standing still
    // on a point, that point - which is what the handle above then moves.
    // Clicking empty sky clears both, the way clicking empty space does in
    // every hierarchy in this editor.
    if (!u->pick_on && pressed && inside && !consumed) {
        float bestd = 9.0f; uint32_t bdi = 0xFFFFFFFFu;
        for (uint32_t k = 0; k < n; ++k) {
            float hx, hy;
            if (!project(cam, u->fleet[k].x, u->fleet[k].y, u->fleet[k].z, &hx, &hy, nullptr))
                continue;
            float ddx = hx - mx, ddy = hy - my;
            float d = std::sqrt(ddx * ddx + ddy * ddy);
            if (d < bestd) { bestd = d; bdi = k; }
        }
        if (bdi == 0xFFFFFFFFu) {
            u->sel_drone   = 0xFFFFFFFFu;
            u->sel_drone_b = 0xFFFFFFFFu;
            u->pt_f = -1; u->pt_i = 0xFFFFFFFFu;
        } else {
            u->sel_drone   = bdi;
            u->sel_drone_b = 0xFFFFFFFFu;
            uint32_t f2 = 0, pt = 0;
            int settled = 0;
            u->pt_f = -1; u->pt_i = 0xFFFFFFFFu;
            if (dai_show_drone_point_at(u->sh, bdi, u->time, &f2, &pt, &settled)) {
                // Only where the drone STANDS: mid-flight the point it is
                // heading for is somewhere else entirely, and a handle drawn
                // there points at a place the click did not mean.
                if (settled) { u->pt_f = (int)f2; u->pt_i = pt; }
            } else if (dai_show_formation_count(u->sh)) {
                // No plan yet: the preview IS the selected figure, so the
                // fleet index is its point index.
                dai_show_formation_info inf;
                if (u->sel_formation >= 0 &&
                    dai_show_formation_get(u->sh, (uint32_t)u->sel_formation, &inf) &&
                    bdi < inf.point_count) {
                    u->pt_f = u->sel_formation; u->pt_i = bdi;
                }
            }
            consumed = 1;
        }
    }

    // The 2D toggle: top-down, like the planning sheet a show is drawn on
    // first. The 3D angles are kept and restored, not recomputed - a view
    // you return to should be the view you left. Handled BEFORE the orbit:
    // a press on the button is never also the start of a camera move.
    {
        const dai_ui_style *vst = dai_ui_style_of(ui);
        const char *vl = u->view_2d ? "3D" : "2D";
        float vx = x + w - 50.0f, vy = y + 6.0f, vw2 = 44.0f, vh2 = 20.0f;
        int over = (mx >= vx && mx < vx + vw2 && my >= vy && my < vy + vh2);
        dai_ui_rect(ui, vx, vy, vw2, vh2, over ? vst->button_hover : vst->button);
        dai_ui_rect_outline(ui, vx, vy, vw2, vh2, 1.0f, vst->panel_border);
        dai_ui_text(ui, vx + (vw2 - dai_ui_text_width(ui, vl)) * 0.5f,
                    vy + (vh2 - dai_ui_text_height(ui)) * 0.5f, vl, vst->text);
        if (over && pressed) {
            if (!u->view_2d) {
                u->pre2d_yaw = u->yaw; u->pre2d_pitch = u->pitch;
                u->yaw = 0.0f; u->pitch = 1.55f;    // straight down, a degree
                                                    // off the degenerate pole
                u->view_2d = 1;
            } else {
                u->yaw = u->pre2d_yaw; u->pitch = u->pre2d_pitch;
                u->view_2d = 0;
            }
            consumed = 1;
        }
    }

    // Orbit and zoom, on the SCENE VIEW's gestures - which is what "like Unity"
    // means, and what this did not do:
    //
    //   Alt + left drag   orbit                    (2D: slide the map)
    //   middle drag       pan                      (through dai_show_ui_nav)
    //   right drag        look, W A S D to fly     (same)
    //   Alt + right drag  zoom
    //   wheel             zoom
    //   F                 frame it all
    //
    // A PLAIN left drag is NOT a camera gesture. It belongs to the selection,
    // and taking it for an orbit is why clicking a drone and turning the world
    // used to be the same motion: you could not click anything without
    // spinning the sky.
    if (pressed && inside && !consumed && u->key_alt) {
        u->dragging = 1; u->drag_x = mx; u->drag_y = my;
    }
    if (!u->key_alt) u->dragging = 0;
    if (!down || consumed) u->dragging = 0;
    if (u->dragging) {
        if (u->view_2d) {
            // Top-down, a drag SLIDES the map: the world point under the
            // press stays under the pointer. No orbit - there is nothing to
            // orbit around in a plan view, only something to lose.
            float wax, way, waz, wbx, wby, wbz;
            unproject(cam, u->drag_x, u->drag_y, u->dist, &wax, &way, &waz);
            unproject(cam, mx, my, u->dist, &wbx, &wby, &wbz);
            u->focus_x += wax - wbx;
            u->focus_z += waz - wbz;
            u->drag_x = mx; u->drag_y = my;
        } else {
            u->yaw   += (mx - u->drag_x) * 0.008f;
            u->pitch += (my - u->drag_y) * 0.006f;
            u->pitch  = std::max(-1.4f, std::min(1.4f, u->pitch));
            u->drag_x = mx; u->drag_y = my;
        }
    }
    if (inside) {
        float wheel = dai_ui_wheel(ui);
        if (wheel != 0.0f) u->dist = std::max(10.0f, u->dist * (1.0f - 0.1f * wheel));
    }

    // ---- the timeline: a dope sheet, not a slider --------------------------
    //
    // Blender's, in the one way that matters: the two numbers a show is made
    // of - how long a figure holds, how long the flight into it takes - are
    // DRAGGED where they are seen instead of typed into a panel in seconds.
    // One row per step, because steps run at the same time as each other; a
    // bar per figure, a diamond at each end of it.
    //
    //   left diamond    the transit INTO this figure
    //   right diamond   how long it holds
    //   the bar         click to select the figure and seek to it
    //   the ruler       drag to scrub
    //
    // The times are computed HERE from holds and transitions rather than read
    // from the last solve, so the sheet is right on a show that has never been
    // solved - which is every show for the first few minutes of its life.
    const dai_ui_style *st = dai_ui_style_of(ui);
    float ty = y + vh;
    dai_ui_rect(ui, x, ty, w, TL, st->chrome);
    dai_ui_rect(ui, x, ty, w, 1.0f, st->panel_border);

    struct Bar { uint32_t fi; int group; float t0, t1; int first; float transit; float hold; };
    std::vector<Bar> bars;
    std::vector<int> groups;                 // step ids, in the order first seen
    float dur_calc = 0.0f;
    {
        uint32_t fn2 = dai_show_formation_count(u->sh);
        std::vector<float> gclock;           // parallel to `groups`
        for (uint32_t i = 0; i < fn2; ++i) {
            dai_show_formation_info info;
            if (!dai_show_formation_get(u->sh, i, &info)) continue;
            size_t gi = groups.size();
            for (size_t k = 0; k < groups.size(); ++k)
                if (groups[k] == info.group) { gi = k; break; }
            if (gi == groups.size()) { groups.push_back(info.group); gclock.push_back(0.0f); }
            Bar b;
            b.fi = i; b.group = info.group;
            // "First in its step" is about the STEP, not about the list: a
            // figure is first when nothing before it shares its group.
            b.first = 1;
            for (size_t k = 0; k < bars.size(); ++k)
                if (bars[k].group == info.group) { b.first = 0; break; }
            dai_show_transition tr;
            b.transit = 0.0f;
            if (!b.first && dai_show_transition_get(u->sh, i, &tr)) b.transit = tr.duration_s;
            b.hold = info.hold_s;
            b.t0 = gclock[gi] + b.transit;
            b.t1 = b.t0 + b.hold;
            gclock[gi] = b.t1;
            if (b.t1 > dur_calc) dur_calc = b.t1;
            bars.push_back(b);
        }
    }
    float dur = show_duration(u);
    if (dur_calc > dur) dur = dur_calc;
    if (dur < 1.0f) dur = 1.0f;

    // The left gutter: Play, and the clock under it.
    float bw = 54.0f, bh = 22.0f;
    float bx = x + 6.0f, by = ty + 5.0f;
    int over_btn = (mx >= bx && mx < bx + bw && my >= by && my < by + bh);
    dai_ui_rect(ui, bx, by, bw, bh, over_btn ? st->button_hover : st->button);
    const char *label = u->playing ? "Pause" : "Play";
    dai_ui_text(ui, bx + (bw - dai_ui_text_width(ui, label)) * 0.5f,
                by + (bh - dai_ui_text_height(ui)) * 0.5f, label, st->text);
    if (over_btn && pressed) u->playing = !u->playing;

    char tstr[64];
    std::snprintf(tstr, sizeof(tstr), "%.2f / %.2f s", (double)u->time, (double)dur);
    if (by + bh + 4.0f + dai_ui_text_height(ui) < ty + TL)
        dai_ui_text(ui, bx, by + bh + 4.0f, tstr, st->text_dim);

    const float TX0 = bx + bw + 12.0f;
    const float TX1 = x + w - 10.0f;
    const float PPS = (TX1 > TX0 + 20.0f) ? (TX1 - TX0) / dur : 0.0f;   // px per second

    if (PPS > 0.0f) {
        const float RULER_H = 16.0f;
        const float ruler_y = ty + 3.0f;
        // Ticks at a spacing that stays legible: the first of 1, 2, 5, 10, 30,
        // 60, 300 seconds that leaves at least 60 px between two labels.
        const float CAND[7] = { 1.0f, 2.0f, 5.0f, 10.0f, 30.0f, 60.0f, 300.0f };
        float tick = CAND[6];
        for (int k = 0; k < 7; ++k) if (CAND[k] * PPS >= 60.0f) { tick = CAND[k]; break; }
        for (float t = 0.0f; t <= dur + 0.001f; t += tick) {
            float px2 = TX0 + t * PPS;
            dai_ui_rect(ui, px2, ruler_y + RULER_H - 5.0f, 1.0f, 5.0f, st->panel_border);
            char lab[24];
            std::snprintf(lab, sizeof(lab), "%g", (double)t);
            dai_ui_text(ui, px2 + 2.0f, ruler_y, lab, st->text_dim);
        }

        const float rows_y = ruler_y + RULER_H + 2.0f;
        // Sixteen steps are allowed, so the rows SHRINK rather than spill out
        // of the strip and over the sky above it.
        float room = (ty + TL) - rows_y - 3.0f;
        float row_h = 18.0f;
        if (!groups.empty() && (float)groups.size() * row_h > room)
            row_h = room / (float)groups.size();
        if (row_h < 6.0f) row_h = 6.0f;

        const uint32_t BAR_COL = rgba(0x3D, 0x6E, 0xA8, 235);
        const uint32_t BAR_SEL = rgba(0xF5, 0xD7, 0x60, 245);
        const uint32_t TRN_COL = rgba(0x4A, 0x52, 0x64, 220);
        const float    GRAB    = 5.0f;         // half-width of a diamond, px

        // What the pointer is on, decided BEFORE anything is drawn hot, so a
        // diamond and the bar under it cannot both claim the same press.
        int hover_f = -1, hover_end = 0;
        if (u->tl_drag_f < 0) {
            for (size_t k = 0; k < bars.size(); ++k) {
                float ry = rows_y;
                for (size_t g = 0; g < groups.size(); ++g)
                    if (groups[g] == bars[k].group) { ry = rows_y + (float)g * row_h; break; }
                if (my < ry - 2.0f || my > ry + row_h + 2.0f) continue;
                float x0 = TX0 + bars[k].t0 * PPS, x1 = TX0 + bars[k].t1 * PPS;
                if (std::fabs(mx - x1) <= GRAB + 2.0f) { hover_f = (int)k; hover_end = 1; break; }
                if (!bars[k].first && std::fabs(mx - x0) <= GRAB + 2.0f) {
                    hover_f = (int)k; hover_end = 0; break;
                }
            }
        }

        for (size_t k = 0; k < bars.size(); ++k) {
            const Bar &b = bars[k];
            float ry = rows_y;
            for (size_t g = 0; g < groups.size(); ++g)
                if (groups[g] == b.group) { ry = rows_y + (float)g * row_h; break; }
            float x0 = TX0 + b.t0 * PPS, x1 = TX0 + b.t1 * PPS;
            // The flight in, as a thinner bar - a show is mostly transitions,
            // and a sheet that draws only the holds hides where the time goes.
            if (!b.first) {
                float xt = TX0 + (b.t0 - b.transit) * PPS;
                dai_ui_rect(ui, xt, ry + row_h * 0.35f, std::max(1.0f, x0 - xt),
                            std::max(1.0f, row_h * 0.3f), TRN_COL);
            }
            int sel = ((int)b.fi == u->sel_formation);
            dai_ui_rect(ui, x0, ry + 2.0f, std::max(2.0f, x1 - x0),
                        std::max(2.0f, row_h - 4.0f), sel ? BAR_SEL : BAR_COL);
            dai_show_formation_info ni;
            if (x1 - x0 > 26.0f && dai_ui_text_height(ui) < row_h - 2.0f &&
                dai_show_formation_get(u->sh, b.fi, &ni)) {
                char nm[64];
                std::snprintf(nm, sizeof(nm), "%s", ni.name);
                ellide(ui, nm, x1 - x0 - 6.0f);
                dai_ui_text(ui, x0 + 3.0f, ry + (row_h - dai_ui_text_height(ui)) * 0.5f,
                            nm, sel ? rgba(0x18, 0x18, 0x18, 255) : st->text);
            }
            // The two handles, as diamonds: the keyframe shape every timeline
            // in the business uses, so nobody has to be told it can be dragged.
            float cy2 = ry + row_h * 0.5f;
            for (int e = 0; e < 2; ++e) {
                if (e == 0 && b.first) continue;      // nothing flies into it
                float hx = (e == 0) ? x0 : x1;
                int lit = (hover_f == (int)k && hover_end == e) ||
                          (u->tl_drag_f == (int)b.fi && u->tl_drag_end == e);
                uint32_t hc = lit ? rgba(0xFF, 0xFF, 0xFF, 255)
                                  : rgba(0xD8, 0xDE, 0xE9, 235);
                float th = lit ? 2.0f : 1.5f;
                dai_ui_line(ui, hx, cy2 - GRAB, hx + GRAB, cy2, th, hc);
                dai_ui_line(ui, hx + GRAB, cy2, hx, cy2 + GRAB, th, hc);
                dai_ui_line(ui, hx, cy2 + GRAB, hx - GRAB, cy2, th, hc);
                dai_ui_line(ui, hx - GRAB, cy2, hx, cy2 - GRAB, th, hc);
            }
        }

        // Conflicts, on the ruler, where they were before.
        for (uint32_t i = 0; i < cn; ++i) {
            dai_show_conflict c;
            if (!dai_show_conflict_at(u->sh, i, &c)) continue;
            dai_ui_rect(ui, TX0 + std::min(c.time_s, dur) * PPS,
                        ruler_y + RULER_H - 6.0f, 2.0f, 6.0f, COL_CONFLICT);
        }

        // The playhead, over everything.
        dai_ui_rect(ui, TX0 + std::min(u->time, dur) * PPS - 1.0f, ty + 2.0f,
                    2.0f, TL - 4.0f, st->text);

        // ---- the gestures --------------------------------------------------
        const int in_strip = (mx >= TX0 - 8.0f && mx <= TX1 + 8.0f &&
                              my >= ty && my < ty + TL);
        if (pressed && in_strip) {
            if (hover_f >= 0) {
                const Bar &b = bars[hover_f];
                push_undo(u, hover_end ? "hold" : "transit");
                u->tl_drag_f   = (int)b.fi;
                u->tl_drag_end = hover_end;
                u->tl_drag_mx  = mx;
                u->tl_drag_v0  = hover_end ? b.hold : b.transit;
                u->sel_formation = (int)b.fi;
                u->playing = 0;
            } else if (my < rows_y) {
                u->tl_scrub = 1;                    // the ruler scrubs
                u->playing  = 0;
            } else {
                // A press on a bar selects that figure and seeks to it; a
                // press on empty track scrubs, which is what the whole strip
                // did before and what a hand reaches for out of habit.
                int on_bar = -1;
                for (size_t k = 0; k < bars.size(); ++k) {
                    float ry = rows_y;
                    for (size_t g = 0; g < groups.size(); ++g)
                        if (groups[g] == bars[k].group) { ry = rows_y + (float)g * row_h; break; }
                    if (my < ry || my > ry + row_h) continue;
                    if (mx >= TX0 + bars[k].t0 * PPS && mx <= TX0 + bars[k].t1 * PPS) {
                        on_bar = (int)k; break;
                    }
                }
                if (on_bar >= 0) {
                    u->sel_formation = (int)bars[on_bar].fi;
                    u->time    = bars[on_bar].t0;
                    u->playing = 0;
                } else {
                    u->tl_scrub = 1;
                    u->playing  = 0;
                }
            }
        }
        if (!down) { u->tl_drag_f = -1; u->tl_scrub = 0; }
        if (u->tl_scrub && down)
            u->time = std::max(0.0f, std::min(dur, (mx - TX0) / PPS));
        if (u->tl_drag_f >= 0 && down) {
            // A quarter of a second is the grid a show is written on, and
            // Shift takes the snap off for the rare case that needs less.
            float secs = u->tl_drag_v0 + (mx - u->tl_drag_mx) / PPS;
            if (!u->key_shift) secs = std::floor(secs * 4.0f + 0.5f) * 0.25f;
            uint32_t fi2 = (uint32_t)u->tl_drag_f;
            if (u->tl_drag_end) {
                if (secs < 0.0f)   secs = 0.0f;
                if (secs > 600.0f) secs = 600.0f;
                dai_show_formation_set_hold(u->sh, fi2, secs);
            } else {
                // Below half a second a transition is a teleport and the plan
                // builder refuses it, so the drag stops there rather than
                // writing a number the solver will throw back.
                if (secs < 0.5f)   secs = 0.5f;
                if (secs > 600.0f) secs = 600.0f;
                dai_show_transition tr;
                if (dai_show_transition_get(u->sh, fi2, &tr)) {
                    tr.duration_s = secs;
                    dai_show_transition_set(u->sh, fi2, &tr);
                }
            }
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
    // The + and the right click open the same menu, because adding a figure
    // is adding a figure no matter which hand did it. The menu floats; the
    // pick is answered below, once, for both ways in.
    float hmx = 0.0f, hmy = 0.0f;
    dai_ui_mouse(ui, &hmx, &hmy, nullptr, nullptr);
    dai_ui_row(ui, 0.0f);
    if (dai_ui_button(ui, "+")) dai_ui_popup_open(&u->add_menu, hmx, hmy);
    dai_ui_row_end(ui);
    if (dai_ui_right_pressed(ui) &&
        hmx >= x && hmx < x + w && hmy >= y && hmy < y + h)
        dai_ui_popup_open(&u->add_menu, hmx, hmy);
    int apick = dai_ui_popup_menu(ui, &u->add_menu, ADD_ITEMS, ADD_ITEM_COUNT);
    // A new step is a second timeline: its own figures, its own drones, all
    // of it at the same time as the others. Adding one is adding a figure
    // with a group nobody has used yet - so the fleet has to be shared out
    // again, which the solve says out loud if the counts stop adding up.
    if (apick == 8) {                       // duplicate the selected figure
        uint32_t fc4 = dai_show_formation_count(u->sh);
        if (u->sel_formation >= 0 && (uint32_t)u->sel_formation < fc4) {
            dai_show_formation_info di;
            const dai_show_point *dp =
                dai_show_formation_local_points(u->sh, (uint32_t)u->sel_formation);
            if (dp && dai_show_formation_get(u->sh, (uint32_t)u->sel_formation, &di)) {
                push_undo(u, "duplicate");
                char dn[80];
                std::snprintf(dn, sizeof(dn), "%s copy", di.name);
                uint32_t ni = dai_show_formation_add(u->sh, dn, di.source, dp,
                                                     di.point_count, di.hold_s);
                if (ni != 0xFFFFFFFFu) {
                    dai_show_formation_set_group(u->sh, ni, di.group);
                    dai_show_formation_set_transform(u->sh, ni, &di.xf);
                    dai_show_formation_set_sample_mode(u->sh, ni, di.sample_mode);
                    u->sel_formation = (int)ni;
                    say(u, 0, "%s added", dn);
                }
            }
        }
        apick = -1;
    }
    if (apick == 9) { dai_show_ui_delete_selected(u); apick = -1; }
    if (apick == 6) {
        int maxg = -1;
        uint32_t fc3 = dai_show_formation_count(u->sh);
        for (uint32_t k = 0; k < fc3; ++k) {
            dai_show_formation_info gi3;
            if (dai_show_formation_get(u->sh, k, &gi3) && gi3.group > maxg) maxg = gi3.group;
        }
        uint32_t was = dai_show_formation_count(u->sh);
        push_undo(u, "new step");
        add_builtin(u);
        if (dai_show_formation_count(u->sh) > was) {
            dai_show_formation_set_group(u->sh, was, maxg + 1);
            u->sel_formation = (int)was;
            say(u, 0, "step %d added - give its figures their share of the fleet",
                maxg + 2);
        }
        apick = -1;
    }
    if (apick >= 1 && apick < 4) {
        push_undo(u, "add figure");
        u->figure = apick - 1;
        add_builtin(u);
        uint32_t nf = dai_show_formation_count(u->sh);
        if (nf) u->sel_formation = (int)nf - 1;   // what you just added is what
                                                 // the inspector should show
    }
    else if (apick == 4)         { dai_ui_popup_open(&u->img_menu, hmx, hmy); }
    else if (apick == 5) {
        // Where the drones stand before anybody presses play. Added to the
        // step the selected figure belongs to, at its front.
        int grp = 0;
        dai_show_formation_info gi;
        if (u->sel_formation >= 0 &&
            dai_show_formation_get(u->sh, (uint32_t)u->sel_formation, &gi))
            grp = gi.group;
        char gerr[256] = { 0 };
        push_undo(u, "takeoff grid");
        uint32_t gidx = dai_show_add_takeoff_grid(u->sh, grp, 0.0f, gerr, sizeof(gerr));
        if (gidx == 0xFFFFFFFFu) say(u, 1, "%s", gerr[0] ? gerr : "no takeoff grid");
        else { u->sel_formation = (int)gidx; say(u, 0, "takeoff grid added to step %d", grp + 1); }
    }
    // The image import is a little window of its own, because a menu row is
    // not the place that can hold a path, a width and a threshold.
    if (u->img_menu.open) {
        dai_ui_popup_panel_begin(ui, &u->img_menu, 300.0f, 210.0f);
        // The button FIRST, because typing a path is the fallback and not the
        // way in. Nobody knows the full path of their own picture, and Import
        // did nothing at all while the field was empty - which is how "adding
        // an image does not work" came to be true.
        if (dai_ui_button(ui, "Choose a file...")) u->want_browse = 1;
        dai_ui_input_text(ui, "Image", u->image_path, sizeof(u->image_path));
        dai_ui_num_field(ui, "Width (m)", &u->image_width_m, 1.0f, 2.0f, 2000.0f, "popimgw");
        dai_ui_num_field(ui, "Threshold", &u->image_threshold, 1.0f, 0.0f, 255.0f, "popimgt");
        dai_ui_num_field(ui, "Points (0 = fleet)", &u->image_points, 1.0f, 0.0f,
                         (float)u->s.drone_count, "popimgn");
        if (dai_ui_button(ui, "Import")) {
            if (!u->image_path[0]) {
                // Import with nothing chosen used to write a complaint to the
                // status line at the far edge of the window, where nobody was
                // looking. It opens the file dialog instead.
                u->want_browse = 1;
            } else {
                push_undo(u, "add image");
                uint32_t was_i = dai_show_formation_count(u->sh);
                add_image(u);
                if (dai_show_formation_count(u->sh) > was_i)
                    u->sel_formation = (int)dai_show_formation_count(u->sh) - 1;
                dai_ui_popup_close(&u->img_menu);
            }
        }
        if (!u->image_path[0]) wrapped_label(ui, "no file chosen yet");
        dai_ui_popup_panel_end(ui);
    }
    if (!n) {
        wrapped_label(ui, "no figures yet - the + above adds one");
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
    int prev_group = -1;                     // forces the first header
    dai_ui_scroll_begin(ui, "showfigs", list_h);
    for (uint32_t i = 0; i < n; ++i) {
        dai_show_formation_info info;
        if (!dai_show_formation_get(u->sh, i, &info)) continue;
        // The step is the PARENT, the figure its child: everything sharing a
        // step number flies one after another, a new step starts a parallel
        // timeline with its own drones. Said as a header row, not a manual.
        if (info.group != prev_group) {
            prev_group = info.group;
            char gh[64];
            std::snprintf(gh, sizeof(gh), "Step %d", info.group + 1);
            float hx2 = 0.0f, hy2 = 0.0f;
            dai_ui_cursor_pos(ui, &hx2, &hy2);
            if (hy2 >= top - 0.5f && hy2 + pitch <= bottom + 0.5f)
                dai_ui_text(ui, hx2 + 2.0f,
                            hy2 + (pitch - dai_ui_text_height(ui)) * 0.5f, gh, st->text_dim);
            dai_ui_advance(ui, w - 16.0f, pitch - st->spacing);
        }
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
        char row[224], row_head[160], row_tail[64];
        // Measured against the toggle button this row IS - panel width less
        // its padding and the text inset - rather than against the whole-row
        // estimate the wider panels use. In a tenth of the frame the two
        // differ by twenty pixels, which is the difference between "1. Sphere"
        // and "1. Sp...".
        const float avail = dai_ui_panel_width(ui) - st->padding * 2.0f - 10.0f;
        std::snprintf(row_head, sizeof(row_head), "   %u. %s  %u drones",
                      i + 1u, info.name, info.point_count);
        std::snprintf(row_tail, sizeof(row_tail), "  %.1f s", (double)info.t_start);
        std::snprintf(row, sizeof(row), "%s%s", row_head, row_tail);
        // The drone count is the first thing dropped, whole - it is the same
        // for every figure of a fixed fleet. What follows is cut in the MIDDLE
        // so the second the figure begins in survives the narrow column.
        if (dai_ui_text_width(ui, row) > avail) {
            std::snprintf(row_head, sizeof(row_head), "   %u. %s", i + 1u, info.name);
            ellide_middle(ui, row, sizeof(row), row_head, row_tail, avail, 8);
        }
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
    // A released button ends whatever gesture was coalescing its undo steps.
    { int dnow = 0; dai_ui_mouse(ui, nullptr, nullptr, &dnow, nullptr);
      if (!dnow) u->undo_guard = 0; }
    dai_ui_panel_begin(ui, x, y, w, h, nullptr);
    const dai_show_plan *plan = dai_show_get_plan(u->sh);
    const char *const LABELS[] = { "Position", "Colour", "Keyframes", "Nearest",
                                   "Hold (s)", "Step", "Points", "Mode",
                                   "Transit (s)", "Profile", "Timing", "Spread (s)",
                                   "Assignment" };
    float label_was = fit_labels(ui, LABELS, 13);

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

    // ---- the figure: where it stands, and what colour it burns ------------
    //
    // This is the section that turns a droneshow project from a solver with a
    // preview into an editor. Before it, a figure's place was baked into its
    // points by the sampler and the only way to move one was to sample it
    // again - a different cloud, a different assignment, a different show.
    {
        uint32_t fcount = dai_show_formation_count(u->sh);
        if (fcount) {
            if (u->sel_formation < 0) u->sel_formation = 0;
            if ((uint32_t)u->sel_formation >= fcount) u->sel_formation = (int)fcount - 1;
            const uint32_t fi = (uint32_t)u->sel_formation;
            dai_show_formation_info info;
            if (dai_show_formation_get(u->sh, fi, &info)) {
                char head[128];
                std::snprintf(head, sizeof(head), "%u. %s", fi + 1u, info.name);
                ellide(ui, head, dai_ui_panel_width(ui) - 24.0f);
                dai_ui_section(ui, head);

                // The launch pad is a MEASURED thing: its whole promise is
                // that no two drones on the ground stand closer than the
                // spacing the crew taped out. Scaling it breaks that promise
                // silently - half scale is half the spacing, and a grid built
                // at the minimum becomes a grid inside it. So the pad is
                // offered a RIGID transform only: moving and turning keep
                // every distance in a grid, scaling is the one operation that
                // does not, which is exactly why it is not there. Its size is
                // set by Spacing (m) below, which cannot go under the minimum.
                const int xf_takeoff = std::strcmp(info.source, "builtin://takeoff") == 0;
                dai_show_transform xf = info.xf;
                float pos[3] = { xf.position.x, xf.position.y, xf.position.z };
                float rot[3] = { xf.rotation_deg.x, xf.rotation_deg.y, xf.rotation_deg.z };
                float scl[3] = { xf.scale.x, xf.scale.y, xf.scale.z };
                int moved = 0;
                moved |= vec3_row(ui, bot, "Position", pos, 0.05f);
                moved |= vec3_row(ui, bot, "Rotation", rot, 0.5f);
                if (!xf_takeoff) {
                    moved |= vec3_row(ui, bot, "Scale", scl, 0.01f);
                } else {
                    field_row(ui, "Scale", "fixed - use Spacing (m)", bot);
                    if (scl[0] != 1.0f || scl[1] != 1.0f || scl[2] != 1.0f) {
                        // A show saved before this rule existed may carry one.
                        // Put it back rather than quietly drawing a grid whose
                        // spacing is not the number the panel is showing.
                        scl[0] = scl[1] = scl[2] = 1.0f;
                        moved = 1;
                    }
                }
                if (moved) {
                    edit_begin(u, ui, "transform");
                    dai_show_transform nx;
                    nx.position     = dai_vec3{ pos[0], pos[1], pos[2] };
                    nx.rotation_deg = dai_vec3{ rot[0], rot[1], rot[2] };
                    // A scale of zero is the whole fleet at one coordinate, and
                    // it is what a drag through zero produces on its way to the
                    // other side. Held just off it rather than refused, so the
                    // drag keeps working.
                    nx.scale = dai_vec3{
                        (std::fabs(scl[0]) < 1e-3f) ? 1e-3f : scl[0],
                        (std::fabs(scl[1]) < 1e-3f) ? 1e-3f : scl[1],
                        (std::fabs(scl[2]) < 1e-3f) ? 1e-3f : scl[2] };
                    if (dai_show_formation_set_transform(u->sh, fi, &nx))
                        say(u, 0, "%s moved", info.name);
                }
                if (row_button(ui, bot, "Reset the transform")) {
                    push_undo(u, "reset transform");
                    dai_show_transform id = dai_show_transform_identity();
                    dai_show_formation_set_transform(u->sh, fi, &id);
                    say(u, 0, "%s put back where it was sampled", info.name);
                }
                // The transform is only half of what an edit can do to a
                // figure: points get dragged one at a time, colours get
                // painted, a sample mode gets changed. "Reset the object"
                // takes ALL of it back by building the figure from its source
                // again - the only definition of "as it was" that does not
                // depend on how long ago it was.
                if (row_button(ui, bot, "Reset the object")) {
                    push_undo(u, "reset object");
                    if (reset_object(u, fi))
                        say(u, 0, "%s built again from its source", info.name);
                }

                // THE TRAP THE GIZMO OPENS, said out loud while it is being
                // opened: min_distance is a world distance, so shrinking a
                // figure built at the limit is a collision the sampler would
                // have refused to produce. Measured after the transform, every
                // frame, because that is when the user is dragging.
                float gap = dai_show_formation_min_spacing(u->sh, fi);
                if (gap >= 0.0f) {
                    char sp[96];
                    std::snprintf(sp, sizeof(sp), "%.2f m", (double)gap);
                    field_row(ui, "Closest", sp, bot);
                    if (gap < u->s.min_distance_m) {
                        char warn[160];
                        std::snprintf(warn, sizeof(warn),
                                      "inside the %.2f m minimum - this figure cannot fly",
                                      (double)u->s.min_distance_m);
                        float wx = 0.0f, wy = 0.0f;
                        dai_ui_cursor_pos(ui, &wx, &wy);
                        if (fits(ui, bot, dai_ui_text_height(ui)))
                            dai_ui_text(ui, wx, wy, warn, COL_CONFLICT);
                        dai_ui_advance(ui, w - 16.0f, dai_ui_text_height(ui));
                    }
                }

                // ---- the fields the storyboard used to own ----------------
                // Hold, step and count are properties OF THE FIGURE, so they
                // live where the figure is inspected - a second panel editing
                // the same selected thing is how two panels drift apart.
                {
                    float hold = info.hold_s;
                    float hgt = widget_row(ui);
                    if (fits(ui, bot, hgt)) {
                        if (dai_ui_num_field(ui, "Hold (s)", &hold, 0.25f, 0.0f, 600.0f, "inshold")) {
                            edit_begin(u, ui, "hold");
                            dai_show_formation_set_hold(u->sh, fi, hold);
                        }
                    } else dai_ui_advance(ui, dai_ui_panel_width(ui), hgt);
                }
                {
                    float grp = (float)info.group;
                    float hgt = widget_row(ui);
                    if (fits(ui, bot, hgt)) {
                        if (dai_ui_num_field(ui, "Step", &grp, 1.0f, 1.0f, 16.0f, "insgrp")) {
                            edit_begin(u, ui, "step");
                            dai_show_formation_set_group(u->sh, fi, (int)(grp - 0.5f));
                        }
                    } else dai_ui_advance(ui, dai_ui_panel_width(ui), hgt);
                }
                {
                    float pts_f = (float)info.point_count;
                    float hgt = widget_row(ui);
                    // The launch pad is not a figure you sample - it is a
                    // grid whose size the STEP decides and whose only knob is
                    // how far apart the drones stand. Offering it Surface /
                    // Volume / Silhouette was nonsense the code could not
                    // carry out and the user could not undo.
                    int is_takeoff = std::strcmp(info.source, "builtin://takeoff") == 0;
                    int is_builtin = !is_takeoff &&
                                     std::strncmp(info.source, "builtin://", 10) == 0;
                    int is_image   = std::strncmp(info.source, "image://", 8) == 0;
                    if (is_takeoff) {
                        // How many drones stand on it, and how far apart they
                        // stand. The count used to be fixed at creation, so a
                        // step whose figures were resized afterwards kept a
                        // launch pad of the wrong size with no way to say so.
                        float gap = dai_show_formation_min_spacing(u->sh, fi);
                        if (gap < 0.0f) gap = u->s.min_distance_m * 1.5f;
                        float gcount = (float)info.point_count;
                        int regrid = 0;
                        if (fits(ui, bot, hgt)) {
                            if (dai_ui_num_field(ui, "Drones", &gcount, 1.0f, 1.0f,
                                                 (float)u->s.drone_count, "insgcnt") &&
                                (uint32_t)(gcount + 0.5f) != info.point_count)
                                regrid = 1;
                        } else dai_ui_advance(ui, dai_ui_panel_width(ui), hgt);
                        hgt = widget_row(ui);
                        if (fits(ui, bot, hgt)) {
                            if (dai_ui_num_field(ui, "Spacing (m)", &gap, 0.1f,
                                                 u->s.min_distance_m, 100.0f, "insgap"))
                                regrid = 1;
                        } else dai_ui_advance(ui, dai_ui_panel_width(ui), hgt);
                        if (regrid) {
                            edit_begin(u, ui, "takeoff grid");
                            uint32_t want = (uint32_t)(gcount + 0.5f);
                            if (want < 1u) want = 1u;
                            std::vector<dai_show_point> gp(want);
                            if (dai_show_takeoff_points(u->sh, want, gap, gp.data()))
                                dai_show_formation_replace_points(u->sh, fi, gp.data(), want);
                        }
                        char lay[96];
                        std::snprintf(lay, sizeof(lay), "%u on the ground, %.2f m apart",
                                      info.point_count, (double)gap);
                        field_row(ui, "Grid", lay, bot);
                    } else if (is_builtin || is_image) {
                        if (fits(ui, bot, hgt)) {
                            if (dai_ui_num_field(ui, "Points", &pts_f, 1.0f, 1.0f,
                                                 (float)u->s.drone_count, "inspts") &&
                                (uint32_t)(pts_f + 0.5f) != info.point_count) {
                                edit_begin(u, ui, "point count");
                                if (is_builtin) {
                                    int shape = 0;
                                    const char *bn = info.source + 10;
                                    for (int k = 0; k < 3; ++k)
                                        if (!std::strcmp(bn, FIGURES[k])) shape = k;
                                    resample_builtin(u, fi, shape,
                                                     (uint32_t)(pts_f + 0.5f), info.sample_mode);
                                } else {
                                    resample_image(u, fi, info.source,
                                                   (uint32_t)(pts_f + 0.5f));
                                }
                            }
                        } else dai_ui_advance(ui, dai_ui_panel_width(ui), hgt);
                    } else {
                        char fixed[80];
                        std::snprintf(fixed, sizeof(fixed), "%u points (fixed at creation)",
                                      info.point_count);
                        field_row(ui, "Points", fixed, bot);
                    }
                    // WHICH picture, right here. The import window is for
                    // adding one; changing your mind afterwards is an
                    // inspector's job, and before this the only way to swap
                    // the image was to delete the figure and start again.
                    if (is_image) {
                        // The path and the two numbers ride inside the source
                        // string; they are unpacked into the panel's fields
                        // the first time this figure is selected, so typing
                        // in them does not fight the document every frame.
                        if (u->img_edit_for != (int)fi) {
                            u->img_edit_for = (int)fi;
                            const char *q = info.source + 8;
                            const char *qm = std::strchr(q, '?');
                            size_t plen = qm ? (size_t)(qm - q) : std::strlen(q);
                            if (plen >= sizeof(u->img_edit_path)) plen = sizeof(u->img_edit_path) - 1;
                            std::memcpy(u->img_edit_path, q, plen);
                            u->img_edit_path[plen] = 0;
                            u->img_edit_w  = 60.0f;
                            u->img_edit_th = 128.0f;
                            if (qm) {
                                float ww = 60.0f; int tt = 128;
                                if (std::sscanf(qm + 1, "w=%f&th=%d", &ww, &tt) >= 1) {
                                    u->img_edit_w  = ww;
                                    u->img_edit_th = (float)tt;
                                }
                            }
                        }
                        if (row_button(ui, bot, "Choose a file...")) u->want_browse = 2;
                        float hi1 = widget_row(ui);
                        if (fits(ui, bot, hi1))
                            dai_ui_input_text(ui, "Image", u->img_edit_path,
                                              sizeof(u->img_edit_path));
                        else dai_ui_advance(ui, dai_ui_panel_width(ui), hi1);
                        hi1 = widget_row(ui);
                        if (fits(ui, bot, hi1))
                            dai_ui_num_field(ui, "Width (m)", &u->img_edit_w, 1.0f,
                                             2.0f, 2000.0f, "insimgw");
                        else dai_ui_advance(ui, dai_ui_panel_width(ui), hi1);
                        hi1 = widget_row(ui);
                        if (fits(ui, bot, hi1))
                            dai_ui_num_field(ui, "Threshold", &u->img_edit_th, 1.0f,
                                             0.0f, 255.0f, "insimgt");
                        else dai_ui_advance(ui, dai_ui_panel_width(ui), hi1);
                        if (row_button(ui, bot, "Load this image")) {
                            char nsrc[300];
                            std::snprintf(nsrc, sizeof(nsrc), "image://%s?w=%.1f&th=%d",
                                          u->img_edit_path, (double)u->img_edit_w,
                                          (int)u->img_edit_th);
                            push_undo(u, "image");
                            if (resample_image(u, fi, nsrc, info.point_count)) {
                                // The figure now IS that picture, so its source
                                // has to say so - otherwise the next re-sample
                                // would read the old file again.
                                dai_show_formation_set_source(u->sh, fi, nsrc);
                                const char *base = std::strrchr(u->img_edit_path, '/');
                                if (!base) base = std::strrchr(u->img_edit_path, '\\');
                                dai_show_formation_rename(u->sh, fi,
                                                          base ? base + 1 : u->img_edit_path);
                            }
                        }
                    }
                    // Surface, volume or only the edges - the silhouette is
                    // the one that makes a cube read as a cube instead of a
                    // cloud. Re-samples the shape with the mode kept.
                    if (is_builtin) {
                        int mode = info.sample_mode;
                        float hgt2 = widget_row(ui);
                        if (fits(ui, bot, hgt2)) {
                            if (seg_row(ui, "Mode", &mode, SAMPLE_MODES, 3) &&
                                mode != info.sample_mode) {
                                push_undo(u, "sample mode");
                                int shape = 0;
                                const char *bn = info.source + 10;
                                for (int k = 0; k < 3; ++k)
                                    if (!std::strcmp(bn, FIGURES[k])) shape = k;
                                resample_builtin(u, fi, shape, info.point_count, mode);
                            }
                        } else dai_ui_advance(ui, dai_ui_panel_width(ui), hgt2);
                    }
                }
                // The move INTO this figure is meaningless for the first
                // figure of a step - nothing comes before it.
                int first_in_step = 1;
                for (uint32_t j = 0; j < fi; ++j) {
                    dai_show_formation_info oi;
                    if (dai_show_formation_get(u->sh, j, &oi) && oi.group == info.group) {
                        first_in_step = 0;
                        break;
                    }
                }
                if (!first_in_step) {
                    dai_show_transition tr;
                    if (dai_show_transition_get(u->sh, fi, &tr)) {
                        int changed = 0;
                        float hgt = widget_row(ui);
                        if (fits(ui, bot, hgt))
                            changed |= dai_ui_num_field(ui, "Transit (s)", &tr.duration_s,
                                                        0.25f, 0.5f, 600.0f, "insdur");
                        else dai_ui_advance(ui, dai_ui_panel_width(ui), hgt);
                        hgt = widget_row(ui);
                        if (fits(ui, bot, hgt))
                            changed |= dai_ui_option(ui, "Profile", &tr.profile, PROFILES, 4);
                        else dai_ui_advance(ui, dai_ui_panel_width(ui), hgt);
                        hgt = widget_row(ui);
                        if (fits(ui, bot, hgt))
                            changed |= dai_ui_option(ui, "Timing", &tr.timing, TIMINGS, 2);
                        else dai_ui_advance(ui, dai_ui_panel_width(ui), hgt);
                        if (tr.timing == DAI_SHOW_TIMING_STAGGERED) {
                            hgt = widget_row(ui);
                            if (fits(ui, bot, hgt))
                                changed |= dai_ui_num_field(ui, "Spread (s)", &tr.stagger_s,
                                                            0.1f, 0.0f, 120.0f, "insstag");
                            else dai_ui_advance(ui, dai_ui_panel_width(ui), hgt);
                        }
                        hgt = widget_row(ui);
                        if (fits(ui, bot, hgt))
                            changed |= dai_ui_option(ui, "Assignment", &tr.assign_method,
                                                     ASSIGN_METHODS, 4);
                        else dai_ui_advance(ui, dai_ui_panel_width(ui), hgt);
                        if (changed) {
                            edit_begin(u, ui, "transition");
                            dai_show_transition_set(u->sh, fi, &tr);
                        }
                    }
                }

                // ---- colour, in the three levels the document has ---------
                if (u->colour_for != (int)fi) {
                    u->colour_for = (int)fi;
                    u->figure_on  = info.colour_override;
                    u->figure_rgb[0] = (float)info.colour[0] / 255.0f;
                    u->figure_rgb[1] = (float)info.colour[1] / 255.0f;
                    u->figure_rgb[2] = (float)info.colour[2] / 255.0f;
                    u->figure_w      = info.colour[3];
                    u->sel_point     = 0xFFFFFFFFu;
                }
                int was_on = u->figure_on;
                int col_changed = check_row(ui, bot, "One colour", &u->figure_on);
                col_changed |= colour_row(ui, bot, "Figure", u->figure_rgb, "showfigcol");
                float wch = (float)u->figure_w;
                {
                    float hgt = widget_row(ui);
                    if (fits(ui, bot, hgt)) {
                        if (dai_ui_num_field(ui, "White", &wch, 1.0f, 0.0f, 255.0f, "showfigw")) {
                            float cw = wch + 0.5f;
                            if (!(cw > 0.0f)) cw = 0.0f;
                            if (cw > 255.0f)  cw = 255.0f;
                            u->figure_w = (uint8_t)cw;
                            col_changed = 1;
                        }
                    } else dai_ui_advance(ui, dai_ui_panel_width(ui), hgt);
                }
                if (col_changed || was_on != u->figure_on) {
                    edit_begin(u, ui, "colour");
                    dai_show_formation_set_colour(u->sh, fi, u->figure_on,
                                                  to_u8(u->figure_rgb[0]),
                                                  to_u8(u->figure_rgb[1]),
                                                  to_u8(u->figure_rgb[2]), u->figure_w);
                }

                // ---- painting single points -------------------------------
                int was_pick = u->pick_on;
                check_row(ui, bot, "Pick points in the preview", &u->pick_on);
                if (u->pick_on && !was_pick) {
                    // What is on screen has to BE the figure that is being
                    // painted, or the click lands on a drone in transit and
                    // paints a point somewhere else entirely.
                    u->time    = info.t_start;
                    u->playing = 0;
                }
                if (u->pick_on) {
                    check_row(ui, bot, "Paint while dragging", &u->paint_on);
                    colour_row(ui, bot, "Brush", u->paint_rgb, "showbrush");
                    if (row_button(ui, bot, "Forget all painted colours")) {
                        push_undo(u, "clear paint");
                        dai_show_formation_clear_strokes(u->sh, fi);
                    }
                    float bp = u->brush_px;
                    {
                        float hgt = widget_row(ui);
                        if (fits(ui, bot, hgt)) {
                            if (dai_ui_num_field(ui, "Radius", &bp, 1.0f, 3.0f, 200.0f, "showbrushr"))
                                u->brush_px = bp;
                        } else dai_ui_advance(ui, dai_ui_panel_width(ui), hgt);
                    }
                    const dai_show_point *fp = dai_show_formation_points(u->sh, fi);
                    if (u->sel_point < info.point_count && fp) {
                        char pl[96];
                        std::snprintf(pl, sizeof(pl), "point %u of %u",
                                      u->sel_point, info.point_count);
                        field_row(ui, "Picked", pl, bot);
                        std::snprintf(pl, sizeof(pl), "%u %u %u  w %u",
                                      fp[u->sel_point].r, fp[u->sel_point].g,
                                      fp[u->sel_point].b, fp[u->sel_point].w);
                        field_row(ui, "Colour", pl, bot);
                        if (row_button(ui, bot, "Paint the picked point")) {
                            uint32_t one = u->sel_point;
                            // A stroke AT THE PLAYHEAD: the colour starts at
                            // the second the user is looking at, not from the
                            // first frame of the show.
                            dai_show_formation_paint_at(
                                u->sh, fi, u->time, &one, 1, to_u8(u->paint_rgb[0]),
                                to_u8(u->paint_rgb[1]), to_u8(u->paint_rgb[2]), 0);
                            say(u, 0, "point %u painted at %.1f s", u->sel_point,
                                (double)u->time);
                            say(u, 0, "point %u painted", one);
                        }
                    } else {
                        stat_line(ui, bot, "click a drone in the preview to pick it");
                    }
                    if (row_button(ui, bot, "Paint every point")) {
                        dai_show_formation_set_point_colour(
                            u->sh, fi, nullptr, 0, to_u8(u->paint_rgb[0]),
                            to_u8(u->paint_rgb[1]), to_u8(u->paint_rgb[2]), 0);
                        u->figure_on = 0;
                        say(u, 0, "%s painted", info.name);
                    }
                }
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
    // ...and it is the SAME sentence dai_show_ui_verdict hands a test, from the
    // same function, so a check on the wording checks the panel rather than a
    // second copy of the wording.
    char verdict[128];
    const uint32_t VERDICT_COL[4] = { st->text_dim, COL_WARN, COL_CONFLICT, COL_OK };
    uint32_t verdict_col = VERDICT_COL[verdict_text(u, verdict, sizeof(verdict)) & 3];
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
    // No new screen real estate at all: the parameters tab themselves into
    // the Inspector (both edit the one selected thing) and the validation
    // into the Console (both are the program talking back). The storyboard
    // panel is gone - the hierarchy lists the figures under their steps and
    // the inspector edits the selected one, which is all it ever did.
    dai_dock_add_tab(dock, "Show Parameters", "Inspector");
    dai_dock_add_tab(dock, "Validation", "Console");

    float px, py, pw, ph;
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
