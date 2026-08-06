// Immediate mode UI. See include/dai_ui.h for the design argument.
//
// No Vulkan here: this file turns widgets into triangles and nothing else.

#include "dai_ui.h"
#include "dai_tr.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

struct Batch {
    std::vector<dai_ui_vertex> verts;
    dai_texture texture = 0;
    float clip[4] = { 0, 0, 1e9f, 1e9f };
    // Draw order. Everything outside a window is layer 0; a window's contents
    // get its position in the z order. Sorted (stably) at the end of the frame,
    // which is what lets a click raise a window without the host having to
    // reorder its own calls.
    int layer = 0;
};

uint32_t rgba(int r, int g, int b, int a) {
    return (uint32_t)((a << 24) | (b << 16) | (g << 8) | r);
}

} // namespace

struct dai_ui {
    dai_font *font = nullptr;
    dai_texture font_tex = 0;
    float white_u = 0.0f, white_v = 0.0f;
    dai_ui_style style{};
    int   tr_on = 0;              // translate widget text through dai_tr

    std::vector<Batch> batches;
    std::vector<dai_ui_draw> draws;

    float width = 0, height = 0;
    dai_ui_input input{};
    dai_ui_input prev{};

    // The code editor has the keyboard. Kept next to edit.editing because the
    // host asks one question - "is the user typing" - and must get one answer.
    int code_focus = 0;
    uint64_t color_open = 0;      // which colour field has its panel down
    int      color_drag = 0;      // 0 none, 1 wheel, 2 value bar
    int      color_mode = 0;      // 0 RGB, 1 HSV, 2 Hex
    // The clipboard, handed in and handed back. See dai_ui.h.
    std::string clip_in;
    std::string clip_out;
    bool        clip_out_set = false;

    // layout cursor
    float cursor_x = 0, cursor_y = 0;
    // The rectangle the last row widget claimed, for callers that have to
    // draw ON it (a drop target marker) rather than after it.
    float last_x = 0, last_y = 0, last_w = 0, last_h = 0;
    float panel_x = 0, panel_y = 0, panel_w = 0;
    bool in_panel = false;
    float row_height = 0;
    bool in_row = false;
    float row_start_y = 0, row_start_x = 0;

    // interaction bookkeeping: one hot widget, one active widget, addressed by
    // a hash of the label plus its position - stable across frames, and it does
    // not need the caller to invent ids
    uint64_t hot = 0, active = 0;
    bool mouse_over_ui = false;

    // Clipping is done on the CPU rather than with a scissor rect, so the
    // renderer needs no extra state and a WebGPU or software backend gets it
    // for free. Every widget quad is axis aligned, so the cut is exact.
    struct Clip { float x0, y0, x1, y1; };
    std::vector<Clip> clips;

    std::vector<std::pair<uint64_t, float>> scroll;   // region id -> offset
    float &scroll_of(uint64_t id) {
        for (auto &kv : scroll) if (kv.first == id) return kv.second;
        scroll.push_back({ id, 0.0f });
        return scroll.back().second;
    }
    struct ScrollFrame { uint64_t id; float x, y, w, h, start_y; };
    std::vector<ScrollFrame> scroll_stack;
    std::vector<float> scroll_saved_panel_w;   // width the region borrowed from
    uint64_t scroll_drag_id = 0;   // the bar the pointer is holding
    float    scroll_grab = 0.0f;
    float out_scale = 1.0f;   // logical -> real pixels, applied at the very end
    // How far the content of each region reached LAST frame. The wheel needs
    // it before the content is laid out; without it a region can only find
    // out it should not have scrolled after it already has.
    std::vector<std::pair<uint64_t, float>> scroll_max;
    float &scroll_max_of(uint64_t id) {
        for (auto &e : scroll_max) if (e.first == id) return e.second;
        scroll_max.push_back({ id, 0.0f });
        return scroll_max.back().second;
    }

    float    drag_accum = 0.0f;     // sub-step remainder of a drag_float
    int      cursor_want = DAI_CURSOR_ARROW;   // what the pointer should look like

    // ---- icons ------------------------------------------------------------
    dai_icons  *icons = nullptr;
    dai_texture icon_tex = 0;

    // ---- the one text editor -----------------------------------------------
    // Numeric fields, the name field, the rename row in the hierarchy: all
    // three are a byte buffer, a caret and a selection. There used to be three
    // half implementations of that, which is why only one of them could put
    // the caret in the middle of the text. Now there is one, and Home, Ctrl+A
    // and drag-select work in every field that exists.
    struct Edit {
        uint64_t id = 0;
        char     buf[128] = { 0 };
        uint32_t cursor = 0;      // caret, as a byte offset into buf
        uint32_t anchor = 0;      // the other end of the selection
        bool     editing = false;
        bool     numeric = false; // digits, one leading sign, one dot
        bool     dragging = false;// selecting with the mouse held
        bool     opened_now = false;
        float    text_x = 0;      // where buf is drawn, for caret hit testing
        float    scroll = 0;      // horizontal scroll when the text is too long
    } edit;
    // The id of the field that actually DREW itself this frame. A panel that
    // stops being drawn while one of its fields has the keyboard used to leave
    // edit.editing true for the rest of the session - and "does a text field
    // have the keyboard" is the question F2 asks before it renames anything,
    // and the one a search box asks before it takes focus. A flag only ever
    // set by the thing that is gone is not a flag, it is a fuse.
    uint64_t edit_seen = 0;
    int focus_next_field = 0;   // the next text field takes focus on its own (create flows)
    const char *hot_label = nullptr;   // label of the hovered widget, for drop targets

    // An open menu swallows every hit test outside its own rectangle - see
    // dai_ui_popup_menu. popup_was_open is written at the end of a frame and
    // read at the start of the next, so the click that dismisses the menu
    // cannot fall through to whatever was underneath it.
    bool in_popup = false;
    bool popup_was_open = false;

    // ---- roots and hit testing ---------------------------------------------
    // A "root" is anything that can overlap something else: a window, a popup,
    // a dock panel. Only the FRONTMOST root under the pointer may react to it -
    // otherwise two things that overlap both take the same click, which is
    // exactly the bug where the scene view swallowed the panels behind it.
    struct Root { uint64_t id; float x, y, w, h; int layer; };
    std::vector<Root> roots;          // this frame, in submit order
    std::vector<uint64_t> root_stack; // currently open roots
    uint64_t hover_root = 0;          // decided at the END of the last frame
    uint64_t hover_root_next = 0;
    int      hover_root_layer = -1;

    // The popup stack. A popup is a root with a high layer that also BLOCKS
    // everything below it: the click that closes a menu must not press the
    // button it lands on.
    struct Popup { uint64_t id; float x, y, w, h; };
    std::vector<Popup> popups;
    uint64_t popup_open_id = 0;       // the one dropdown that is open
    // popup panel (widget-hosting popup) save state
    int pp_save_layer = 0;
    std::vector<dai_ui::Clip> pp_save_clips;
    dai_ui_popup *pp_menu = nullptr;
    int      popup_nav = -1;          // highlighted row, keyboard only
    uint64_t popup_opened_frame_id = 0;

    std::vector<int> layer_stack;

    // Is the widget currently being laid out allowed to react to the pointer?
    // Inside a root: only if that root is the frontmost one under the pointer.
    // Outside every root (a toolbar drawn straight onto the surface): yes -
    // such a widget cannot be overlapped by anything that is not a root.
    bool root_ok() const {
        if (root_stack.empty()) return true;
        for (uint64_t id : root_stack) if (id == hover_root) return true;
        return false;
    }
    // What the icon under the pointer means. Collected during the frame and
    // drawn at the very end, on top of everything - a tooltip emitted in place
    // would be painted over by the next window.
    char  tooltip[64] = { 0 };
    float tooltip_x = 0, tooltip_y = 0;
    bool  tooltip_on = false;

    // ---- windows ----------------------------------------------------------
    struct Win {
        uint64_t id = 0;
        char     title[48] = { 0 };
        float    x = 0, y = 0, w = 0, h = 0;   // last frame's rectangle
        bool     seen = false;                 // drawn this frame
        int      dock = 0, slot = 0;           // last frame's dock state
        float    dock_size = 0;   // this window's share of its dock edge
    };
    std::vector<Win> wins;          // z order: back() is in front
    int      cur_layer = 0;
    bool     blocked = false;       // the current window is behind another one
    int   panel_clip_depth = 0;   // the clip a panel pushed, to pop exactly it
    float dock_x = 0, dock_y = 0, dock_w = 0, dock_h = 0;   // area docked windows divide
    bool  dock_area_set = false;
    int   preview_slot = 0;             // set for the drop preview only
    uint64_t drag_win = 0;          // window being moved
    uint64_t size_win = 0;          // window being resized
    int      size_edge = 0;         // 1 left, 2 right, 4 top, 8 bottom
    float    size_fx = 0, size_fy = 0;   // the edges that stay put while dragging
    float    drag_dx = 0, drag_dy = 0;
    int      win_depth = 0;         // inside dai_ui_window_begin/end

    Win *find_win(uint64_t id) {
        for (Win &w : wins) if (w.id == id) return &w;
        return nullptr;
    }
    int layer_of(uint64_t id) {
        for (size_t i = 0; i < wins.size(); ++i) if (wins[i].id == id) return (int)i + 1;
        return 0;
    }
    void raise(uint64_t id) {
        for (size_t i = 0; i < wins.size(); ++i) {
            if (wins[i].id != id) continue;
            Win w = wins[i];
            wins.erase(wins.begin() + (long)i);
            wins.push_back(w);
            return;
        }
    }
    // Is the pointer over a window that is in FRONT of this one? Uses last
    // frame's rectangles, because this frame's are not all known yet - one
    // frame of lag on a window someone just dragged over another, and nothing
    // a user can perceive.
    bool covered_by_higher(uint64_t id, float mx, float my) {
        bool past = false;
        for (const Win &w : wins) {
            if (w.id == id) { past = true; continue; }
            if (!past) continue;
            if (mx >= w.x && mx < w.x + w.w && my >= w.y && my < w.y + w.h) return true;
        }
        return false;
    }

    // A frame is a picture, not a memory sink. When the host loops (a panel
    // iterator that never ended is exactly what shipped once), the buffer used
    // to grow until the process died of std::bad_alloc with nothing on screen
    // and nothing in the log. Past a ceiling no honest frame reaches, drop the
    // rest of the frame and say so - a glitchy frame is a bug report, a dead
    // process is not.
    size_t frame_verts = 0;
    bool   over_budget = false;
    bool take(size_t n) {
        if (over_budget) return false;
        frame_verts += n;
        if (frame_verts > 2000000u) {
            over_budget = true;
            std::fprintf(stderr, "dai_ui: %zu vertices in one frame - the interface is in a "
                                 "loop; dropping the rest of it\n", frame_verts);
            return false;
        }
        return true;
    }

    Batch &batch(dai_texture tex) {
        if (batches.empty() || batches.back().texture != tex || batches.back().layer != cur_layer) {
            batches.push_back(Batch{});
            batches.back().texture = tex;
            batches.back().layer = cur_layer;
        }
        return batches.back();
    }

    void quad(dai_texture tex, float x0, float y0, float x1, float y1,
              float u0, float v0, float u1, float v1, uint32_t col) {
        if (!clips.empty()) {
            const Clip &c = clips.back();
            if (x1 <= c.x0 || x0 >= c.x1 || y1 <= c.y0 || y0 >= c.y1) return;
            // Cut the uv range by the same fraction, or clipped glyphs would
            // stretch instead of being trimmed.
            float du = (u1 - u0) / (x1 - x0 != 0 ? x1 - x0 : 1.0f);
            float dv = (v1 - v0) / (y1 - y0 != 0 ? y1 - y0 : 1.0f);
            if (x0 < c.x0) { u0 += (c.x0 - x0) * du; x0 = c.x0; }
            if (x1 > c.x1) { u1 -= (x1 - c.x1) * du; x1 = c.x1; }
            if (y0 < c.y0) { v0 += (c.y0 - y0) * dv; y0 = c.y0; }
            if (y1 > c.y1) { v1 -= (y1 - c.y1) * dv; y1 = c.y1; }
        }
        if (!take(6)) return;
        Batch &b = batch(tex);
        dai_ui_vertex v[6] = {
            { x0, y0, u0, v0, col }, { x1, y0, u1, v0, col }, { x1, y1, u1, v1, col },
            { x0, y0, u0, v0, col }, { x1, y1, u1, v1, col }, { x0, y1, u0, v1, col },
        };
        b.verts.insert(b.verts.end(), v, v + 6);
    }

    // Four arbitrary corners, wound a-b-c-d. Needed for anything that is not
    // axis aligned - gizmo arms, graph lines, debug overlays.
    void quad4(dai_texture tex, float ax, float ay, float bx, float by,
               float cx, float cy, float dx, float dy, uint32_t col) {
        if (!take(6)) return;
        Batch &b = batch(tex);
        const float u = white_u, v_ = white_v;
        dai_ui_vertex v[6] = {
            { ax, ay, u, v_, col }, { bx, by, u, v_, col }, { cx, cy, u, v_, col },
            { ax, ay, u, v_, col }, { cx, cy, u, v_, col }, { dx, dy, u, v_, col },
        };
        b.verts.insert(b.verts.end(), v, v + 6);
    }
};

namespace {

// Widgets that MUST keep working while a popup is open (the popup itself,
// and the field it was opened from, which keeps its layer so the click that
// summons it can still find it) call plain inside(). Everyone else calls
// inside_chk: with a popup open, they are dead - otherwise a click that is
// meant to close the menu also presses whatever lies under the pointer.
bool inside(const dai_ui *ui, float x, float y, float w, float h);
static bool inside_chk(const dai_ui *ui, float x, float y, float w, float h) {
    if (ui->in_popup) return false;
    return inside(ui, x, y, w, h);
}

uint64_t hash_id(const char *s, float x, float y) {
    uint64_t h = 1469598103934665603ULL;
    for (const char *p = s; p && *p; ++p) { h ^= (uint8_t)*p; h *= 1099511628211ULL; }
    h ^= (uint64_t)(int)(x * 4.0f) * 2654435761u;
    h ^= (uint64_t)(int)(y * 4.0f) * 40503u;
    return h ? h : 1;
}

bool inside(const dai_ui *ui, float x, float y, float w, float h) {
    // A widget under another window must not react. Without this, hovering a
    // button through the window lying on top of it still highlights it, and
    // clicking presses both.
    if (ui->blocked) return false;
    // Same rule, generalised: a widget only reacts if the root it belongs to
    // is the frontmost root under the pointer. Windows had their own version
    // of this (covered_by_higher); everything else - dock panels, popups,
    // the viewport - had none, which is why they fought over clicks.
    if (!ui->root_ok()) return false;
    if (!ui->clips.empty()) {
        // A widget scrolled out of its panel must not react either.
        const dai_ui::Clip &c = ui->clips.back();
        if (ui->input.mouse_x < c.x0 || ui->input.mouse_x >= c.x1 ||
            ui->input.mouse_y < c.y0 || ui->input.mouse_y >= c.y1) return false;
    }
    return ui->input.mouse_x >= x && ui->input.mouse_x < x + w &&
           ui->input.mouse_y >= y && ui->input.mouse_y < y + h;
}

} // namespace

extern "C" {

dai_ui_style dai_ui_style_default(void) {
    dai_ui_style s{};
    // Unity dark, not "a dark theme": panels at 56,56,56, the chrome a step
    // darker, blue accent. The point is not the brand - it is that an editor
    // spends all day next to the viewport, and its surface has to be neutral
    // enough that a colour in the scene is still the colour you picked.
    // The three greys are Unity's, measured off the real editor rather than
    // guessed: #333333 panels, #141414 the chrome behind them, #2A2A2A the
    // inside of every field. Everything else is derived from those so a panel,
    // a button and a text box read as one surface instead of three themes.
    s.panel        = rgba(0x33, 0x33, 0x33, 255);
    s.panel_border = rgba(0x14, 0x14, 0x14, 255);
    s.text         = rgba(0xD2, 0xD2, 0xD2, 255);
    s.text_dim     = rgba(0x9E, 0x9E, 0x9E, 255);
    s.button       = rgba(0x48, 0x48, 0x4A, 255);
    s.button_hover = rgba(0x58, 0x58, 0x5B, 255);
    s.button_active= rgba(0x2F, 0x6C, 0xB5, 255);   // selection: readable, not neon
    s.accent       = rgba(0x3D, 0x84, 0xD8, 255);
    s.track        = rgba(0x2A, 0x2A, 0x2A, 255);   // text fields
    // Editor chrome: the surface the viewport is a hole in.
    s.titlebar         = rgba(0x21, 0x21, 0x21, 255);
    s.titlebar_focused = rgba(0x2D, 0x2D, 0x2D, 255);
    s.chrome           = rgba(0x14, 0x14, 0x14, 255);
    s.shadow           = rgba(0, 0, 0, 90);
    // Compact by default. A 13 px font with 8 px of padding and 6 px between
    // every widget is not a dense editor, it is a phone app: the inspector ran
    // off the bottom of the panel with a dozen fields in it.
    s.padding = 5.0f;
    s.spacing = 3.0f;
    s.rounding = 4.0f;   // soft corners: modern without going full cartoon
    s.border = 1.0f;
    s.label_w = 62.0f;
    s.row_pad = 4.0f;
    return s;
}

dai_ui *dai_ui_create(dai_font *font, dai_texture font_texture) {
    dai_ui *ui = new dai_ui();
    ui->font = font;
    ui->font_tex = font_texture;
    dai_font_white_uv(font, &ui->white_u, &ui->white_v);
    ui->style = dai_ui_style_default();
    return ui;
}

void dai_ui_destroy(dai_ui *ui) { delete ui; }

void dai_ui_font_set(dai_ui *ui, dai_font *font, dai_texture font_texture) {
    if (!ui) return;
    ui->font = font;
    ui->font_tex = font_texture;
    if (font) dai_font_white_uv(font, &ui->white_u, &ui->white_v);
}
dai_ui_style *dai_ui_style_of(dai_ui *ui) { return ui ? &ui->style : nullptr; }

void dai_ui_begin(dai_ui *ui, float width, float height, const dai_ui_input *in) {
    if (!ui) return;
    ui->prev = ui->input;
    if (in) ui->input = *in; else ui->input = dai_ui_input{};
    ui->width = width; ui->height = height;
    ui->batches.clear();
    ui->draws.clear();
    ui->frame_verts = 0;      // the ceiling is per frame, not per lifetime
    ui->over_budget = false;
    ui->cursor_x = ui->cursor_y = 0;
    ui->in_panel = ui->in_row = false;
    ui->hot = 0;
    ui->hot_label = nullptr;
    ui->mouse_over_ui = false;
    ui->clips.clear();
    ui->scroll_stack.clear();
    ui->scroll_saved_panel_w.clear();
    ui->cur_layer = 0;
    ui->blocked = false;
    ui->win_depth = 0;
    ui->tooltip_on = false;
    ui->cursor_want = DAI_CURSOR_ARROW;
    // Raised by whichever code editor draws itself focused THIS frame. A flag
    // that is only ever set is not a flag, it is a fuse.
    ui->code_focus = 0;
    // An open popup swallows every hit test below its own layer. The flag
    // comes from the END of the previous frame, so the click that closes the
    // menu cannot also press the button it lands on.
    ui->in_popup = ui->popup_was_open;
    ui->popup_was_open = false;
    ui->roots.clear();
    ui->root_stack.clear();
    ui->layer_stack.clear();
    ui->hover_root_next = 0;
    ui->hover_root_layer = -1;
    ui->popups.clear();
    if (!ui->dock_area_set) { ui->dock_x = 0; ui->dock_y = 0; ui->dock_w = width; ui->dock_h = height; }
    ui->dock_area_set = false;
    for (auto &w : ui->wins) w.seen = false;
    // NOTE: the active widget is cleared in dai_ui_end, not here. Clearing it
    // at the start of the frame means the widget never sees the release that
    // completes its click - which is exactly the bug the button test caught.
}

void dai_ui_end(dai_ui *ui) {
    if (!ui) return;
    if (ui->tooltip_on && ui->tooltip[0]) {
        // Above every window, and outside every clip rectangle: a tooltip that
        // obeys the panel it was raised in gets cut in half by it.
        int save_layer = ui->cur_layer;
        std::vector<dai_ui::Clip> save_clips;
        save_clips.swap(ui->clips);
        ui->cur_layer = 1 << 20;
        float tw = dai_ui_text_width(ui, ui->tooltip);
        float th = dai_font_line_height(ui->font) + 6.0f;
        float x = ui->tooltip_x, y = ui->tooltip_y;
        if (x + tw + 12.0f > ui->width) x = ui->width - tw - 12.0f;
        if (y + th > ui->height) y = ui->tooltip_y - th - 8.0f;
        if (x < 0) x = 0;
        if (y < 0) y = 0;
        dai_ui_rect(ui, x, y, tw + 12.0f, th, 0xF0101010u);
        dai_ui_rect_outline(ui, x, y, tw + 12.0f, th, 1.0f, ui->style.panel_border);
        dai_ui_text(ui, x + 6.0f, y + 3.0f, ui->tooltip, ui->style.text);
        ui->cur_layer = save_layer;
        ui->clips.swap(save_clips);
    }
    // Nobody drew the field that had the keyboard: its panel was closed, its
    // tab switched away or its row scrolled out of the world. It is over.
    if (ui->edit.editing && ui->edit_seen != ui->edit.id) {
        ui->edit.editing = false;
        ui->edit.dragging = false;
        ui->edit.id = 0;
    }
    ui->edit_seen = 0;
    if (!ui->input.mouse_down) {
        ui->active = 0; ui->drag_win = 0; ui->size_win = 0; ui->size_edge = 0;
        ui->edit.dragging = false;
    }
    // Who is in front under the pointer, for the NEXT frame. Cannot be decided
    // any earlier: the last root submitted may well be the one on top.
    ui->hover_root = ui->hover_root_next;
    // A window the host stopped drawing loses its slot in the z order rather
    // than sitting there forever blocking clicks with a stale rectangle.
    ui->wins.erase(std::remove_if(ui->wins.begin(), ui->wins.end(),
                   [](const dai_ui::Win &w) { return !w.seen; }), ui->wins.end());
    // Stable, so within one layer the host's call order still decides.
    std::stable_sort(ui->batches.begin(), ui->batches.end(),
                     [](const Batch &a, const Batch &b) { return a.layer < b.layer; });
    // Logical pixels became real pixels here, once, for everything: scaling
    // at the source would need every hardcoded 20.0f in the editor multiplied
    // by hand, and one of them would always be missed.
    if (ui->out_scale != 1.0f) {
        const float s = ui->out_scale;
        for (Batch &b : ui->batches) {
            for (dai_ui_vertex &v : b.verts) { v.x *= s; v.y *= s; }
            b.clip[0] *= s; b.clip[1] *= s; b.clip[2] *= s; b.clip[3] *= s;
        }
    }
    for (Batch &b : ui->batches) {
        if (b.verts.empty()) continue;
        dai_ui_draw d{};
        d.vertices = b.verts.data();
        d.count = (uint32_t)b.verts.size();
        d.texture = b.texture;
        std::memcpy(d.clip, b.clip, sizeof(d.clip));
        ui->draws.push_back(d);
    }
}

void dai_ui_scale_set(dai_ui *ui, float scale) {
    if (!ui) return;
    if (!(scale > 0.05f) || scale > 8.0f) scale = 1.0f;
    ui->out_scale = scale;
}
float dai_ui_scale_get(const dai_ui *ui) { return ui ? ui->out_scale : 1.0f; }

uint32_t dai_ui_draws(dai_ui *ui, const dai_ui_draw **out) {
    if (!ui || !out) return 0;
    *out = ui->draws.empty() ? nullptr : ui->draws.data();
    return (uint32_t)ui->draws.size();
}

void dai_ui_mouse(const dai_ui *ui, float *x, float *y, int *down, int *pressed) {
    if (!ui) return;
    if (x) *x = ui->input.mouse_x;
    if (y) *y = ui->input.mouse_y;
    if (down) *down = ui->input.mouse_down;
    if (pressed) *pressed = (ui->input.mouse_down && !ui->prev.mouse_down) ? 1 : 0;
}

float dai_ui_wheel(const dai_ui *ui) { return ui ? ui->input.wheel : 0.0f; }
int dai_ui_double_click(const dai_ui *ui) { return ui ? ui->input.double_click : 0; }

int dai_ui_wants_mouse(const dai_ui *ui) { return ui && ui->mouse_over_ui ? 1 : 0; }

void dai_ui_claim_mouse(dai_ui *ui) { if (ui) ui->mouse_over_ui = true; }

void dai_ui_root_begin(dai_ui *ui, const char *id_str, float x, float y, float w, float h) {
    if (!ui) return;
    uint64_t id = hash_id(id_str ? id_str : "root", 0, 0);
    int layer = ui->cur_layer;
    ui->roots.push_back(dai_ui::Root{ id, x, y, w, h, layer });
    ui->root_stack.push_back(id);
    // Frontmost wins, and later-submitted wins a tie: within one layer the
    // host's own order is the z order.
    float mx = ui->input.mouse_x, my = ui->input.mouse_y;
    if (mx >= x && mx < x + w && my >= y && my < y + h && layer >= ui->hover_root_layer) {
        ui->hover_root_next = id;
        ui->hover_root_layer = layer;
    }
}

void dai_ui_root_end(dai_ui *ui) {
    if (ui && !ui->root_stack.empty()) ui->root_stack.pop_back();
}

int dai_ui_root_hovered(const dai_ui *ui, const char *id_str) {
    if (!ui) return 0;
    return ui->hover_root == hash_id(id_str ? id_str : "root", 0, 0) ? 1 : 0;
}

void dai_ui_layer_push(dai_ui *ui, int layer) {
    if (!ui) return;
    ui->layer_stack.push_back(ui->cur_layer);
    ui->cur_layer = layer;
}

void dai_ui_layer_pop(dai_ui *ui) {
    if (!ui || ui->layer_stack.empty()) return;
    ui->cur_layer = ui->layer_stack.back();
    ui->layer_stack.pop_back();
}

int dai_ui_popup_active(const dai_ui *ui) {
    return ui && (ui->popup_open_id != 0 || ui->popup_was_open) ? 1 : 0;
}
int  dai_ui_cursor(const dai_ui *ui) { return ui ? ui->cursor_want : DAI_CURSOR_ARROW; }
void dai_ui_cursor_set(dai_ui *ui, int cursor) { if (ui) ui->cursor_want = cursor; }
int  dai_ui_text_active(const dai_ui *ui) { return ui && ui->edit.editing ? 1 : 0; }

void dai_ui_clipboard_feed(dai_ui *ui, const char *utf8) {
    if (!ui) return;
    ui->clip_in = utf8 ? utf8 : "";
}

const char *dai_ui_clipboard_taken(dai_ui *ui) {
    if (!ui || !ui->clip_out_set) return nullptr;
    ui->clip_out_set = false;
    return ui->clip_out.c_str();
}

int  dai_ui_code_focused(const dai_ui *ui) { return ui && ui->code_focus ? 1 : 0; }
int  dai_ui_typing(const dai_ui *ui) {
    return ui && (ui->edit.editing || ui->code_focus) ? 1 : 0;
}
// Give the keyboard back. A field keeps focus until something takes it, and
// "something" has to include the scene view: W A S D typed into an invisible
// text box is the bug where the camera turns but never moves.
void dai_ui_text_defocus(dai_ui *ui) {
    if (!ui) return;
    ui->edit.editing = false;
    ui->edit.id = 0;
}

// ---------------------------------------------------------------- drawing

void dai_ui_rect(dai_ui *ui, float x, float y, float w, float h, uint32_t color) {
    if (!ui || w <= 0 || h <= 0) return;
    // Solid rectangles come out of the font atlas too - it reserves a block of
    // full coverage for exactly this. Pointing at the empty border pixel
    // instead (which is what this did) multiplies every panel, button and
    // gizmo line by alpha 0: the interface renders as text floating over the
    // scene, with nothing behind it.
    ui->quad(ui->font_tex, x, y, x + w, y + h,
             ui->white_u, ui->white_v, ui->white_u, ui->white_v, color);
}

void dai_ui_rrect(dai_ui *ui, float x, float y, float w, float h, float radius, uint32_t color) {
    dai_ui_rrect_mask(ui, x, y, w, h, radius, color, 0xF);
}

void dai_ui_rrect_mask(dai_ui *ui, float x, float y, float w, float h,
                       float radius, uint32_t color, int corners) {
    if (!ui || w <= 0 || h <= 0) return;
    float r = radius;
    if (r > w * 0.5f) r = w * 0.5f;
    if (r > h * 0.5f) r = h * 0.5f;
    if (r < 1.0f || !corners) { dai_ui_rect(ui, x, y, w, h, color); return; }
    // Centre cross: two clipped rects, then the corner fans the mask allows.
    // A corner the mask excludes is covered by extending the cross there, so
    // a "top corners only" tab stays square along the bottom edge.
    bool tl = corners & 1, tr = corners & 2, bl = corners & 4, br = corners & 8;
    float top_l = tl ? r : 0.0f, top_r = tr ? r : 0.0f;
    float bot_l = bl ? r : 0.0f, bot_r = br ? r : 0.0f;
    dai_ui_rect(ui, x + top_l, y, w - top_l - top_r, tl || tr ? r : 0.0f, color);              // top strip
    dai_ui_rect(ui, x, y + (tl || tr ? r : 0.0f), w,
                h - (tl || tr ? r : 0.0f) - (bl || br ? r : 0.0f), color);                      // middle
    dai_ui_rect(ui, x + bot_l, y + h - (bl || br ? r : 0.0f), w - bot_l - bot_r,
                bl || br ? r : 0.0f, color);                                                    // bottom strip
    const int SEG = 5;   // 5 segments per quarter: smooth at 4-8 px, cheap
    for (int c = 0; c < 4; ++c) {
        if (!(corners & (1 << c))) continue;
        float cx = (c & 1) ? x + w - r : x + r;
        float cy = (c & 2) ? y + h - r : y + r;
        float a0 = 0.0f;
        switch (c) {
            case 0: a0 = 3.14159265f;        break;   // top left
            case 1: a0 = 3.14159265f * 1.5f; break;   // top right
            case 2: a0 = 3.14159265f * 0.5f; break;   // bottom left
            case 3: a0 = 0.0f;               break;   // bottom right
        }
        // Skip a corner the clip rect cuts away entirely.
        if (!ui->clips.empty()) {
            const dai_ui::Clip &cl = ui->clips.back();
            float x0 = cx - r, x1 = cx + r, y0 = cy - r, y1 = cy + r;
            if (x1 <= cl.x0 || x0 >= cl.x1 || y1 <= cl.y0 || y0 >= cl.y1) continue;
        }
        for (int i = 0; i < SEG; ++i) {
            float a1 = a0 + (3.14159265f * 0.5f) * (float)i / (float)SEG;
            float a2 = a0 + (3.14159265f * 0.5f) * (float)(i + 1) / (float)SEG;
            float px1 = cx + r * cosf(a1), py1 = cy + r * sinf(a1);
            float px2 = cx + r * cosf(a2), py2 = cy + r * sinf(a2);
            ui->quad4(ui->font_tex, cx, cy, px1, py1, px2, py2, px2, py2, color);
        }
    }
}

void dai_ui_rect_outline(dai_ui *ui, float x, float y, float w, float h, float t, uint32_t color) {
    if (!ui) return;
    dai_ui_rect(ui, x, y, w, t, color);
    dai_ui_rect(ui, x, y + h - t, w, t, color);
    dai_ui_rect(ui, x, y, t, h, color);
    dai_ui_rect(ui, x + w - t, y, t, h, color);
}

void dai_ui_line(dai_ui *ui, float x0, float y0, float x1, float y1,
                 float thickness, uint32_t color) {
    if (!ui) return;
    // Lines go through quad4, which is not axis aligned and therefore cannot
    // be cut the way a rectangle is. So the SEGMENT is clipped instead, before
    // the quad exists - Liang-Barsky, exact, and it is what stopped a scrolled
    // hierarchy from drawing its fold arrows above the panel and a gizmo from
    // drawing over the inspector.
    if (!ui->clips.empty()) {
        const dai_ui::Clip &c = ui->clips.back();
        float p[4] = { -(x1 - x0), (x1 - x0), -(y1 - y0), (y1 - y0) };
        float q[4] = { x0 - c.x0, c.x1 - x0, y0 - c.y0, c.y1 - y0 };
        float t0 = 0.0f, t1 = 1.0f;
        for (int i = 0; i < 4; ++i) {
            if (p[i] == 0.0f) { if (q[i] < 0.0f) return; continue; }
            float t = q[i] / p[i];
            if (p[i] < 0.0f) { if (t > t1) return; if (t > t0) t0 = t; }
            else             { if (t < t0) return; if (t < t1) t1 = t; }
        }
        float ox0 = x0, oy0 = y0, odx = x1 - x0, ody = y1 - y0;
        x0 = ox0 + t0 * odx; y0 = oy0 + t0 * ody;
        x1 = ox0 + t1 * odx; y1 = oy0 + t1 * ody;
    }
    float dx = x1 - x0, dy = y1 - y0;
    float len = std::sqrt(dx*dx + dy*dy);
    if (len < 1e-4f) return;
    if (thickness < 0.5f) thickness = 0.5f;
    // Perpendicular offset, half the thickness each way.
    float nx = -dy / len * thickness * 0.5f;
    float ny =  dx / len * thickness * 0.5f;
    ui->quad4(ui->font_tex, x0 + nx, y0 + ny, x1 + nx, y1 + ny,
              x1 - nx, y1 - ny, x0 - nx, y0 - ny, color);
}

void dai_ui_text(dai_ui *ui, float x, float y, const char *utf8, uint32_t color) {
    // The editor's localisation hook: labels go through the table, everything
    // else is a game string we have no business touching. Off by default.
    if (ui && ui->tr_on && utf8) utf8 = dai_tr(utf8);
    if (!ui || !ui->font || !utf8) return;
    float pen_x = x, pen_y = y + dai_font_ascent(ui->font);
    uint32_t off = 0;
    for (;;) {
        uint32_t cp = dai_utf8_next(utf8, &off);
        if (!cp) break;
        if (cp == '\n') { pen_x = x; pen_y += dai_font_line_height(ui->font); continue; }
        const dai_glyph *g = dai_font_glyph(ui->font, cp);
        if (!g) continue;
        if (g->x1 > g->x0)
            // ADD the offsets, do not subtract them. dai_font already returns
            // them in screen convention - y0 above the baseline is negative -
            // so negating again mirrors the glyph about the baseline: a 'T'
            // came out with its bar along the bottom. It hid behind the white
            // box bug for as long as that lasted, and behind the letter H,
            // which is symmetric, for one test after that.
            ui->quad(ui->font_tex, pen_x + g->x0, pen_y + g->y0, pen_x + g->x1, pen_y + g->y1,
                     g->u0, g->v0, g->u1, g->v1, color);
        pen_x += g->advance;
    }
}

// Text at a size the atlas was not rasterised at.
//
// The glyphs are magnified, so a 48 px title is a stretched 13 px atlas and
// looks it. That is a deliberate trade: a HUD label is a handful of words at
// a size the author picked, and rasterising a second atlas per size would
// mean a texture upload every time somebody drags the Size field. Small
// factors (a 24 px label from a 13 px atlas) are visually fine; the fix for
// the day someone wants a 96 px title is a second atlas, not a different
// magnification filter.
void dai_ui_text_scaled(dai_ui *ui, float x, float y, const char *utf8,
                        uint32_t color, float scale) {
    if (!ui || !ui->font || !utf8) return;
    if (!(scale > 0.0f)) scale = 1.0f;
    if (scale > 0.999f && scale < 1.001f) { dai_ui_text(ui, x, y, utf8, color); return; }
    float pen_x = x, pen_y = y + dai_font_ascent(ui->font) * scale;
    uint32_t off = 0;
    for (;;) {
        uint32_t cp = dai_utf8_next(utf8, &off);
        if (!cp) break;
        if (cp == '\n') { pen_x = x; pen_y += dai_font_line_height(ui->font) * scale; continue; }
        const dai_glyph *g = dai_font_glyph(ui->font, cp);
        if (!g) continue;
        if (g->x1 > g->x0)
            ui->quad(ui->font_tex,
                     pen_x + g->x0 * scale, pen_y + g->y0 * scale,
                     pen_x + g->x1 * scale, pen_y + g->y1 * scale,
                     g->u0, g->v0, g->u1, g->v1, color);
        pen_x += g->advance * scale;
    }
}

namespace dai {
float ui_detail_row_x();
float ui_detail_row_y();
float ui_detail_row_w();
float ui_detail_row_h();
// Forget the last field's rectangle. A label is not a field: it never set one,
// so dai_ui_help() after a label attached its tooltip to whatever field came
// BEFORE the label - which is how the localisation hint ended up floating
// across the rows above it, following the pointer over the Text box.
void ui_detail_row_clear();
}

// The same, for a widget that was drawn by hand and therefore has no "row"
// for dai_ui_help to attach to. The caller knows its own rectangle.
void dai_ui_tooltip_at(dai_ui *ui, float x, float y, float w, float h, const char *text) {
    if (!ui || !text || !*text) return;
    if (ui->in_popup || ui->blocked) return;
    if (w <= 0.0f || h <= 0.0f) return;
    if (ui->input.mouse_x < x || ui->input.mouse_x >= x + w) return;
    if (ui->input.mouse_y < y || ui->input.mouse_y >= y + h) return;
    if (ui->input.mouse_down) return;
    std::snprintf(ui->tooltip, sizeof(ui->tooltip), "%s", text);
    ui->tooltip_x = x;
    ui->tooltip_y = y + h + 4.0f;
    ui->tooltip_on = true;
}

// ---------------------------------------------------------------- colour
//
// The wheel is hue by angle and saturation by radius, drawn as a triangle fan
// so it costs one draw call and no texture. Value gets its own bar on the
// right, because a wheel that dims with V is a wheel you cannot pick a dark
// colour on - the ring you want goes black before you reach it.
namespace {

// "X##fpx" is one letter of label and one widget id. Everything after ## is
// the id half: three checkboxes all called "X" need three ids, and nobody
// should be shown the plumbing. It WAS shown - the Freeze Position row read
// "X##fpx Y##fpy Z##fpz" on screen, which is not what anyone typed it for.
const char *vis_label(const char *s, char *tmp, size_t n) {
    if (!s) return "";
    const char *hh = std::strstr(s, "##");
    if (!hh) return s;
    size_t len = (size_t)(hh - s);
    if (len >= n) len = n - 1;
    std::memcpy(tmp, s, len);
    tmp[len] = 0;
    return tmp;
}

void rgb_to_hsv(const float *rgb, float *h, float *sv, float *v) {
    float r = rgb[0], g = rgb[1], b = rgb[2];
    float mx = r > g ? (r > b ? r : b) : (g > b ? g : b);
    float mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
    float d = mx - mn;
    *v = mx;
    *sv = mx > 0.0f ? d / mx : 0.0f;
    if (d <= 0.0f) { *h = 0.0f; return; }
    float hh;
    if (mx == r)      hh = (g - b) / d + (g < b ? 6.0f : 0.0f);
    else if (mx == g) hh = (b - r) / d + 2.0f;
    else              hh = (r - g) / d + 4.0f;
    *h = hh / 6.0f;
}

void hsv_to_rgb(float h, float s, float v, float *rgb) {
    h = h - std::floor(h);
    float i = std::floor(h * 6.0f);
    float f = h * 6.0f - i;
    float p = v * (1.0f - s);
    float q = v * (1.0f - f * s);
    float t = v * (1.0f - (1.0f - f) * s);
    switch ((int)i % 6) {
    case 0: rgb[0] = v; rgb[1] = t; rgb[2] = p; break;
    case 1: rgb[0] = q; rgb[1] = v; rgb[2] = p; break;
    case 2: rgb[0] = p; rgb[1] = v; rgb[2] = t; break;
    case 3: rgb[0] = p; rgb[1] = q; rgb[2] = v; break;
    case 4: rgb[0] = t; rgb[1] = p; rgb[2] = v; break;
    default: rgb[0] = v; rgb[1] = p; rgb[2] = q; break;
    }
}

uint32_t pack_rgb(const float *rgb, float a) {
    auto ch = [](float x) -> uint32_t {
        float c = x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x);
        return (uint32_t)(c * 255.0f + 0.5f);
    };
    return (ch(a) << 24) | (ch(rgb[2]) << 16) | (ch(rgb[1]) << 8) | ch(rgb[0]);
}

} // namespace

// Declared here, defined with the rest of the layout further down: this
// widget is written next to the colour maths it needs, not next to the
// cursor bookkeeping it merely uses.
namespace {
void  next_rect(dai_ui *ui, float w, float h, float *x, float *y);
float widget_height(dai_ui *ui);
}

int dai_ui_color(dai_ui *ui, const char *label, float *rgb, const char *id) {
    if (!ui || !rgb || !id) return 0;
    int changed = 0;
    float h = 0, sv = 0, v = 0;
    rgb_to_hsv(rgb, &h, &sv, &v);

    // ---- the row: label, swatch, hex ---------------------------------------
    float rh = widget_height(ui);
    float x, y;
    next_rect(ui, 0, rh, &x, &y);
    float w = (ui->in_panel ? ui->panel_w - ui->style.padding * 2 : ui->width);
    float lw = w * 0.42f;
    if (lw > 150.0f) lw = 150.0f;
    dai_ui_text(ui, x, y + ui->style.row_pad * 0.5f, label, ui->style.text_dim);

    float sx = x + lw, sw = w - lw;
    dai_ui_rrect(ui, sx, y + 2.0f, sw, rh - 4.0f, 3.0f, ui->style.panel_border);
    dai_ui_rrect(ui, sx + 1.0f, y + 3.0f, sw - 2.0f, rh - 6.0f, 3.0f, pack_rgb(rgb, 1.0f));
    char hex[16];
    std::snprintf(hex, sizeof(hex), "#%02X%02X%02X",
                  (unsigned)(rgb[0] * 255.0f + 0.5f), (unsigned)(rgb[1] * 255.0f + 0.5f),
                  (unsigned)(rgb[2] * 255.0f + 0.5f));
    // Black text on a light swatch, white on a dark one - the label has to be
    // readable on the colour it is naming, whatever that colour turns out to be.
    float lum = 0.299f * rgb[0] + 0.587f * rgb[1] + 0.114f * rgb[2];
    dai_ui_text(ui, sx + 8.0f, y + ui->style.row_pad * 0.5f, hex,
                lum > 0.55f ? 0xFF101010u : 0xFFF0F0F0u);

    uint64_t wid = hash_id(id, x, y);
    bool over = inside_chk(ui, sx, y, sw, rh);
    if (over) { ui->hot = wid; ui->mouse_over_ui = true; ui->cursor_want = DAI_CURSOR_HAND; }
    if (over && ui->input.mouse_down && !ui->prev.mouse_down)
        ui->color_open = (ui->color_open == wid) ? 0 : wid;

    if (ui->color_open != wid) return 0;

    // ---- the panel ---------------------------------------------------------
    const float WHEEL = 132.0f, BAR = 18.0f, GAP = 8.0f;
    const float PANEL_H = WHEEL + 34.0f + rh * 4.0f + 18.0f;
    float px, py;
    next_rect(ui, 0, PANEL_H, &px, &py);
    dai_ui_rrect(ui, px, py, w, PANEL_H, 4.0f, ui->style.track);
    dai_ui_rect_outline(ui, px, py, w, PANEL_H, 1.0f, ui->style.panel_border);

    float cx = px + 8.0f + WHEEL * 0.5f, cy = py + 8.0f + WHEEL * 0.5f;
    float rad = WHEEL * 0.5f;

    // The wheel: 48 wedges, each a fan of 6 rings so saturation is smooth
    // enough at this size without turning into a mesh.
    // Each cell is filled with the colour at its OWN CENTRE. It used to be
    // filled with the colour at its outer edge, so every pixel showed a hue
    // up to 7.5 degrees and a saturation up to a sixth off what clicking it
    // would give you - the wheel and the pick disagreed everywhere.
    const int SEG = 96, RINGS = 16;
    for (int i = 0; i < SEG; ++i) {
        float a0 = (float)i / SEG * 6.2831853f, a1 = (float)(i + 1) / SEG * 6.2831853f;
        float hc = ((float)i + 0.5f) / SEG;
        for (int rr = 0; rr < RINGS; ++rr) {
            float r0 = rad * (float)rr / RINGS, r1 = rad * (float)(rr + 1) / RINGS;
            float cc[3];
            hsv_to_rgb(hc, ((float)rr + 0.5f) / RINGS, v, cc);
            uint32_t col = pack_rgb(cc, 1.0f);
            ui->quad4(ui->font_tex,
                      cx + std::cos(a0) * r0, cy + std::sin(a0) * r0,
                      cx + std::cos(a1) * r0, cy + std::sin(a1) * r0,
                      cx + std::cos(a1) * r1, cy + std::sin(a1) * r1,
                      cx + std::cos(a0) * r1, cy + std::sin(a0) * r1, col);
        }
    }
    // The marker sits where the current colour is.
    {
        float ang = h * 6.2831853f, rr = sv * rad;
        float mx2 = cx + std::cos(ang) * rr, my2 = cy + std::sin(ang) * rr;
        // A ring of short lines: dai_ui has no circle, and a marker that is
        // one colour disappears on half the wheel - so it is drawn twice,
        // dark inside and light outside, and stays visible on every hue.
        for (int k = 0; k < 16; ++k) {
            float b0 = (float)k / 16.0f * 6.2831853f, b1 = (float)(k + 1) / 16.0f * 6.2831853f;
            dai_ui_line(ui, mx2 + std::cos(b0) * 5.0f, my2 + std::sin(b0) * 5.0f,
                            mx2 + std::cos(b1) * 5.0f, my2 + std::sin(b1) * 5.0f, 2.0f, 0xFF101010u);
            dai_ui_line(ui, mx2 + std::cos(b0) * 6.8f, my2 + std::sin(b0) * 6.8f,
                            mx2 + std::cos(b1) * 6.8f, my2 + std::sin(b1) * 6.8f, 1.5f, 0xFFF0F0F0u);
        }
    }

    // Dragging inside the wheel picks hue and saturation.
    float mx = ui->input.mouse_x, my = ui->input.mouse_y;
    bool in_wheel = (mx - cx) * (mx - cx) + (my - cy) * (my - cy) <= rad * rad;
    if (ui->input.mouse_down && !ui->prev.mouse_down && in_wheel) ui->color_drag = 1;
    if (!ui->input.mouse_down) ui->color_drag = 0;
    if (ui->color_drag == 1 && ui->input.mouse_down) {
        float dx = mx - cx, dy = my - cy;
        float d = std::sqrt(dx * dx + dy * dy);
        float nh = std::atan2(dy, dx) / 6.2831853f;
        if (nh < 0.0f) nh += 1.0f;
        float ns = d / rad; if (ns > 1.0f) ns = 1.0f;
        hsv_to_rgb(nh, ns, v > 0.001f ? v : 1.0f, rgb);
        if (v <= 0.001f) v = 1.0f;
        changed = 1;
    }

    // ---- the value bar -----------------------------------------------------
    float bx = px + 8.0f + WHEEL + GAP;
    float bw = 20.0f;
    for (int i = 0; i < 64; ++i) {
        float t0 = (float)i / 64.0f, t1 = (float)(i + 1) / 64.0f;
        float c[3];
        hsv_to_rgb(h, sv, 1.0f - (t0 + t1) * 0.5f, c);
        dai_ui_rect(ui, bx, py + 8.0f + WHEEL * t0, bw, WHEEL * (t1 - t0) + 1.0f,
                    pack_rgb(c, 1.0f));
    }
    dai_ui_rect_outline(ui, bx, py + 8.0f, bw, WHEEL, 1.0f, ui->style.panel_border);
    {
        float vy = py + 8.0f + (1.0f - v) * WHEEL;
        dai_ui_rect(ui, bx - 2.0f, vy - 1.5f, bw + 4.0f, 3.0f, 0xFFFFFFFFu);
        dai_ui_rect_outline(ui, bx - 2.0f, vy - 1.5f, bw + 4.0f, 3.0f, 1.0f, 0xFF000000u);
    }
    bool in_bar = mx >= bx && mx < bx + bw && my >= py + 8.0f && my < py + 8.0f + WHEEL;
    if (ui->input.mouse_down && !ui->prev.mouse_down && in_bar) ui->color_drag = 2;
    if (ui->color_drag == 2 && ui->input.mouse_down) {
        float t = (my - (py + 8.0f)) / WHEEL;
        if (t < 0.0f) t = 0.0f; if (t > 1.0f) t = 1.0f;
        hsv_to_rgb(h, sv, 1.0f - t, rgb);
        changed = 1;
    }

    // ---- the numbers -------------------------------------------------------
    float ny = py + 8.0f + WHEEL + 10.0f;
    static const char *const MODE[] = { "RGB", "HSV", "Hex" };
    float mw = (w - 16.0f) / 3.0f;
    for (int i = 0; i < 3; ++i) {
        float bxx = px + 8.0f + mw * (float)i;
        bool on = ui->color_mode == i;
        bool ov = mx >= bxx && mx < bxx + mw - 2.0f && my >= ny && my < ny + rh - 2.0f;
        dai_ui_rrect(ui, bxx, ny, mw - 2.0f, rh - 2.0f, 3.0f,
                     on ? ui->style.button_active : (ov ? ui->style.button_hover : ui->style.button));
        float tw2 = dai_ui_text_width(ui, MODE[i]);
        dai_ui_text(ui, bxx + (mw - 2.0f - tw2) * 0.5f, ny + 2.0f, MODE[i], ui->style.text);
        if (ov && ui->input.mouse_down && !ui->prev.mouse_down) ui->color_mode = i;
    }
    ny += rh + 4.0f;

    if (ui->color_mode == 2) {
        char hb[16];
        std::snprintf(hb, sizeof(hb), "%02X%02X%02X",
                      (unsigned)(rgb[0] * 255.0f + 0.5f), (unsigned)(rgb[1] * 255.0f + 0.5f),
                      (unsigned)(rgb[2] * 255.0f + 0.5f));
        char fid[64];
        std::snprintf(fid, sizeof(fid), "%s#hex", id);
        if (dai_ui_text_field(ui, fid, px + 8.0f, ny, w - 16.0f, rh - 2.0f, hb, sizeof(hb), nullptr)) {
            unsigned rv = 0, gv = 0, bv = 0;
            const char *hs = hb[0] == '#' ? hb + 1 : hb;
            if (std::sscanf(hs, "%2x%2x%2x", &rv, &gv, &bv) == 3) {
                rgb[0] = rv / 255.0f; rgb[1] = gv / 255.0f; rgb[2] = bv / 255.0f;
                changed = 1;
            }
        }
    } else {
        const char *names_rgb[3] = { "R", "G", "B" };
        const char *names_hsv[3] = { "H", "S", "V" };
        float vals[3];
        if (ui->color_mode == 0) { vals[0] = rgb[0]; vals[1] = rgb[1]; vals[2] = rgb[2]; }
        else                     { vals[0] = h; vals[1] = sv; vals[2] = v; }
        for (int i = 0; i < 3; ++i) {
            char fid[64];
            std::snprintf(fid, sizeof(fid), "%s#c%d", id, i);
            float was = vals[i];
            // Label on the left, the number in a field on the right - the same
            // shape as every other row here, drawn by hand because this panel
            // is not part of the layout flow.
            const char *nm = ui->color_mode == 0 ? names_rgb[i] : names_hsv[i];
            dai_ui_text(ui, px + 10.0f, ny + 2.0f, nm, ui->style.text_dim);
            char nb[32];
            std::snprintf(nb, sizeof(nb), "%.3f", (double)vals[i]);
            if (dai_ui_text_field(ui, fid, px + 34.0f, ny, w - 44.0f, rh - 2.0f,
                                  nb, sizeof(nb), nullptr)) {
                float nv = (float)std::atof(nb);
                if (nv < 0.0f) nv = 0.0f;
                if (nv > 1.0f) nv = 1.0f;
                vals[i] = nv;
            }
            if (vals[i] != was) changed = 1;
            ny += rh;
        }
        if (changed) {
            if (ui->color_mode == 0) { rgb[0] = vals[0]; rgb[1] = vals[1]; rgb[2] = vals[2]; }
            else hsv_to_rgb(vals[0], vals[1], vals[2], rgb);
        }
    }
    return changed;
}

void dai_ui_help(dai_ui *ui, const char *text) {
    if (!ui || !text || !*text) return;
    // Uses the rect of the field drawn immediately before, which is why this
    // is a separate call and not a parameter: half the widgets in this file
    // would need one, and a host that does not want tooltips pays nothing.
    if (ui->in_popup || ui->blocked) return;
    float x = dai::ui_detail_row_x(), y = dai::ui_detail_row_y();
    float w = dai::ui_detail_row_w(), h = dai::ui_detail_row_h();
    if (w <= 0.0f || h <= 0.0f) return;
    if (ui->input.mouse_x < x || ui->input.mouse_x >= x + w) return;
    if (ui->input.mouse_y < y || ui->input.mouse_y >= y + h) return;
    if (ui->input.mouse_down) return;
    std::snprintf(ui->tooltip, sizeof(ui->tooltip), "%s", text);
    ui->tooltip_x = x;
    ui->tooltip_y = y + h + 4.0f;
    ui->tooltip_on = true;
}

float dai_ui_text_height(dai_ui *ui) {
    return ui ? dai_font_line_height(ui->font) : 0.0f;
}

float dai_ui_text_width(dai_ui *ui, const char *utf8) {
    return (ui && ui->font) ? dai_font_measure(ui->font, utf8, nullptr) : 0.0f;
}

// ---------------------------------------------------------------- layout

// ---------------------------------------------------------------- windows

dai_ui_window dai_ui_window_make(float x, float y, float w, float h) {
    dai_ui_window win{};
    win.x = x; win.y = y; win.w = w; win.h = h;
    win.open = 1;
    win.min_w = 120.0f;
    win.min_h = 60.0f;
    return win;
}

dai_ui_window dai_ui_window_docked(int dock, int slot, float size) {
    dai_ui_window w = dai_ui_window_make(0, 0, size, size);
    w.dock = dock;
    w.dock_slot = slot;
    return w;
}

void dai_ui_dock_area(dai_ui *ui, float x, float y, float w, float h) {
    if (!ui) return;
    ui->dock_x = x; ui->dock_y = y; ui->dock_w = w; ui->dock_h = h;
    ui->dock_area_set = true;
}

namespace {

// Where a docked window sits. Windows on the same edge STACK: each one's
// share of the cross axis is its dock_size, normalised to the space that
// exists - so resizing ONE window takes the pixels from its neighbours, which
// is what "dragging the split between two panels resizes both" means. The old
// model gave every window a fixed half and made the split unmoving.
void dock_stack(const dai_ui *ui, int dock, std::vector<const dai_ui::Win *> *out) {
    out->clear();
    for (const dai_ui::Win &w : ui->wins)
        if (w.dock == dock) out->push_back(&w);
    // Slot first, then position: slot 0 means "the whole edge" and comes
    // alone; 1 and 2 are the two halves, in order.
    std::sort(out->begin(), out->end(), [](const dai_ui::Win *a, const dai_ui::Win *b) {
        if (a->slot != b->slot) return a->slot < b->slot;
        return a->y != b->y ? a->y < b->y : a->x < b->x;
    });
}

void dock_rect(const dai_ui *ui, uint64_t self, int dock, float own_w, float own_h,
               float *x, float *y, float *w, float *h) {
    float ax = ui->dock_x, ay = ui->dock_y, aw = ui->dock_w, ah = ui->dock_h;
    bool vertical = (dock == DAI_DOCK_LEFT || dock == DAI_DOCK_RIGHT);
    float span = vertical ? ah : aw;

    std::vector<const dai_ui::Win *> sibs;
    dock_stack(ui, dock, &sibs);
    auto size_of = [&](const dai_ui::Win *s2) {
        float v = s2->dock_size > 0.0f ? s2->dock_size : (vertical ? s2->h : s2->w);
        return v > 1.0f ? v : span / 2.0f;
    };
    float total = 0.0f;
    bool have_self = false;
    for (const dai_ui::Win *s2 : sibs) {
        total += size_of(s2);
        if (s2->id == self) have_self = true;
    }
    if (!have_self) {
        // The drop preview, or the first frame of a freshly docked window: it
        // joins the stack as one share among the others.
        total += own_h > 1.0f ? own_h : span / 2.0f;
    }

    float pos = vertical ? ay : ax;
    float mine = span;
    bool placed = false;
    for (const dai_ui::Win *s2 : sibs) {
        float share = span * size_of(s2) / total;
        if (s2->id == self) { mine = share; placed = true; break; }
        pos += share;
    }
    if (!placed) {
        // The drop preview has no record yet: draw the half the pointer is
        // over, which is what the drop will actually produce.
        if (ui->preview_slot == 1)      { mine = span * 0.5f; pos = vertical ? ay : ax; }
        else if (ui->preview_slot == 2) { mine = span * 0.5f; pos = (vertical ? ay : ax) + span * 0.5f; }
        else { mine = span; pos = vertical ? ay : ax; }
    }

    if (vertical) {
        *x = (dock == DAI_DOCK_LEFT) ? ax : ax + aw - own_w;
        *y = pos; *w = own_w; *h = mine;
    } else {
        *x = pos;
        *y = (dock == DAI_DOCK_TOP) ? ay : ay + ah - own_h;
        *w = mine; *h = own_h;
    }
}

// Which edge is the pointer asking for, and which half of it.
int dock_hit(const dai_ui *ui, float mx, float my, int *slot) {
    const float ZONE = 48.0f;
    float ax = ui->dock_x, ay = ui->dock_y, aw = ui->dock_w, ah = ui->dock_h;
    *slot = 0;
    if (mx < ax - ZONE || mx > ax + aw + ZONE || my < ay - ZONE || my > ay + ah + ZONE)
        return DAI_DOCK_NONE;
    int edge = DAI_DOCK_NONE;
    float best = ZONE;
    if (mx - ax < best)            { best = mx - ax;            edge = DAI_DOCK_LEFT; }
    if ((ax + aw) - mx < best)     { best = (ax + aw) - mx;     edge = DAI_DOCK_RIGHT; }
    if (my - ay < best)            { best = my - ay;            edge = DAI_DOCK_TOP; }
    if ((ay + ah) - my < best)     { best = (ay + ah) - my;     edge = DAI_DOCK_BOTTOM; }
    if (edge == DAI_DOCK_NONE) return DAI_DOCK_NONE;
    // Half of the edge, chosen by where along it the pointer is. Dropping in
    // the middle third takes the whole edge - otherwise a window can never be
    // put back to full height once anything else has been split.
    if (edge == DAI_DOCK_LEFT || edge == DAI_DOCK_RIGHT) {
        float t = ah > 0 ? (my - ay) / ah : 0.5f;
        if (t < 0.34f) *slot = 1;
        else if (t > 0.66f) *slot = 2;
    } else {
        float t = aw > 0 ? (mx - ax) / aw : 0.5f;
        if (t < 0.34f) *slot = 1;
        else if (t > 0.66f) *slot = 2;
    }
    return edge;
}

} // namespace

int dai_ui_window_begin(dai_ui *ui, const char *title, dai_ui_window *win) {
    if (!ui || !win) return 0;
    if (!win->open) return 0;
    if (win->min_w <= 0) win->min_w = 120.0f;
    if (win->min_h <= 0) win->min_h = 60.0f;

    const dai_ui_style &st = ui->style;
    uint64_t id = hash_id(title ? title : "window", 0, 0);
    if (!ui->find_win(id)) {
        dai_ui::Win nw;
        nw.id = id;
        std::snprintf(nw.title, sizeof(nw.title), "%s", title ? title : "");
        ui->wins.push_back(nw);
    }

    float bar = dai_font_line_height(ui->font) + 6.0f;
    float mx = ui->input.mouse_x, my = ui->input.mouse_y;
    bool pressed = ui->input.mouse_down && !ui->prev.mouse_down;

    // The dock state lives in the persistent record, because a stack needs to
    // know its members BEFORE they have all been laid out this frame - last
    // frame's record is the best answer there is, and it is a good one.
    dai_ui::Win *rec = ui->find_win(id);
    rec->dock = win->dock;
    rec->slot = win->dock_slot;
    if (win->dock != DAI_DOCK_NONE && rec->dock_size <= 0.0f)
        rec->dock_size = (win->dock == DAI_DOCK_LEFT || win->dock == DAI_DOCK_RIGHT)
                         ? win->h : win->w;
    if (win->dock == DAI_DOCK_NONE) rec->dock_size = 0.0f;

    // Dragging first, so a window that follows the pointer keeps following it
    // even when the pointer briefly leaves its title bar - anything else makes
    // a fast drag drop the window.
    int drop_dock = DAI_DOCK_NONE, drop_slot = 0;
    if (ui->drag_win == id && ui->input.mouse_down) {
        win->dock = DAI_DOCK_NONE;      // picking it up undocks it
        win->x = mx - ui->drag_dx;
        win->y = my - ui->drag_dy;
        drop_dock = dock_hit(ui, mx, my, &drop_slot);
    } else if (ui->drag_win == id && !ui->input.mouse_down) {
        // The frame the button comes up on: this is the drop. drag_win is
        // cleared in dai_ui_end, so this is the only place that sees it.
        int slot = 0;
        int edge = dock_hit(ui, mx, my, &slot);
        if (edge != DAI_DOCK_NONE) { win->dock = edge; win->dock_slot = slot; }
    } else if (ui->size_win == id && ui->input.mouse_down &&
               (win->dock == DAI_DOCK_LEFT || win->dock == DAI_DOCK_RIGHT) &&
               (ui->size_edge & 12) && !(ui->size_edge & 3)) {
        // A docked window's top/bottom edge is the SPLIT between it and its
        // stack neighbour: the pixels have to come from somewhere, so they
        // come from the adjacent window. Both change - that is the whole
        // point of a split, and what a stack of independent sizes never did.
        float delta = (ui->size_edge & 8) ? (my - (rec->y + rec->h)) : (rec->y - my);
        float want = rec->dock_size + delta;
        if (want >= 40.0f) {
            std::vector<const dai_ui::Win *> sibs;
            dock_stack(ui, win->dock, &sibs);
            for (size_t i = 0; i < sibs.size(); ++i) {
                if (sibs[i]->id != id) continue;
                size_t ni = (ui->size_edge & 8) ? i + 1 : i - 1;
                if (ni >= sibs.size()) break;
                dai_ui::Win *nb = ui->find_win(sibs[ni]->id);
                if (!nb) break;
                float nsize = nb->dock_size > 0.0f ? nb->dock_size : nb->h;
                if (nsize - delta < 40.0f) break;
                nb->dock_size = nsize - delta;
                rec->dock_size = want;
                break;
            }
        }
    } else if (ui->size_win == id && ui->input.mouse_down) {
        // Any edge, and any two of them at a corner. The opposite edge is
        // pinned (size_fx/size_fy), which is what makes dragging the LEFT side
        // of the inspector widen it instead of sliding the whole window - and
        // the left side is the only one a right docked window even has.
        if (ui->size_edge & 2) win->w = mx - win->x + ui->drag_dx;
        if (ui->size_edge & 1) {
            float nx = mx - ui->drag_dx;
            if (ui->size_fx - nx < win->min_w) nx = ui->size_fx - win->min_w;
            win->w = ui->size_fx - nx;
            win->x = nx;
        }
        if (ui->size_edge & 8) win->h = my - win->y + ui->drag_dy;
        if (ui->size_edge & 4) {
            float ny = my - ui->drag_dy;
            if (ui->size_fy - ny < win->min_h) ny = ui->size_fy - win->min_h;
            win->h = ui->size_fy - ny;
            win->y = ny;
        }
    }
    if (win->w < win->min_w) win->w = win->min_w;
    if (win->h < win->min_h) win->h = win->min_h;

    // Snap to the surface edges and to a small margin inside them. This is what
    // makes free floating windows feel like a docked layout without a docking
    // system: everything lines up if you let go anywhere near an edge.
    if (ui->drag_win == id) {
        const float SNAP = 12.0f, M = 6.0f;
        if (std::fabs(win->x - M) < SNAP) win->x = M;
        if (std::fabs(win->y - M) < SNAP) win->y = M;
        if (std::fabs((win->x + win->w) - (ui->width - M)) < SNAP) win->x = ui->width - M - win->w;
        if (std::fabs((win->y + win->h) - (ui->height - M)) < SNAP) win->y = ui->height - M - win->h;
    }
    // Never let a window leave the surface completely: a title bar dragged off
    // the bottom can never be grabbed again.
    if (win->x > ui->width - 40.0f) win->x = ui->width - 40.0f;
    if (win->y > ui->height - bar) win->y = ui->height - bar;
    if (win->x + win->w < 40.0f) win->x = 40.0f - win->w;
    if (win->y < 0.0f) win->y = 0.0f;

    // A docked window does not own its position - the dock area does. Its own
    // width still counts, so the resize grip drags the split.
    if (win->dock != DAI_DOCK_NONE && !(ui->drag_win == id && ui->input.mouse_down)) {
        float dx, dy, dw, dh;
        dock_rect(ui, id, win->dock, win->w, win->h, &dx, &dy, &dw, &dh);
        win->x = dx; win->y = dy;
        if (win->dock == DAI_DOCK_LEFT || win->dock == DAI_DOCK_RIGHT) win->h = dh;
        else                                                          win->w = dw;
    }

    float body_h = win->collapsed ? 0.0f : win->h - bar;
    float full_h = bar + body_h;

    {
        rec->x = win->x; rec->y = win->y; rec->w = win->w; rec->h = full_h;
        rec->seen = true;
        std::snprintf(rec->title, sizeof(rec->title), "%s", title ? title : "");
    }

    ui->blocked = ui->covered_by_higher(id, mx, my);
    ui->cur_layer = DAI_LAYER_WINDOW + ui->layer_of(id);
    ui->win_depth++;
    // A window is a root: everything inside it is only live while it is the
    // frontmost thing under the pointer.
    dai_ui_root_begin(ui, title ? title : "window", win->x, win->y, win->w, full_h);

    bool over_win = !ui->blocked && mx >= win->x - 3.0f && mx < win->x + win->w + 3.0f &&
                    my >= win->y - 3.0f && my < win->y + full_h + 3.0f;
    // A viewport window's body is the 3D view: the title bar and the resize
    // edges belong to the window, the middle belongs to the scene.
    if (over_win && !win->viewport) ui->mouse_over_ui = true;

    bool over_bar = over_win && my < win->y + bar && mx >= win->x && mx < win->x + win->w;
    // Any edge resizes, not a corner grip: 6 px measured from the outside in,
    // so the hot zone is half inside, half outside the rectangle. A window is
    // resized at its borders - that is where every desktop puts it, and a
    // special grip you have to aim for in one corner is a workaround for a
    // missing border hit test.
    const float EDGE = 6.0f;
    // Which edges the pointer is on, as a mask. The hot zone straddles the
    // border (half in, half out) so you do not have to aim at a 1 px line.
    int edge_mask = 0;
    if (!win->collapsed && !ui->blocked && !ui->in_popup &&
        mx >= win->x - EDGE * 0.5f && mx <= win->x + win->w + EDGE * 0.5f &&
        my >= win->y - EDGE * 0.5f && my <= win->y + full_h + EDGE * 0.5f) {
        if (mx <= win->x + EDGE * 0.5f)             edge_mask |= 1;
        if (mx >= win->x + win->w - EDGE)           edge_mask |= 2;
        if (my <= win->y + EDGE * 0.5f)             edge_mask |= 4;
        if (my >= win->y + full_h - EDGE)           edge_mask |= 8;
    }
    bool over_grip = edge_mask != 0;
    // The pointer says what the edge does before you press it. A resize border
    // you can only find by trial is a border nobody finds.
    if (over_grip || ui->size_win == id) {
        int m = ui->size_win == id ? ui->size_edge : edge_mask;
        int cur = DAI_CURSOR_ARROW;
        bool horiz = (m & 3) != 0, vert = (m & 12) != 0;
        if (horiz && vert) {
            bool nwse = ((m & 1) && (m & 4)) || ((m & 2) && (m & 8));
            cur = nwse ? DAI_CURSOR_SIZE_NWSE : DAI_CURSOR_SIZE_NESW;
        } else if (horiz) cur = DAI_CURSOR_SIZE_WE;
        else if (vert)    cur = DAI_CURSOR_SIZE_NS;
        if (cur != DAI_CURSOR_ARROW) ui->cursor_want = cur;
    }

    if (pressed && over_win) {
        ui->raise(id);
        ui->cur_layer = ui->layer_of(id);
        if (over_grip) {
            ui->size_win = id;
            ui->size_edge = edge_mask;
            ui->size_fx = win->x + win->w;      // the pinned right edge
            ui->size_fy = win->y + full_h;      // the pinned bottom edge
            ui->drag_dx = (edge_mask & 1) ? mx - win->x : win->x + win->w - mx;
            ui->drag_dy = (edge_mask & 4) ? my - win->y : win->y + full_h - my;
        } else if (over_bar) {
            // The fold arrow is a button, not a drag handle.
            if (mx < win->x + bar) win->collapsed = !win->collapsed;
            else { ui->drag_win = id; ui->drag_dx = mx - win->x; ui->drag_dy = my - win->y; }
        }
    }

    bool focused = !ui->wins.empty() && ui->wins.back().id == id;

    // shadow, body, title bar, border - in that order. A viewport window has
    // no body to draw: drawing one would paint over the scene it exists to
    // frame.
    dai_ui_rect(ui, win->x + 3.0f, win->y + 3.0f, win->w, full_h + 1.0f, st.shadow);
    if (!win->collapsed && !win->viewport)
        dai_ui_rect(ui, win->x, win->y + bar, win->w, body_h, st.panel);
    dai_ui_rect(ui, win->x, win->y, win->w, bar, focused ? st.titlebar_focused : st.titlebar);
    dai_ui_rect_outline(ui, win->x, win->y, win->w, full_h, st.border,
                        focused ? st.accent : st.panel_border);

    // The window menu button - Unity's ⋮, and the reason the fold arrow is
    // gone: collapsing a docked window does nothing useful, and the arrow was
    // drawn with a glyph this font does not have, so it showed up as a '?'.
    {
        float bx = win->x + 4.0f, by = win->y + 3.0f, bs = bar - 6.0f;
        bool over_btn = !ui->blocked && mx >= bx && mx < bx + bs && my >= by && my < by + bs;
        if (over_btn) dai_ui_rect(ui, bx, by, bs, bs, st.button_hover);
        uint32_t col = over_btn ? st.text : st.text_dim;
        float cx = bx + bs * 0.5f, cy = by + bs * 0.5f;
        for (int i = -1; i <= 1; ++i)
            dai_ui_rect(ui, cx - 1.0f, cy - 1.0f + (float)i * 4.0f, 2.0f, 2.0f, col);
        if (over_btn && pressed) win->menu_wanted = 1;
    }
    // A viewport window's bar belongs to its tabs - printing the title there
    // too would draw "Scene" under the Scene tab.
    if (!win->viewport)
        dai_ui_text(ui, win->x + bar, win->y + 3.0f, title ? title : "", st.text);
    (void)0;

    if (win->collapsed) return 0;


    // Where it would land if the button came up now. Drawn after the window so
    // it is visible over it - a drag with no feedback is a guess.
    if (drop_dock != DAI_DOCK_NONE) {
        float dx, dy, dw, dh;
        ui->preview_slot = drop_slot;
        dock_rect(ui, 0, drop_dock, win->w, win->h, &dx, &dy, &dw, &dh);
        ui->preview_slot = 0;
        uint32_t tint = (st.accent & 0x00FFFFFFu) | 0x50000000u;
        dai_ui_rect(ui, dx, dy, dw, dh, tint);
        dai_ui_rect_outline(ui, dx, dy, dw, dh, 2.0f, st.accent);
    }

    // Everything after this behaves exactly like a panel, clipped to the body.
    ui->clips.push_back(dai_ui::Clip{ win->x, win->y + bar, win->x + win->w, win->y + full_h });
    ui->panel_x = win->x; ui->panel_y = win->y + bar; ui->panel_w = win->w;
    ui->cursor_x = win->x + st.padding;
    ui->cursor_y = win->y + bar + st.padding;
    ui->in_panel = true;
    ui->in_row = false;
    return 1;
}

void dai_ui_window_end(dai_ui *ui) {
    if (!ui) return;
    dai_ui_root_end(ui);
    if (ui->win_depth > 0) {
        ui->win_depth--;
        if (!ui->clips.empty()) ui->clips.pop_back();
    }
    ui->in_panel = false;
    ui->in_row = false;
    ui->blocked = false;
    ui->cur_layer = 0;
}

int dai_ui_window_layer(const dai_ui *ui, const char *title) {
    if (!ui) return 0;
    uint64_t id = hash_id(title ? title : "window", 0, 0);
    for (size_t i = 0; i < ui->wins.size(); ++i)
        if (ui->wins[i].id == id) return (int)i + 1;
    return 0;
}

void dai_ui_layer_set(dai_ui *ui, int layer) { if (ui) ui->cur_layer = layer; }

const char *dai_ui_window_front(const dai_ui *ui) {
    if (!ui || ui->wins.empty()) return "";
    return ui->wins.back().title;
}

void dai_ui_free_area(const dai_ui *ui, float *ox, float *oy, float *ow, float *oh) {
    if (!ui) return;
    float x0 = 0, y0 = 0, x1 = ui->width, y1 = ui->height;
    const float EDGE = 24.0f;          // "touching" the edge
    // A window that sits against an edge and is narrow enough to be a column
    // (or short enough to be a strip) eats into the free area; anything else is
    // floating over the scene and must not shrink it. Two stacked windows in
    // the same column both count, which a "must span the whole side" rule got
    // wrong - and that rule is why the viewport used to report the full width
    // with the hierarchy sitting right on top of it.
    for (const dai_ui::Win &w : ui->wins) {
        float wx0 = w.x, wy0 = w.y, wx1 = w.x + w.w, wy1 = w.y + w.h;
        bool column = w.w <= ui->width * 0.40f;
        bool strip   = w.h <= ui->height * 0.40f;
        if (column && wx0 <= EDGE)                 { if (wx1 > x0) x0 = wx1; }
        else if (column && wx1 >= ui->width - EDGE) { if (wx0 < x1) x1 = wx0; }
        else if (strip && wy0 <= EDGE)             { if (wy1 > y0) y0 = wy1; }
        else if (strip && wy1 >= ui->height - EDGE) { if (wy0 < y1) y1 = wy0; }
    }
    if (ox) *ox = x0;
    if (oy) *oy = y0;
    if (ow) *ow = x1 - x0 > 0 ? x1 - x0 : 0;
    if (oh) *oh = y1 - y0 > 0 ? y1 - y0 : 0;
}

void dai_ui_panel_begin(dai_ui *ui, float x, float y, float w, float h, const char *title) {
    if (!ui) return;
    dai_ui_rect(ui, x, y, w, h, ui->style.panel);
    if (ui->style.border > 0) dai_ui_rect_outline(ui, x, y, w, h, ui->style.border, ui->style.panel_border);
    ui->panel_x = x; ui->panel_y = y; ui->panel_w = w;
    ui->cursor_x = x + ui->style.padding;
    ui->cursor_y = y + ui->style.padding;
    ui->in_panel = true;
    // A row never survives a panel. Without this, a toolbar that ends with
    // dai_ui_row leaves every later panel laying its widgets out sideways -
    // which looks like the panel is empty, because everything piles up off the
    // right edge.
    ui->in_row = false;
    if (title && *title) {
        dai_ui_text(ui, ui->cursor_x, ui->cursor_y, title, ui->style.text);
        ui->cursor_y += dai_font_line_height(ui->font) + ui->style.spacing;
        dai_ui_rect(ui, x + ui->style.padding, ui->cursor_y - ui->style.spacing * 0.5f,
                    w - ui->style.padding * 2, 1.0f, ui->style.panel_border);
    }
    if (inside_chk(ui, x, y, w, h)) ui->mouse_over_ui = true;
    // Contents belong to this panel and nowhere else: an inspector longer
    // than the space it has must scroll, not paint over the panel below.
    ui->clips.push_back(dai_ui::Clip{ x, y, x + w, y + h });
    ui->panel_clip_depth = (int)ui->clips.size();
}

void dai_ui_panel_end(dai_ui *ui) {
    if (!ui) return;
    if ((int)ui->clips.size() >= ui->panel_clip_depth && ui->panel_clip_depth > 0)
        ui->clips.resize((size_t)ui->panel_clip_depth - 1);
    ui->panel_clip_depth = 0;
    ui->in_panel = false;
    if (ui->in_row) {                 // close an open row with the panel
        ui->in_row = false;
        ui->cursor_y = ui->row_start_y + ui->row_height + ui->style.spacing;
    }
}

void dai_ui_row(dai_ui *ui, float height) {
    if (!ui) return;
    if (ui->in_row) dai_ui_row_end(ui);     // two rows in a row are two rows
    ui->in_row = true;
    ui->row_height = height > 0 ? height : dai_font_line_height(ui->font) + ui->style.row_pad;
    ui->row_start_y = ui->cursor_y;
    ui->row_start_x = ui->cursor_x;
}

void dai_ui_row_end(dai_ui *ui) {
    if (!ui || !ui->in_row) return;
    ui->in_row = false;
    ui->cursor_x = ui->row_start_x;
    ui->cursor_y = ui->row_start_y + ui->row_height + ui->style.spacing;
}

void dai_ui_spacing(dai_ui *ui, float px) {
    if (!ui) return;
    // A row runs sideways: leaving space in one has to leave it sideways too.
    // Always adding to cursor_y pushed everything after the gap onto the next
    // line instead of along the current one - which is what made the
    // Materials header and its +/- buttons land on top of their neighbours.
    if (ui->in_row) ui->cursor_x += px;
    else            ui->cursor_y += px;
}

void dai_ui_cursor_pos(const dai_ui *ui, float *x, float *y) {
    if (!ui) return;
    if (x) *x = ui->cursor_x;
    if (y) *y = ui->cursor_y;
}

void dai_ui_last_rect(const dai_ui *ui, float *x, float *y, float *w, float *h) {
    if (!ui) return;
    if (x) *x = ui->last_x;
    if (y) *y = ui->last_y;
    if (w) *w = ui->last_w;
    if (h) *h = ui->last_h;
}

void dai_ui_advance(dai_ui *ui, float w, float h) {
    if (!ui) return;
    if (ui->in_row) ui->cursor_x += w + ui->style.spacing;
    else ui->cursor_y += h + ui->style.spacing;
}

void dai_ui_clip_begin(dai_ui *ui, float x, float y, float w, float h) {
    if (!ui) return;
    dai_ui::Clip c{ x, y, x + w, y + h };
    // Nesting INTERSECTS. Drawing only tests clips.back(), so an inner clip
    // that is taller than the outer one used to let its contents escape the
    // outer box entirely - a console row half scrolled out of the list drew
    // its text over the pane below it. A clip that can widen the visible area
    // is not a clip.
    if (!ui->clips.empty()) {
        const dai_ui::Clip &o = ui->clips.back();
        if (c.x0 < o.x0) c.x0 = o.x0;
        if (c.y0 < o.y0) c.y0 = o.y0;
        if (c.x1 > o.x1) c.x1 = o.x1;
        if (c.y1 > o.y1) c.y1 = o.y1;
    }
    ui->clips.push_back(c);
}

void dai_ui_clip_end(dai_ui *ui) {
    if (!ui || ui->clips.empty()) return;
    ui->clips.pop_back();
}

float dai_ui_panel_width(const dai_ui *ui) {
    if (!ui) return 0.0f;
    return ui->in_panel ? ui->panel_w : ui->width;
}

namespace {

// Reserves the next widget rectangle from the layout cursor.
void next_rect(dai_ui *ui, float w, float h, float *x, float *y) {
    float avail = ui->in_panel ? ui->panel_w - ui->style.padding * 2 : ui->width;
    if (w <= 0) w = avail;
    *x = ui->cursor_x;
    *y = ui->cursor_y;
    if (ui->in_row) {
        ui->cursor_x += w + ui->style.spacing;
    } else {
        ui->cursor_y += h + ui->style.spacing;
    }
}

float widget_height(dai_ui *ui) { return dai_font_line_height(ui->font) + ui->style.row_pad; }

} // namespace

// ---------------------------------------------------------------- widgets

void dai_ui_translate(dai_ui *ui, int on) { if (ui) ui->tr_on = on ? 1 : 0; }

void dai_ui_label(dai_ui *ui, const char *utf8) {
    dai::ui_detail_row_clear();
    if (!ui) return;
    float x, y;
    next_rect(ui, 0, dai_font_line_height(ui->font), &x, &y);
    dai_ui_text(ui, x, y, utf8, ui->style.text);
}

void dai_ui_label_fmt(dai_ui *ui, const char *fmt, ...) {
    dai::ui_detail_row_clear();
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    dai_ui_label(ui, buf);
}

int dai_ui_button(dai_ui *ui, const char *utf8) {
    if (!ui) return 0;
    float h = widget_height(ui);
    float w = ui->in_row ? dai_ui_text_width(ui, utf8) + ui->style.padding * 2 : 0;
    float x, y;
    next_rect(ui, w, h, &x, &y);
    if (w <= 0) w = (ui->in_panel ? ui->panel_w - ui->style.padding * 2 : ui->width);

    uint64_t id = hash_id(utf8, x, y);
    bool over = inside_chk(ui, x, y, w, h);
    if (over) { ui->hot = id; ui->hot_label = utf8; ui->mouse_over_ui = true; }
    bool pressed = false;
    if (over && ui->input.mouse_down && !ui->prev.mouse_down) ui->active = id;
    if (ui->active == id && !ui->input.mouse_down) { pressed = over; ui->active = 0; }

    uint32_t col = ui->style.button;
    if (ui->active == id) col = ui->style.button_active;
    else if (over) col = ui->style.button_hover;
    dai_ui_rrect(ui, x, y, w, h, ui->style.rounding, col);
    float tw = dai_ui_text_width(ui, utf8);
    dai_ui_text(ui, x + (w - tw) * 0.5f, y + ui->style.row_pad * 0.5f, utf8, ui->style.text);
    return pressed ? 1 : 0;
}

int dai_ui_toggle_button(dai_ui *ui, const char *utf8, int active) {
    if (!ui || !utf8) return 0;
    float h = widget_height(ui), x, y;
    // As wide as its label, like every other button here. Full width is what
    // a SECTION looks like: "Edit Collider" stretched across the inspector
    // read as a heading with a box drawn round it, not as something to press.
    float w = dai_ui_text_width(ui, utf8) + ui->style.padding * 3.0f;
    float avail = (ui->in_panel ? ui->panel_w - ui->style.padding * 2 : ui->width);
    if (w > avail) w = avail;
    next_rect(ui, w, h, &x, &y);
    uint64_t id = hash_id(utf8, x, y);
    bool over = inside_chk(ui, x, y, w, h);
    if (over) { ui->hot = id; ui->mouse_over_ui = true; }
    int pressed = 0;
    if (over && ui->input.mouse_down && !ui->prev.mouse_down) ui->active = id;
    if (ui->active == id && !ui->input.mouse_down) { pressed = over ? 1 : 0; ui->active = 0; }
    dai_ui_rrect(ui, x, y + 1.0f, w, h - 2.0f, ui->style.rounding,
                 active ? ui->style.button_active
                        : (over ? ui->style.button_hover : ui->style.button));
    dai_ui_rect_outline(ui, x, y + 1.0f, w, h - 2.0f, 1.0f, ui->style.panel_border);
    float tw = dai_ui_text_width(ui, utf8);
    dai_ui_text(ui, x + (w - tw) * 0.5f, y + ui->style.row_pad * 0.5f, utf8, ui->style.text);
    return pressed;
}

int dai_ui_checkbox(dai_ui *ui, const char *utf8, int *value) {
    if (!ui || !value) return 0;
    float h = widget_height(ui);
    float x, y;
    next_rect(ui, 0, h, &x, &y);
    float box = h - 8.0f;
    char lbuf[96];
    const char *shown = vis_label(utf8, lbuf, sizeof(lbuf));
    uint64_t id = hash_id(utf8, x, y);
    bool over = inside_chk(ui, x, y, box + 8.0f + dai_ui_text_width(ui, shown), h);
    if (over) { ui->hot = id; ui->mouse_over_ui = true; }
    int changed = 0;
    if (over && ui->input.mouse_down && !ui->prev.mouse_down) { *value = !*value; changed = 1; }

    dai_ui_rrect(ui, x, y + 4.0f, box, box, ui->style.rounding * 0.75f,
                 over ? ui->style.button_hover : ui->style.track);
    dai_ui_rect_outline(ui, x, y + 4.0f, box, box, 1.0f, ui->style.panel_border);
    if (*value) {
        // A checkmark, drawn as two strokes. Not a filled tile: the box says
        // "this is a checkbox", the tick says "on", and a blue square said
        // neither - it looked like a colour swatch.
        float bx = x, by = y + 4.0f;
        dai_ui_line(ui, bx + box * 0.20f, by + box * 0.52f, bx + box * 0.42f, by + box * 0.74f,
                    2.0f, ui->style.text);
        dai_ui_line(ui, bx + box * 0.42f, by + box * 0.74f, bx + box * 0.82f, by + box * 0.24f,
                    2.0f, ui->style.text);
    }
    dai_ui_text(ui, x + box + 8.0f, y + ui->style.row_pad * 0.5f, shown, ui->style.text);
    return changed;
}

int dai_ui_slider(dai_ui *ui, const char *utf8, float *value, float min, float max) {
    if (!ui || !value || max <= min) return 0;
    float h = widget_height(ui);
    float x, y;
    next_rect(ui, 0, h, &x, &y);
    float w = (ui->in_panel ? ui->panel_w - ui->style.padding * 2 : ui->width);

    uint64_t id = hash_id(utf8, x, y);
    bool over = inside_chk(ui, x, y, w, h);
    if (over) { ui->hot = id; ui->mouse_over_ui = true; }
    if (over && ui->input.mouse_down && !ui->prev.mouse_down) ui->active = id;
    int changed = 0;
    if (ui->active == id && ui->input.mouse_down) {
        float t = (ui->input.mouse_x - x) / (w > 0 ? w : 1.0f);
        t = t < 0 ? 0 : (t > 1 ? 1 : t);
        float nv = min + (max - min) * t;
        if (nv != *value) { *value = nv; changed = 1; }
    }

    float t = (*value - min) / (max - min);
    t = t < 0 ? 0 : (t > 1 ? 1 : t);
    dai_ui_rect(ui, x, y + h * 0.35f, w, h * 0.3f, ui->style.track);
    dai_ui_rect(ui, x, y + h * 0.35f, w * t, h * 0.3f, ui->style.accent);
    float knob = 8.0f;
    dai_ui_rect(ui, x + w * t - knob * 0.5f, y + 2.0f, knob, h - 4.0f,
                ui->active == id ? ui->style.button_active : ui->style.button_hover);
    char buf[160];
    std::snprintf(buf, sizeof(buf), "%s: %.2f", utf8 ? utf8 : "", *value);
    dai_ui_text(ui, x, y + ui->style.row_pad * 0.5f, buf, ui->style.text_dim);
    return changed;
}

void dai_ui_progress(dai_ui *ui, float fraction, const char *utf8) {
    if (!ui) return;
    float h = widget_height(ui);
    float x, y;
    next_rect(ui, 0, h, &x, &y);
    float w = (ui->in_panel ? ui->panel_w - ui->style.padding * 2 : ui->width);
    fraction = fraction < 0 ? 0 : (fraction > 1 ? 1 : fraction);
    dai_ui_rect(ui, x, y, w, h, ui->style.track);
    dai_ui_rect(ui, x, y, w * fraction, h, ui->style.accent);
    if (utf8 && *utf8) {
        float tw = dai_ui_text_width(ui, utf8);
        dai_ui_text(ui, x + (w - tw) * 0.5f, y + ui->style.row_pad * 0.5f, utf8, ui->style.text);
    }
}

void dai_ui_set_icons(dai_ui *ui, dai_icons *icons, dai_texture tex) {
    if (!ui) return;
    ui->icons = icons;
    ui->icon_tex = tex;
}

int dai_ui_has_icon(const dai_ui *ui, const char *name) {
    if (!ui || !ui->icons || !name) return 0;
    return dai_icons_uv(ui->icons, name, nullptr, nullptr, nullptr, nullptr);
}

void dai_ui_icon_at(dai_ui *ui, const char *name, float x, float y,
                    float size, uint32_t color) {
    if (!ui || !ui->icons || !name) return;
    if (!color) color = ui->style.text;
    // A colored icon is its own picture; tinting it with the text color is
    // what turned the yellow warning triangle gray. Its OPACITY is still the
    // caller's, though: "half strength" is a statement about state, not about
    // hue, and forcing opaque white threw that away along with the tint. The
    // atlas keeps colored cells in straight alpha, so scaling the vertex
    // alpha is exactly a fade - no fringe, no darkening.
    if (dai_icons_colored(ui->icons, name))
        color = 0x00FFFFFFu | (color & 0xFF000000u);
    float u0, v0, u1, v1;
    if (!dai_icons_uv(ui->icons, name, &u0, &v0, &u1, &v1)) return;
    if (size <= 0.0f) size = dai_icons_size(ui->icons);
    // Snapped to whole pixels. Half a pixel of offset on a 16 px icon made of
    // 1.3 px strokes is the difference between a crisp line and a grey smear.
    x = std::floor(x + 0.5f);
    y = std::floor(y + 0.5f);
    ui->quad(ui->icon_tex, x, y, x + size, y + size, u0, v0, u1, v1, color);
}

void dai_ui_icon(dai_ui *ui, const char *name, float size, uint32_t color) {
    if (!ui) return;
    if (size <= 0.0f) size = ui->icons ? dai_icons_size(ui->icons) : 16.0f;
    float x, y;
    next_rect(ui, size, size, &x, &y);
    dai_ui_icon_at(ui, name, x, y, size, color);
}

int dai_ui_icon_button(dai_ui *ui, const char *name, const char *tooltip, int active) {
    if (!ui) return 0;
    float h = widget_height(ui);
    float w = h;                       // square, so a toolbar reads as a strip
    float x, y;
    next_rect(ui, w, h, &x, &y);

    uint64_t id = hash_id(name ? name : "icon", x, y);
    bool over = inside_chk(ui, x, y, w, h);
    if (over) { ui->hot = id; ui->mouse_over_ui = true; }
    bool pressed = false;
    if (over && ui->input.mouse_down && !ui->prev.mouse_down) ui->active = id;
    if (ui->active == id && !ui->input.mouse_down) { pressed = over; ui->active = 0; }

    uint32_t bg = active ? ui->style.accent : ui->style.button;
    if (ui->active == id) bg = ui->style.button_active;
    else if (over && !active) bg = ui->style.button_hover;
    dai_ui_rrect(ui, x, y, w, h, ui->style.rounding, bg);

    float isz = dai_icons_size(ui->icons);
    if (isz <= 0.0f || isz > h - 4.0f) isz = h - 6.0f;
    uint32_t tint = active ? 0xFF101010u : ui->style.text;
    if (dai_ui_has_icon(ui, name)) {
        dai_ui_icon_at(ui, name, x + (w - isz) * 0.5f, y + (h - isz) * 0.5f, isz, tint);
    } else if (tooltip) {
        // No icon set loaded: fall back to the words, so the editor is still
        // usable rather than a row of empty squares.
        float tw = dai_ui_text_width(ui, tooltip);
        dai_ui_text(ui, x + (w - tw) * 0.5f, y + ui->style.row_pad * 0.5f, tooltip, tint);
    }
    if (over && tooltip && !ui->input.mouse_down) {
        std::snprintf(ui->tooltip, sizeof(ui->tooltip), "%s", tooltip);
        ui->tooltip_x = x;
        ui->tooltip_y = y + h + 6.0f;
        ui->tooltip_on = true;
    }
    return pressed ? 1 : 0;
}

void dai_ui_toolbar_gap(dai_ui *ui, float w) {
    if (!ui) return;
    if (ui->in_row) ui->cursor_x += w;
    else            ui->cursor_y += w;
}

int dai_ui_header(dai_ui *ui, const char *title, int *open, int *enabled) {
    return dai_ui_header_icon(ui, nullptr, title, open, enabled);
}

int dai_ui_header_icon_col(dai_ui *ui, const char *icon, uint32_t tint,
                           const char *title, int *open, int *enabled);

int dai_ui_header_icon(dai_ui *ui, const char *icon, const char *title,
                       int *open, int *enabled) {
    return dai_ui_header_icon_col(ui, icon, 0, title, open, enabled);
}

int dai_ui_header_icon_col(dai_ui *ui, const char *icon, uint32_t tint,
                           const char *title, int *open, int *enabled) {
    if (!ui || !title) return 0;
    float h = dai_font_line_height(ui->font) + 6.0f;
    float x, y;
    next_rect(ui, 0, h, &x, &y);
    float w = (ui->in_panel ? ui->panel_w - ui->style.padding * 2 : ui->width);

    uint64_t id = hash_id(title, x, y);
    bool over = inside_chk(ui, x, y, w, h);
    if (over) { ui->hot = id; ui->mouse_over_ui = true; }
    int result = 0;
    float box = h - 6.0f;
    bool on_box = enabled && over && ui->input.mouse_x > x + w - box - 6.0f;
    if (over && ui->input.mouse_down && !ui->prev.mouse_down) {
        if (on_box) { *enabled = !*enabled; result = 2; }
        else if (open) { *open = !*open; result = 1; }
    }
    // Right click on the header (not its enable box) reports 3: the caller's
    // context menu - Unity opens Copy/Paste/Remove there, and so do we.
    if (over && !on_box && ui->input.right_down && !ui->prev.right_down) result = 3;

    // #3E3E3E - a component header is quieter than a button. The button grey
    // made every header read as something to press, which is how "Is Trigger"
    // ended up looking like it lived on a toolbar.
    dai_ui_rect(ui, x, y, w, h, over ? rgba(0x48, 0x48, 0x48, 255) : rgba(0x3E, 0x3E, 0x3E, 255));
    dai_ui_rect(ui, x, y, 3.0f, h, ui->style.accent);

    // The fold arrow, then the component's own icon, then its name. Falls back
    // to the two triangle glyphs when no icon set was given - the same header
    // has to work in a headless test with nothing but a font.
    float tx = x + 8.0f;
    bool folded = (open && !*open);
    const char *chev = folded ? "chevron-right" : "chevron-down";
    float isz = dai_icons_size(ui->icons);
    if (isz <= 0.0f || isz > h - 2.0f) isz = h - 4.0f;
    if (dai_ui_has_icon(ui, chev)) {
        dai_ui_icon_at(ui, chev, tx, y + (h - isz) * 0.5f, isz, ui->style.text_dim);
        tx += isz + 3.0f;
    } else {
        float ax = tx + 5.0f, ay = y + h * 0.5f;
        if (folded) {
            dai_ui_line(ui, ax - 1.5f, ay - 3.5f, ax + 2.5f, ay, 1.6f, ui->style.text_dim);
            dai_ui_line(ui, ax + 2.5f, ay, ax - 1.5f, ay + 3.5f, 1.6f, ui->style.text_dim);
        } else {
            dai_ui_line(ui, ax - 3.5f, ay - 1.5f, ax, ay + 2.5f, 1.6f, ui->style.text_dim);
            dai_ui_line(ui, ax, ay + 2.5f, ax + 3.5f, ay - 1.5f, 1.6f, ui->style.text_dim);
        }
        tx += 14.0f;
    }
    if (icon && dai_ui_has_icon(ui, icon)) {
        dai_ui_icon_at(ui, icon, tx, y + (h - isz) * 0.5f, isz,
                       tint ? tint : ui->style.accent);
        tx += isz + 5.0f;
    }
    dai_ui_text(ui, tx, y + 2.0f, title, ui->style.text);
    if (enabled) {
        float bx = x + w - box - 4.0f, by = y + 3.0f;
        dai_ui_rect(ui, bx, by, box, box, on_box ? ui->style.button_hover : ui->style.track);
        dai_ui_rect_outline(ui, bx, by, box, box, 1.0f, ui->style.panel_border);
        if (*enabled) {
            dai_ui_line(ui, bx + box * 0.20f, by + box * 0.52f, bx + box * 0.42f, by + box * 0.74f,
                        2.0f, ui->style.text);
            dai_ui_line(ui, bx + box * 0.42f, by + box * 0.74f, bx + box * 0.82f, by + box * 0.24f,
                        2.0f, ui->style.text);
        }
    }
    return result;
}

// A fold INSIDE a component, Unity's shape: a small triangle, a dim label,
// no bar and no accent stripe. A second full-width header made "Constraints"
// read as a second component - which is exactly what it looked like.
int dai_ui_subheader(dai_ui *ui, const char *title, int *open) {
    if (!ui || !title) return 0;
    float h = dai_font_line_height(ui->font) + 4.0f;
    float x, y;
    next_rect(ui, 0, h, &x, &y);
    float w = (ui->in_panel ? ui->panel_w - ui->style.padding * 2 : ui->width);
    uint64_t id = hash_id(title, x, y);
    bool over = inside_chk(ui, x, y, w, h);
    if (over) { ui->hot = id; ui->mouse_over_ui = true; }
    int clicked = 0;
    if (over && ui->input.mouse_down && !ui->prev.mouse_down) {
        if (open) *open = !*open;
        clicked = 1;
    }
    float tx = x + 4.0f;
    bool folded = (open && !*open);
    float isz = dai_icons_size(ui->icons);
    if (isz <= 0.0f || isz > h - 2.0f) isz = h - 4.0f;
    const char *chev = folded ? "chevron-right" : "chevron-down";
    if (dai_ui_has_icon(ui, chev)) {
        dai_ui_icon_at(ui, chev, tx, y + (h - isz) * 0.5f, isz, ui->style.text_dim);
        tx += isz + 3.0f;
    } else {
        float ax = tx + 5.0f, ay = y + h * 0.5f;
        if (folded) {
            dai_ui_line(ui, ax - 1.5f, ay - 3.5f, ax + 2.5f, ay, 1.6f, ui->style.text_dim);
            dai_ui_line(ui, ax + 2.5f, ay, ax - 1.5f, ay + 3.5f, 1.6f, ui->style.text_dim);
        } else {
            dai_ui_line(ui, ax - 3.5f, ay - 1.5f, ax, ay + 2.5f, 1.6f, ui->style.text_dim);
            dai_ui_line(ui, ax, ay + 2.5f, ax + 3.5f, ay - 1.5f, 1.6f, ui->style.text_dim);
        }
        tx += 14.0f;
    }
    dai_ui_text(ui, tx, y + 1.0f, title, over ? ui->style.text : ui->style.text_dim);
    return clicked;
}

// A multi-line text field. The code editor with the code turned off: it
// already knows about carets, selections, Enter and scrolling, and a second
// implementation of all four would be a second set of the same bugs.
//
// The per-field state lives here, keyed by the widget id, so callers keep
// passing a plain char buffer like every other field in this file.
int dai_ui_input_multiline(dai_ui *ui, const char *label, char *buf, size_t buf_size,
                           int rows) {
    if (!ui || !buf || buf_size < 2) return 0;
    if (rows < 2) rows = 3;
    float lh = dai_font_line_height(ui->font);
    float h = lh * (float)rows + 10.0f;
    // The same split every other field uses. Spelled out here rather than
    // through field_rect(), which lives further down the file - a multi-line
    // field is not worth reordering three hundred lines for.
    float rx, ry;
    next_rect(ui, 0, h, &rx, &ry);
    float full = (ui->in_panel ? ui->panel_w - ui->style.padding * 2 : ui->width);
    float lw = ui->style.label_w > 0 ? ui->style.label_w : 62.0f;
    if (label && *label) dai_ui_text(ui, rx, ry + 2.0f, label, ui->style.text_dim);
    float x = (label && *label) ? rx + lw : rx;
    float y = ry;
    float w = (label && *label) ? full - lw : full;
    if (w < 40.0f) w = 40.0f;
    uint64_t id = hash_id(label ? label : "multi", x, y);

    static std::vector<std::pair<uint64_t, dai_ui_code_state> > states;
    dai_ui_code_state *st = nullptr;
    for (size_t i = 0; i < states.size(); ++i)
        if (states[i].first == id) { st = &states[i].second; break; }
    if (!st) {
        dai_ui_code_state fresh{};
        fresh.plain = 1;
        states.push_back(std::make_pair(id, fresh));
        st = &states.back().second;
    }
    st->plain = 1;

    char sid[48];
    std::snprintf(sid, sizeof(sid), "ml%llu", (unsigned long long)id);
    dai_ui_rect(ui, x, y, w, h, ui->style.track);
    dai_ui_rect_outline(ui, x, y, w, h, 1.0f,
                        st->focused ? ui->style.accent : ui->style.panel_border);
    return dai_ui_code_edit(ui, sid, x + 1.0f, y + 1.0f, w - 2.0f, h - 2.0f,
                            buf, buf_size, st, DAI_CODE_LANG_NONE);
}

// "Freeze Position  [x] X  [x] Y  [x] Z" as ONE widget, drawn with explicit
// rectangles.
//
// It was three dai_ui_checkbox calls inside a dai_ui_row after a dai_ui_label.
// A label does not advance the row cursor - it never had to, because nothing
// was ever drawn beside one - so all three ticks were laid on top of the
// caption and the row showed a caption and nothing else. Three checkboxes
// were there the whole time, stacked in the same six pixels.
int dai_ui_axis_toggles(dai_ui *ui, const char *label, uint32_t *bits,
                        uint32_t bx_, uint32_t by_, uint32_t bz_) {
    if (!ui || !bits) return 0;
    float h = widget_height(ui);
    float x, y;
    next_rect(ui, 0, h, &x, &y);
    float full = (ui->in_panel ? ui->panel_w - ui->style.padding * 2 : ui->width);
    float lw = ui->style.label_w > 0 ? ui->style.label_w : 62.0f;
    if (lw > full * 0.55f) lw = full * 0.55f;
    if (label && *label) dai_ui_text(ui, x, y + 2.0f, label, ui->style.text_dim);

    const uint32_t BIT[3] = { bx_, by_, bz_ };
    static const char *const NAME[3] = { "X", "Y", "Z" };
    float box = h - 8.0f;
    float step = box + 6.0f + dai_ui_text_width(ui, "X") + 12.0f;
    float cx = x + lw;
    int changed = 0;
    for (int i = 0; i < 3; ++i) {
        float bxp = cx, byp = y + 4.0f;
        float hitw = box + 4.0f + dai_ui_text_width(ui, NAME[i]);
        bool over = inside_chk(ui, bxp, y, hitw, h);
        if (over) { ui->mouse_over_ui = true; ui->cursor_want = DAI_CURSOR_HAND; }
        if (over && ui->input.mouse_down && !ui->prev.mouse_down) {
            *bits ^= BIT[i];
            changed = 1;
        }
        bool on = (*bits & BIT[i]) != 0;
        dai_ui_rrect(ui, bxp, byp, box, box, ui->style.rounding * 0.75f,
                     over ? ui->style.button_hover : ui->style.track);
        dai_ui_rect_outline(ui, bxp, byp, box, box, 1.0f, ui->style.panel_border);
        if (on) {
            dai_ui_line(ui, bxp + box * 0.20f, byp + box * 0.52f,
                            bxp + box * 0.42f, byp + box * 0.74f, 2.0f, ui->style.text);
            dai_ui_line(ui, bxp + box * 0.42f, byp + box * 0.74f,
                            bxp + box * 0.82f, byp + box * 0.24f, 2.0f, ui->style.text);
        }
        dai_ui_text(ui, bxp + box + 4.0f, y + ui->style.row_pad * 0.5f, NAME[i],
                    ui->style.text);
        cx += step;
    }
    return changed;
}

// A row of buttons of which exactly one is on - Unity's alignment strip.
// A dropdown hides the choice behind a click; a strip shows all of them and
// costs one click. For three or four short options that is simply better.
int dai_ui_seg_buttons(dai_ui *ui, const char *label, int *value,
                       const char *const *items, int count) {
    if (!ui || !value || !items || count <= 0) return 0;
    float h = widget_height(ui);
    float x, y;
    next_rect(ui, 0, h, &x, &y);
    float full = (ui->in_panel ? ui->panel_w - ui->style.padding * 2 : ui->width);
    float lw = 0.0f;
    if (label && *label) {
        lw = ui->style.label_w > 0 ? ui->style.label_w : 62.0f;
        if (lw > full * 0.55f) lw = full * 0.55f;
        dai_ui_text(ui, x, y + 2.0f, label, ui->style.text_dim);
    }
    float sx = x + lw;
    float sw = full - lw;
    if (sw < 40.0f) sw = 40.0f;
    float bw = sw / (float)count;
    float bh = h - 2.0f;
    int changed = 0;
    for (int i = 0; i < count; ++i) {
        float bx = sx + bw * (float)i;
        bool over = inside_chk(ui, bx, y + 1.0f, bw, bh);
        if (over) { ui->mouse_over_ui = true; ui->cursor_want = DAI_CURSOR_HAND; }
        if (over && ui->input.mouse_down && !ui->prev.mouse_down && *value != i) {
            *value = i;
            changed = 1;
        }
        bool on = (*value == i);
        uint32_t bg = on ? ui->style.accent
                         : (over ? ui->style.button_hover : ui->style.button);
        dai_ui_rect(ui, bx, y + 1.0f, bw - 1.0f, bh, bg);
        dai_ui_rect_outline(ui, bx, y + 1.0f, bw - 1.0f, bh, 1.0f, ui->style.panel_border);
        const char *t = items[i] ? items[i] : "";
        float tw = dai_ui_text_width(ui, t);
        dai_ui_text(ui, bx + (bw - 1.0f - tw) * 0.5f,
                    y + (bh - dai_font_line_height(ui->font)) * 0.5f + 1.0f, t,
                    on ? 0xFFFFFFFFu : ui->style.text);
    }
    return changed;
}

void dai_ui_separator(dai_ui *ui) {
    if (!ui) return;
    float x, y;
    next_rect(ui, 0, 1.0f, &x, &y);
    float w = (ui->in_panel ? ui->panel_w - ui->style.padding * 2 : ui->width);
    dai_ui_rect(ui, x, y, w, 1.0f, ui->style.panel_border);
}


// ------------------------------------------------- editor field widgets

namespace {

// Label on the left, field on the right. A fixed split keeps a column of
// fields aligned without a layout engine.
// The label column. In the style, because a 13 px font wants a narrower one
// than a 20 px font and the editor is free to change it.

// Where the label of the last field_rect went. Reported separately instead of
// through more out parameters because every caller wants the field rect and
// only the numeric ones want the label: the label is a DRAG HANDLE (Unity
// scrubs a value by dragging its name), and a widget that cannot report where
// its name is cannot offer that.
float g_label_x = 0.0f, g_label_y = 0.0f, g_label_w = 0.0f;
// The whole row of the last field, label column included - what a help
// tooltip has to hover over. A field whose units are only in the manual is a
// field whose units nobody knows: "Friction 10" means nothing until you learn
// it is a coefficient and not a percentage.
float g_row_x = 0.0f, g_row_y = 0.0f, g_row_w = 0.0f, g_row_h = 0.0f;

void field_rect(dai_ui *ui, const char *label, float *x, float *y, float *w, float h) {
    float rx, ry;
    next_rect(ui, 0, h, &rx, &ry);
    float full = (ui->in_panel ? ui->panel_w - ui->style.padding * 2 : ui->width);
    // Inside a row, "full width" means "what is LEFT of the row", not the
    // whole panel: the second field of a row was drawn at full width from
    // where the first one ended, i.e. mostly off the right edge.
    if (ui->in_row) {
        float left_edge = (ui->in_panel ? ui->panel_x + ui->style.padding : 0.0f);
        float used = rx - left_edge;
        full -= used;
        if (full < 24.0f) full = 24.0f;
    }
    g_label_x = rx; g_label_y = ry; g_label_w = 0.0f;
    g_row_x = rx; g_row_y = ry; g_row_w = full; g_row_h = h;
    if (label && *label) {
        float lw = ui->style.label_w > 0 ? ui->style.label_w : 62.0f;
        dai_ui_text(ui, rx, ry + 2.0f, label, ui->style.text_dim);
        *x = rx + lw;
        *w = full - lw;
        g_label_w = lw - 2.0f;
    } else {
        *x = rx;
        *w = full;
    }
    *y = ry;
}

// Drag the label sideways to change the value. `step` is units per pixel, and
// Shift/Ctrl are the two speeds every DCC tool has: fine and coarse.
//
// This lives next to field_rect and not inside num_field_at because the label
// is drawn by field_rect and is OUTSIDE the field's own rectangle - the drag
// zone and the widget are two different rects, and pretending otherwise is why
// only the X/Y/Z fields used to be draggable.
int label_scrub(dai_ui *ui, uint64_t id, float lx, float ly, float lw, float lh,
                float *value, float step, float min, float max) {
    if (lw <= 0.0f || !value) return 0;
    if (step <= 0.0f) step = 0.01f;
    bool over = !ui->in_popup && !ui->blocked &&
                ui->input.mouse_x >= lx && ui->input.mouse_x < lx + lw &&
                ui->input.mouse_y >= ly && ui->input.mouse_y < ly + lh;
    if (over) { ui->hot = id; ui->mouse_over_ui = true; ui->cursor_want = DAI_CURSOR_SIZE_WE; }
    if (over && ui->input.mouse_down && !ui->prev.mouse_down) ui->active = id;
    int changed = 0;
    if (ui->active == id) {
        if (!ui->input.mouse_down) { ui->active = 0; return 0; }
        ui->cursor_want = DAI_CURSOR_SIZE_WE;
        float dx = ui->input.mouse_x - ui->prev.mouse_x;
        if (dx != 0.0f) {
            float sp = step;
            if (ui->input.key_shift) sp *= 10.0f;
            if (ui->input.key_ctrl)  sp *= 0.1f;
            float v = *value + dx * sp;
            if (min < max) v = v < min ? min : (v > max ? max : v);
            if (v != *value) { *value = v; changed = 1; }
        }
    }
    // The name lights up while it is a handle, so the gesture is discoverable
    // without a manual.
    if (over || ui->active == id) {
        // redraw over the dim text field_rect already emitted
        dai_ui_rect(ui, lx, ly + lh - 1.0f, lw, 1.0f, ui->style.accent);
    }
    return changed;
}

std::string trim_number(float v) {
    // Four significant figures, trailing zeros gone. The old version snapped
    // anything small to zero - which in an inspector means friction, bounce,
    // colour and every half a degree the user typed in is displayed back as 0.
    // A field that lies about what you typed is worse than one that drags.
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.4g", (double)v);
    return buf;
}

int drag_float_at(dai_ui *ui, uint64_t id, float x, float y, float w, float h,
                  float *value, float step, const char *prefix, uint32_t accent) {
    bool over = inside_chk(ui, x, y, w, h);
    if (over) { ui->hot = id; ui->mouse_over_ui = true; }
    if (over && ui->input.mouse_down && !ui->prev.mouse_down) {
        ui->active = id;
        ui->drag_accum = 0.0f;
    }
    int changed = 0;
    if (ui->active == id && ui->input.mouse_down) {
        float dx = ui->input.mouse_x - ui->prev.mouse_x;
        if (dx != 0.0f) {
            *value += dx * step;
            changed = 1;
        }
    }
    dai_ui_rect(ui, x, y + 2.0f, w, h - 4.0f,
                ui->active == id ? ui->style.button_active
                                 : (over ? ui->style.button_hover : ui->style.track));
    if (accent) dai_ui_rect(ui, x, y + 2.0f, 3.0f, h - 4.0f, accent);
    std::string txt = (prefix ? std::string(prefix) + " " : std::string()) + trim_number(*value);
    dai_ui_text(ui, x + 7.0f, y + ui->style.row_pad * 0.5f, txt.c_str(), ui->style.text);
    return changed;
}

} // namespace

namespace dai {
float ui_detail_row_x() { return g_row_x; }
float ui_detail_row_y() { return g_row_y; }
float ui_detail_row_w() { return g_row_w; }
float ui_detail_row_h() { return g_row_h; }
void ui_detail_row_clear() { g_row_w = 0.0f; g_row_h = 0.0f; }
}

int dai_ui_drag_float(dai_ui *ui, const char *label, float *value, float step) {
    if (!ui || !value) return 0;
    if (step <= 0.0f) step = 0.01f;
    float h = widget_height(ui), x, y, w;
    field_rect(ui, label, &x, &y, &w, h);
    float lx = g_label_x, ly = g_label_y, lw = g_label_w;
    int changed = drag_float_at(ui, hash_id(label ? label : "drag", x, y), x, y, w, h,
                                value, step, nullptr, 0);
    changed |= label_scrub(ui, hash_id(label ? label : "drag", lx, ly) ^ 0x9E3779B97F4A7C15ull,
                           lx, ly, lw, h, value, step, 0.0f, 0.0f);
    return changed;
}

int dai_ui_drag_vec3(dai_ui *ui, const char *label, float *xyz, float step) {
    if (!ui || !xyz) return 0;
    if (step <= 0.0f) step = 0.01f;
    float h = widget_height(ui), x, y, w;
    field_rect(ui, label, &x, &y, &w, h);
    const char *names[3] = { "X", "Y", "Z" };
    // Axis colours match the gizmo, so a field and an arm are obviously the
    // same thing.
    const uint32_t cols[3] = { rgba(230, 64, 64, 255), rgba(90, 217, 77, 255),
                               rgba(77, 128, 242, 255) };
    float gap = 4.0f;
    float each = (w - gap * 2.0f) / 3.0f;
    int changed = 0;
    for (int i = 0; i < 3; ++i) {
        float fx = x + (each + gap) * (float)i;
        uint64_t id = hash_id(label ? label : "vec", fx, y) ^ (uint64_t)(i + 1) * 0x9E3779B97F4A7C15ull;
        changed |= drag_float_at(ui, id, fx, y, each, h, &xyz[i], step, names[i], cols[i]);
    }
    return changed;
}

int dai_ui_object_field(dai_ui *ui, const char *label, const char *value, const char *icon) {
    if (!ui) return 0;
    float h = widget_height(ui), x, y, w;
    field_rect(ui, label, &x, &y, &w, h);
    // The target button on the right is the whole point: it says "there is a
    // list of these" without a manual, and it is where Unity puts it.
    float bw = h;
    float fw = w - bw - 2.0f;
    if (fw < 20.0f) fw = w;
    bool over_f = inside_chk(ui, x, y, fw, h);
    bool over_b = fw < w && inside_chk(ui, x + fw + 2.0f, y, bw, h);
    uint64_t oid = hash_id(label ? label : "objfield", x, y);
    if (over_f || over_b) {
        ui->mouse_over_ui = true;
        ui->cursor_want = DAI_CURSOR_HAND;
        // What a dragged hierarchy node asks: which field am I over?
        if (label) ui->hot_label = label;
    }
    if ((over_f || over_b) && ui->input.mouse_down && !ui->prev.mouse_down) ui->active = oid;
    int result = 0;
    if (ui->active == oid && !ui->input.mouse_down) {
        // The target button and the plate are two different questions: one
        // opens the list of everything, the other means "show me this one".
        if (over_b)      result = 2;
        else if (over_f) result = 1;
        ui->active = 0;
    }

    dai_ui_rrect(ui, x, y + 1.0f, fw, h - 2.0f, ui->style.rounding,
                 over_f ? ui->style.button_hover : ui->style.track);
    dai_ui_rect_outline(ui, x, y + 1.0f, fw, h - 2.0f, 1.0f,
                        over_f ? ui->style.accent : ui->style.panel_border);
    float tx = x + 5.0f;
    float isz = dai_icons_size(ui->icons);
    if (isz <= 0.0f || isz > h - 4.0f) isz = h - 6.0f;
    if (icon && dai_ui_has_icon(ui, icon)) {
        dai_ui_icon_at(ui, icon, tx, y + (h - isz) * 0.5f, isz, ui->style.text_dim);
        tx += isz + 4.0f;
    }
    dai_ui_text(ui, tx, y + (h - dai_font_line_height(ui->font)) * 0.5f,
                value ? value : "None", ui->style.text);

    if (fw < w) {
        float bx = x + fw + 2.0f;
        dai_ui_rrect(ui, bx, y + 1.0f, bw, h - 2.0f, ui->style.rounding,
                     over_b ? ui->style.button_hover : ui->style.button);
        if (dai_ui_has_icon(ui, "target"))
            dai_ui_icon_at(ui, "target", bx + (bw - isz) * 0.5f, y + (h - isz) * 0.5f,
                           isz, ui->style.text);
        else
            dai_ui_text(ui, bx + 4.0f, y + 2.0f, "...", ui->style.text);
    }
    return result;
}

void dai_ui_searchlist_open(dai_ui_searchlist *s, float x, float y) {
    if (!s) return;
    s->x = x; s->y = y;
    s->open = 1;
    s->highlight = 0;
    s->scroll = 0.0f;
    s->query[0] = 0;
    s->wants_focus = 1;
}

int dai_ui_searchlist_draw(dai_ui *ui, dai_ui_searchlist *s,
                           const dai_ui_menu_item *items, uint32_t count) {
    if (!ui || !s || !s->open) return DAI_SEARCHLIST_CLOSED;
    const float W = 260.0f;
    const float ROW_H = 22.0f;
    const float SEARCH_H = 26.0f;
    const float MAX_ROWS = 11.0f;

    // Keep it on screen: opening at the mouse near the bottom edge would put
    // half the list under it.
    float x = s->x, y = s->y;
    if (x + W > ui->width - 4.0f) x = ui->width - 4.0f - W;
    if (x < 4.0f) x = 4.0f;
    s->x = x;                    // remember it: a list that re-derives its
                                 // place every frame drifts with the panel
    float rows_avail = (ui->height - y - 8.0f - SEARCH_H) / ROW_H;
    float max_rows = rows_avail < MAX_ROWS ? rows_avail : MAX_ROWS;
    if (max_rows < 2.0f) max_rows = 2.0f;

    // ---- filter, and remember which row maps to which item ----------------
    int shown[128];
    uint32_t nshown = 0;
    for (uint32_t i = 0; i < count && nshown < 128; ++i) {
        if (s->query[0] && items[i].label) {
            const char *q = s->query;
            const char *l = items[i].label;
            bool hit = false;
            for (const char *c = l; *c && !hit; ++c) {
                const char *cc = c, *qq = q;
                while (*qq && *cc &&
                       ((*cc >= 'A' && *cc <= 'Z') ? *cc + 32 : *cc) ==
                       ((*qq >= 'A' && *qq <= 'Z') ? *qq + 32 : *qq)) { ++cc; ++qq; }
                if (!*qq) hit = true;
            }
            if (!hit) continue;
        }
        shown[nshown++] = (int)i;
    }
    if (s->highlight >= (int)nshown) s->highlight = (int)nshown - 1;
    if (s->highlight < 0) s->highlight = 0;

    float rows_h = (nshown < (uint32_t)max_rows ? (float)nshown : max_rows) * ROW_H;
    float H = SEARCH_H + rows_h + 6.0f;
    // Clamped against the height the list COULD have, never the height it has
    // right now. Typing filters rows away, and a clamp that follows the
    // shrinking panel walks it up the screen letter by letter - which moves
    // the search box, and a text field is identified by where it is. The
    // field changed identity under the caret, so the second character of
    // every query went nowhere: that is what "the search does not work" was.
    float H_max = SEARCH_H + max_rows * ROW_H + 6.0f;
    if (y + H_max > ui->height - 4.0f) y = ui->height - 4.0f - H_max;
    if (y < 4.0f) y = 4.0f;
    s->w = W; s->h = H;

    float mx = ui->input.mouse_x, my = ui->input.mouse_y;
    bool over_panel = mx >= x && mx < x + W && my >= y && my < y + H;
    bool pressed = ui->input.mouse_down && !ui->prev.mouse_down;

    // A click outside closes without a pick - a menu that cannot be
    // dismissed trains people to press things they did not want.
    if (pressed && !over_panel) { s->open = 0; return DAI_SEARCHLIST_CLOSED; }
    if (ui->input.key_escape)   { s->open = 0; return DAI_SEARCHLIST_CLOSED; }

    ui->popup_was_open = true;      // next frame's widgets underneath are dead
    ui->mouse_over_ui = true;
    // Keep the whole list above every panel it was opened from.
    dai_ui_layer_push(ui, DAI_LAYER_POPUP);
    dai_ui_rrect(ui, x, y, W, H, 6.0f, ui->style.panel);
    dai_ui_rect_outline(ui, x, y, W, H, 1.0f, ui->style.panel_border);

    // ---- the search field -------------------------------------------------
    if (s->wants_focus) { dai_ui_text_focus_next(ui); s->wants_focus = 0; }
    int commit = 0;
    dai_ui_text_field(ui, "searchlist", x + 6.0f, y + 4.0f, W - 12.0f, SEARCH_H - 4.0f,
                      s->query, sizeof(s->query), &commit);
    if (!s->query[0] && !dai_ui_text_active(ui))
        dai_ui_text(ui, x + 12.0f, y + 4.0f + (SEARCH_H - 4.0f - dai_font_line_height(ui->font)) * 0.5f,
                    s->hint[0] ? s->hint : "Search...", ui->style.text_dim);

    // ---- keyboard navigation ----------------------------------------------
    auto is_header = [&](int row) {
        return row >= 0 && row < (int)nshown && items[shown[row]].header != 0;
    };
    int step = 0;
    if (ui->input.key_down_arrow) { ++s->highlight; step = 1; }
    if (ui->input.key_up_arrow)   { --s->highlight; step = -1; }
    if (s->highlight < 0) s->highlight = (int)nshown - 1;
    if (s->highlight >= (int)nshown) s->highlight = 0;
    // Walk past captions rather than landing on them. Bounded by the row
    // count so a list that is nothing but headers cannot spin here.
    if (step && nshown) {
        for (uint32_t guard = 0; guard < nshown && is_header(s->highlight); ++guard) {
            s->highlight += step;
            if (s->highlight < 0) s->highlight = (int)nshown - 1;
            if (s->highlight >= (int)nshown) s->highlight = 0;
        }
    }
    if (commit && nshown && !is_header(s->highlight)) {
        int pick = shown[s->highlight];
        s->open = 0;
        dai_ui_layer_pop(ui);
        return pick;
    }

    // ---- the rows ----------------------------------------------------------
    float list_y = y + SEARCH_H + 2.0f;
    dai_ui_clip_begin(ui, x, list_y, W, rows_h + 2.0f);
    if (over_panel && mx >= x && mx < x + W && my >= list_y)
        s->scroll -= ui->input.wheel * ROW_H * 2.0f;
    float max_scroll = (float)nshown * ROW_H - rows_h;
    if (max_scroll < 0.0f) max_scroll = 0.0f;
    if (s->scroll < 0.0f) s->scroll = 0.0f;
    if (s->scroll > max_scroll) s->scroll = max_scroll;

    float ry = list_y - s->scroll;
    int result = DAI_SEARCHLIST_OPEN;
    float lh = dai_font_line_height(ui->font);
    float isz = dai_icons_size(ui->icons);
    if (isz <= 0.0f || isz > ROW_H - 4.0f) isz = ROW_H - 6.0f;
    for (uint32_t r = 0; r < nshown; ++r) {
        int idx = shown[r];
        bool head = items[idx].header != 0;
        bool over = !head && mx >= x && mx < x + W && my >= ry && my < ry + ROW_H;
        if (over) s->highlight = (int)r;
        bool hl = !head && (int)r == s->highlight;
        if (head) {
            // A caption: a rule and a quiet label, nothing that invites a
            // click. No icon either - the icon is what made it look like a row.
            dai_ui_text(ui, x + 8.0f, ry + (ROW_H - lh) * 0.5f, items[idx].label,
                        ui->style.text_dim);
            float lw = dai_ui_text_width(ui, items[idx].label);
            float rule_x = x + 14.0f + lw;
            if (rule_x < x + W - 10.0f)
                dai_ui_rect(ui, rule_x, ry + ROW_H * 0.5f, x + W - 10.0f - rule_x, 1.0f,
                            ui->style.panel_border);
            ry += ROW_H;
            continue;
        }
        if (hl) dai_ui_rect(ui, x + 2.0f, ry, W - 4.0f, ROW_H, ui->style.button_hover);
        float tx = x + 10.0f;
        if (items[idx].icon && dai_ui_has_icon(ui, items[idx].icon)) {
            dai_ui_icon_at(ui, items[idx].icon, tx, ry + (ROW_H - isz) * 0.5f, isz,
                           hl ? ui->style.text : ui->style.text_dim);
            tx += isz + 6.0f;
        }
        dai_ui_text(ui, tx, ry + (ROW_H - lh) * 0.5f, items[idx].label,
                    hl ? ui->style.text : ui->style.text_dim);
        if (over && pressed) { result = idx; s->open = 0; }
        ry += ROW_H;
    }
    if (!nshown)
        dai_ui_text(ui, x + 10.0f, list_y + 4.0f, "nothing matches", ui->style.text_dim);
    dai_ui_clip_end(ui);
    dai_ui_layer_pop(ui);
    return result;
}

// ---- the code editor ------------------------------------------------------

namespace {

// The keywords worth colouring. Deliberately short: a list that tries to be
// complete is a list that is wrong for the next language, and the value of
// syntax colour is almost entirely in "string", "comment", "everything else".
const char *const CODE_KW[] = {
    "var", "let", "const", "function", "return", "if", "else", "for", "while",
    "do", "break", "continue", "new", "delete", "typeof", "this", "null",
    "true", "false", "undefined", "switch", "case", "default", "try", "catch",
    "throw", "class", "extends", "in", "of",
    /* the C++ half, for .cpp behaviours */
    "int", "float", "double", "bool", "void", "char", "struct", "auto",
    "static", "public", "private", "namespace", "include", "define", "using",
    "nullptr", "template", "unsigned", "size_t", "const_cast"
};

bool code_is_word(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_' || c == '#';
}

bool code_is_keyword(const char *s, int n) {
    for (const char *k : CODE_KW) {
        int i = 0;
        while (i < n && k[i] && k[i] == s[i]) ++i;
        if (i == n && !k[i]) return true;
    }
    return false;
}

int code_line_start(const char *b, int off) {
    while (off > 0 && b[off - 1] != '\n') --off;
    return off;
}
int code_line_end(const char *b, int off) {
    while (b[off] && b[off] != '\n') ++off;
    return off;
}
int code_count_lines(const char *b) {
    int n = 1;
    for (const char *c = b; *c; ++c) if (*c == '\n') ++n;
    return n;
}
int code_line_of(const char *b, int off) {
    int n = 0;
    for (int i = 0; i < off && b[i]; ++i) if (b[i] == '\n') ++n;
    return n;
}
int code_offset_of_line(const char *b, int line) {
    int n = 0, i = 0;
    while (b[i] && n < line) { if (b[i] == '\n') ++n; ++i; }
    return i;
}

// The width of a run of bytes in the current font, without building a string.
float code_run_w(dai_ui *ui, const char *s, int n) {
    if (n <= 0) return 0.0f;
    char tmp[512];
    float total = 0.0f;
    while (n > 0) {
        int chunk = n < (int)sizeof(tmp) - 1 ? n : (int)sizeof(tmp) - 1;
        std::memcpy(tmp, s, (size_t)chunk);
        tmp[chunk] = 0;
        total += dai_ui_text_width(ui, tmp);
        s += chunk; n -= chunk;
    }
    return total;
}

} // namespace

void dai_ui_code_caret_pos(const char *buf, int caret, int *line, int *col) {
    if (!buf) { if (line) *line = 1; if (col) *col = 1; return; }
    int ls = code_line_start(buf, caret);
    if (line) *line = code_line_of(buf, caret) + 1;
    if (col)  *col = caret - ls + 1;
}

// The engine's own API, spelled once. It is a LIST, not a parse of the
// headers: the headers are C and the scripts are JavaScript, and a generator
// that mapped one to the other would be a build step that breaks silently the
// day someone renames a binding.
namespace {
struct AcEntry { const char *text; const char *hint; };
const AcEntry AC_JS[] = {
    { "input.key(",        "\"w\", \"space\", \"shift\" - held?" },
    { "input.mouseDX()",   "pixels moved this frame" },
    { "input.mouseDY()",   "pixels moved this frame" },
    { "input.mouseButton(","0 left, 1 right, 2 middle" },
    { "body.getVel(",      "self -> [x, y, z]" },
    { "body.setVel(",      "self, x, y, z" },
    { "body.impulse(",     "self, x, y, z" },
    { "body.grounded(",    "self -> standing on something?" },
    { "node.getPos(",      "id -> [x, y, z]" },
    { "node.setPos(",      "id, x, y, z" },
    { "node.getRot(",      "id -> [x, y, z, w]" },
    { "node.setRot(",      "id, x, y, z, w" },
    { "node.setText(",     "id, \"...\" - the Text component" },
    { "scene.find(",       "\"name\" -> id, or -1" },
    { "gui.text(",         "x, y, text, size, colour" },
    { "gui.rect(",         "x, y, w, h, colour" },
    { "gui.image(",        "x, y, w, h, path, tint" },
    { "gui.button(",       "x, y, w, h, label -> clicked?" },
    { "gui.size()",        "[width, height] of the view" },
    { "print(",            "one line into the Console" },
    { "state.dt",          "seconds this frame" },
    { "self",              "the node this script is on" },
    { "self.transform.position", "[x, y, z] of this node" },
    { "self.transform.yaw", "degrees around Y" },
    { "self.velocity",     "[x, y, z] - read and write" },
    { "self.grounded",     "standing on something?" },
    { "self.text",         "write: the Text component" },
    { "params",            "what the inspector stored" },
    { "function init() {", "runs once at Play" },
    { "function frame() {","runs every frame" },
    { "Math.sqrt(",        nullptr },
    { "Math.atan2(",       nullptr },
    { "Math.floor(",       nullptr },
    { "Math.random()",     nullptr },
};
const AcEntry AC_CPP[] = {
    { "api->log(api, ",          "one line into the Console" },
    { "api->get_position(api, ", "self -> dai_nvec3" },
    { "api->set_position(api, ", "self, dai_nvec3" },
    { "api->get_velocity(api, ", "self -> dai_nvec3" },
    { "api->set_velocity(api, ", "self, dai_nvec3" },
    { "api->add_impulse(api, ",  "self, dai_nvec3" },
    { "api->get_rotation(api, ", "self, float xyzw[4]" },
    { "api->set_rotation(api, ", "self, const float xyzw[4]" },
    { "api->get_scale(api, ",    "self -> dai_nvec3" },
    { "api->set_scale(api, ",    "self, dai_nvec3" },
    { "api->find(api, ",         "\"name\" -> entity, 0 if none" },
    { "api->name_of(api, ",      "entity -> const char *" },
    { "api->time(api)",          "seconds since Play" },
    { "api->key_down(api, ",     "DAI_KEY_* or a letter" },
    { "DAI_BEHAVIOUR_INIT(api, self) {",  "runs once at Play" },
    { "DAI_BEHAVIOUR_FRAME(api, self, dt) {", "runs every frame" },
    { "DAI_KEY_SPACE",  nullptr },
    { "DAI_KEY_SHIFT_L", nullptr },
    { "DAI_KEY_LEFT",   nullptr },
    { "DAI_KEY_RIGHT",  nullptr },
    { "DAI_KEY_UP",     nullptr },
    { "DAI_KEY_DOWN",   nullptr },
};

// What a NODE has, spelled the way the object model spells it (see the
// prelude in editor_demo.cpp). Offered after any dotted root that is not one
// of the API globals: `self.` is the common case, `player.transform.` is the
// same thing one level in, and both are the same kind of thing.
//
// This is why "self" completed and "self.transform" did not: the table only
// ever held whole names, and nothing in it began with "self.".
// The same idea for the C++ behaviours: `Node me(api, self); me.` is how
// every one of them starts, and none of those members were ever offered -
// which is exactly what "autocomplete only works in JavaScript" was.
const AcEntry AC_NODE_CPP[] = {
    { "position()",   "-> Vec3" },
    { "position(",    "Vec3 - move it" },
    { "velocity()",   "-> Vec3" },
    { "velocity(",    "Vec3 - drive it" },
    { "scale()",      "-> Vec3" },
    { "scale(",       "Vec3" },
    { "impulse(",     "Vec3 - one push" },
    { "key(",         "'w' or DAI_KEY_* - held?" },
    { "find(",        "\"name\" -> Node" },
    { "param(",       "\"name\", fallback - the inspector's value" },
    { "node(",        "\"name\" -> the Node field's target" },
    { "grounded()",   "standing on something?" },
    { "log(",         "one line into the Console" },
};

const AcEntry AC_NODE[] = {
    { "transform.position",   "[x, y, z] - and .x .y .z" },
    { "transform.position.x", "one axis; y and z stay" },
    { "transform.position.y", "one axis; x and z stay" },
    { "transform.position.z", "one axis; x and y stay" },
    { "transform.rotation",   "quaternion [x, y, z, w]" },
    { "transform.yaw",        "degrees around Y" },
    { "position",             "short for transform.position" },
    { "velocity",             "[x, y, z] - read and write" },
    { "grounded",             "standing on something?" },
    { "text",                 "write: the Text component" },
    { "impulse(",             "x, y, z - one push" },
    { "setVelocity(",         "x, y, z" },
    { "isValid()",            "does this node still exist?" },
    { "id",                   "the node's number" },
};

// The globals that are NOT nodes. Offering `Math.transform.position` would be
// noise, and noise in a completion list is what makes people turn it off.
bool ac_root_is_api(const std::string &r) {
    static const char *const API[] = { "input", "body", "node", "scene", "gui",
                                       "state", "params", "Math", "JSON",
                                       "console", "ui", "Object", "Array" };
    for (const char *a : API) if (r == a) return true;
    return false;
}

// One offered completion: the text that gets inserted plus its hint. A string
// and not a table pointer, because a member completion is BUILT ("self" + "."
// + "transform.position") and does not exist in any table.
struct AcHit { std::string text; const char *hint; };

// Every identifier already in the file, so a variable you declared three
// lines up can be completed too. Cheap enough at every keystroke: a behaviour
// is a few hundred lines, and this is a single pass over them.
void ac_identifiers(const char *buf, const std::string &prefix, int skip_at,
                    std::vector<std::string> &out) {
    if (prefix.empty()) return;
    int i = 0;
    while (buf[i]) {
        if (!code_is_word(buf[i]) || (buf[i] >= '0' && buf[i] <= '9')) { ++i; continue; }
        int a = i;
        while (buf[i] && code_is_word(buf[i])) ++i;
        if (a == skip_at) continue;                 // the word being typed
        std::string word(buf + a, buf + i);
        if (word.size() <= prefix.size()) continue;
        // Case sensitive: JavaScript is, C++ is, and a completion that
        // changes the case of what you typed is a completion you retype.
        if (word.compare(0, prefix.size(), prefix) != 0) continue;
        bool have = false;
        for (const std::string &e : out) if (e == word) { have = true; break; }
        if (!have) out.push_back(word);
        if (out.size() > 40) return;
    }
}
} // namespace

int dai_ui_code_edit(dai_ui *ui, const char *id, float x, float y, float w, float h,
                     char *buf, size_t buf_size, dai_ui_code_state *st, int lang) {
    if (!ui || !buf || !st || buf_size < 2 || w < 40.0f || h < 20.0f) return 0;
    (void)lang;
    const dai_ui_style *sty = &ui->style;
    int len = (int)std::strlen(buf);
    if (st->caret > len) st->caret = len;
    if (st->anchor > len) st->anchor = len;
    if (st->caret < 0) st->caret = 0;
    if (st->anchor < 0) st->anchor = 0;

    const float LH = dai_font_line_height(ui->font) + 2.0f;
    int nlines = code_count_lines(buf);
    char gut[16];
    std::snprintf(gut, sizeof(gut), "%d", nlines < 100 ? 100 : nlines);
    // A plain multi-line field is the same editor with the machinery off:
    // line numbers in a Text component would be nonsense.
    const float GUT = st->plain ? 0.0f : dai_ui_text_width(ui, gut) + 14.0f;
    const float TEXT_X = x + GUT + 6.0f;
    const float VIEW_W = w - GUT - 10.0f;

    uint64_t wid = hash_id(id ? id : "code", x, y);
    float mx = ui->input.mouse_x, my = ui->input.mouse_y;
    bool over = inside_chk(ui, x, y, w, h);
    bool pressed = ui->input.mouse_down && !ui->prev.mouse_down;
    if (over) ui->mouse_over_ui = true;

    // ---- focus ------------------------------------------------------------
    if (st->want_focus) { st->focused = 1; st->want_focus = 0; }
    if (pressed) st->focused = over ? 1 : 0;
    if (st->focused) ui->code_focus = 1;

    // ---- the plate ---------------------------------------------------------
    dai_ui_rect(ui, x, y, w, h, sty->track);
    if (!st->plain) {
        dai_ui_rect(ui, x, y, GUT, h, (sty->chrome & 0x00FFFFFFu) | 0xFF000000u);
        dai_ui_rect(ui, x + GUT, y, 1.0f, h, sty->panel_border);
    }
    dai_ui_rect_outline(ui, x, y, w, h, 1.0f,
                        st->focused ? sty->accent : sty->panel_border);

    // ---- where is the caret, in pixels -------------------------------------
    auto caret_xy = [&](int off, float *ox, float *oy) {
        int ls = code_line_start(buf, off);
        *ox = TEXT_X + code_run_w(ui, buf + ls, off - ls);
        *oy = y + 4.0f + (float)code_line_of(buf, off) * LH;
    };
    // ...and the reverse: which offset is under this point.
    auto offset_at = [&](float px, float py) {
        int line = (int)((py - (y + 4.0f) + st->scroll_y) / LH);
        if (line < 0) line = 0;
        if (line > nlines - 1) line = nlines - 1;
        int ls = code_offset_of_line(buf, line);
        int le = code_line_end(buf, ls);
        float want = px - TEXT_X + st->scroll_x;
        if (want <= 0) return ls;
        int best = ls;
        float acc = 0.0f;
        for (int i = ls; i < le; ++i) {
            char one[2] = { buf[i], 0 };
            float cw = dai_ui_text_width(ui, one);
            if (acc + cw * 0.5f > want) return best;
            acc += cw;
            best = i + 1;
        }
        return le;
    };

    // ---- mouse: caret and selection ----------------------------------------
    if (pressed && over && mx > x + GUT) {
        st->caret = st->anchor = offset_at(mx, my);
        st->dragging = 1;
        st->follow_caret = 1;          // a click is a place you asked to be
        st->focused = 1;
        dai_ui_claim_mouse(ui);
        ui->active = wid;
        // A double click takes the word under it - the one selection gesture
        // everybody uses without being taught.
        if (ui->input.double_click) {
            int a = st->caret, b = st->caret;
            while (a > 0 && code_is_word(buf[a - 1])) --a;
            while (buf[b] && code_is_word(buf[b])) ++b;
            st->anchor = a; st->caret = b;
            st->dragging = 0;
        }
    }
    if (st->dragging) {
        if (ui->input.mouse_down) {
            st->caret = offset_at(mx, my);
            dai_ui_claim_mouse(ui);
        } else st->dragging = 0;
    }
    bool user_scrolled = false;
    if (over) {
        ui->cursor_want = DAI_CURSOR_TEXT;
        if (ui->input.wheel != 0.0f) {
            st->scroll_y -= ui->input.wheel * LH * 3.0f;
            user_scrolled = true;
        }
    }

    // ---- keyboard -----------------------------------------------------------
    int changed = 0;
    // Set by the handlers below when they move the caret on purpose. This is
    // the only thing that makes the view follow it.
    bool caret_input = false;
    auto sel_lo = [&]() { return st->caret < st->anchor ? st->caret : st->anchor; };
    auto sel_hi = [&]() { return st->caret > st->anchor ? st->caret : st->anchor; };
    auto erase = [&](int a, int b) {
        if (b <= a) return;
        std::memmove(buf + a, buf + b, (size_t)(len - b + 1));
        len -= (b - a);
        st->caret = st->anchor = a;
        changed = 1;
    };
    auto insert = [&](const char *txt, int n) {
        if (n <= 0) return;
        if (sel_hi() > sel_lo()) erase(sel_lo(), sel_hi());
        if ((size_t)(len + n + 1) > buf_size) return;
        std::memmove(buf + st->caret + n, buf + st->caret, (size_t)(len - st->caret + 1));
        std::memcpy(buf + st->caret, txt, (size_t)n);
        len += n;
        st->caret += n;
        st->anchor = st->caret;
        changed = 1;
    };

    int ac_take = 0;
    if (st->focused) {
        dai_ui_input in = ui->input;      // a COPY: see the note above ac_take
        bool shift = in.key_shift != 0;
        int before = st->caret;
        if (in.text[0] || in.key_enter || in.key_tab || in.key_backspace ||
            in.key_delete || in.key_left || in.key_right || in.key_home ||
            in.key_end || in.key_up_arrow || in.key_down_arrow ||
            in.key_paste || in.key_cut)
            caret_input = true;

        // ---- autocomplete: the keys it owns --------------------------------
        // Taken BEFORE the editor's own handling, because up, down, tab and
        // escape mean something else while a list is open - and a completion
        // list that you cannot dismiss with escape is a trap.
        if (st->ac_open > 0) {
            // Escape means "not now". It set ac_open to 0 and the list was
            // rebuilt from scratch at the end of the same frame, so it came
            // straight back - the key did nothing you could see. The refusal
            // has to be REMEMBERED until the word being typed changes.
            if (in.key_escape) { st->ac_open = 0; st->ac_off = 1; caret_input = true; }
            else if (in.key_up_arrow)   { if (--st->ac_sel < 0) st->ac_sel = st->ac_open - 1; }
            else if (in.key_down_arrow) { if (++st->ac_sel >= st->ac_open) st->ac_sel = 0; }
            else if (in.key_tab || in.key_enter) {
                ac_take = 1;                      // applied below, where the list is built
            }
            if (in.key_up_arrow || in.key_down_arrow || in.key_tab || in.key_enter || in.key_escape) {
                // Swallow them: the editor must not also move the caret.
                in.key_up_arrow = in.key_down_arrow = in.key_tab = in.key_enter = 0;
                in.key_escape = 0;
            }
        }

        for (int i = 0; i < 8 && in.text[i]; ++i) {
            uint32_t cp = in.text[i];
            if (cp < 0x20 || cp == 0x7F) continue;
            char utf[4];
            int n = 0;
            if (cp < 0x80) { utf[0] = (char)cp; n = 1; }
            else if (cp < 0x800) { utf[0] = (char)(0xC0 | (cp >> 6)); utf[1] = (char)(0x80 | (cp & 0x3F)); n = 2; }
            else { utf[0] = (char)(0xE0 | (cp >> 12)); utf[1] = (char)(0x80 | ((cp >> 6) & 0x3F)); utf[2] = (char)(0x80 | (cp & 0x3F)); n = 3; }
            insert(utf, n);
        }
        if (in.key_enter) {
            // Auto-indent: a new line starts where the old one's text starts.
            // Without it every block has to be re-indented by hand, and an
            // editor that fights the shape of the code is not used twice.
            int ls = code_line_start(buf, st->caret);
            char pad[64];
            int np = 0;
            while (ls + np < len && np < 60 && (buf[ls + np] == ' ' || buf[ls + np] == '\t'))
                { pad[np + 1] = buf[ls + np]; ++np; }
            pad[0] = '\n';
            insert(pad, np + 1);
        }
        if (in.key_tab) insert("    ", 4);
        if (in.key_backspace) {
            if (sel_hi() > sel_lo()) erase(sel_lo(), sel_hi());
            else if (st->caret > 0) {
                int a = st->caret - 1;
                while (a > 0 && ((unsigned char)buf[a] & 0xC0) == 0x80) --a;  // whole code point
                erase(a, st->caret);
            }
        }
        if (in.key_delete) {
            if (sel_hi() > sel_lo()) erase(sel_lo(), sel_hi());
            else if (st->caret < len) {
                int b = st->caret + 1;
                while (b < len && ((unsigned char)buf[b] & 0xC0) == 0x80) ++b;
                erase(st->caret, b);
            }
        }
        if (in.key_left && st->caret > 0) {
            --st->caret;
            while (st->caret > 0 && ((unsigned char)buf[st->caret] & 0xC0) == 0x80) --st->caret;
        }
        if (in.key_right && st->caret < len) {
            ++st->caret;
            while (st->caret < len && ((unsigned char)buf[st->caret] & 0xC0) == 0x80) ++st->caret;
        }
        if (in.key_home) st->caret = code_line_start(buf, st->caret);
        if (in.key_end)  st->caret = code_line_end(buf, st->caret);
        if (in.key_up_arrow || in.key_down_arrow) {
            int line = code_line_of(buf, st->caret);
            int ls = code_line_start(buf, st->caret);
            float want = code_run_w(ui, buf + ls, st->caret - ls);
            int tgt = line + (in.key_down_arrow ? 1 : -1);
            if (tgt < 0) tgt = 0;
            if (tgt > nlines - 1) tgt = nlines - 1;
            int ts = code_offset_of_line(buf, tgt), te = code_line_end(buf, ts);
            int best = ts;
            float acc = 0.0f;
            for (int i = ts; i < te; ++i) {
                char one[2] = { buf[i], 0 };
                float cw = dai_ui_text_width(ui, one);
                if (acc + cw * 0.5f > want) break;
                acc += cw;
                best = i + 1;
            }
            st->caret = best;
        }
        // Ctrl+C / Ctrl+X / Ctrl+V. The characters arrive as text events too
        // when Ctrl is held on some layouts, which is why the insert loop
        // above skips anything below 0x20 and these are checked on the KEY.
        {
            int lo = sel_lo(), hi = sel_hi();
            bool has_sel = hi > lo;
            if ((in.key_copy || in.key_cut) && has_sel) {
                ui->clip_out.assign(buf + lo, (size_t)(hi - lo));
                ui->clip_out_set = true;
                if (in.key_cut) erase(lo, hi);
            }
            if (in.key_paste && !ui->clip_in.empty()) {
                // Line endings normalised on the way in: a paste from a
                // Windows editor otherwise carries a carriage return into
                // every line, and every one of them draws as a glyph.
                std::string t;
                t.reserve(ui->clip_in.size());
                for (char ch : ui->clip_in) if (ch != '\r') t += ch;
                insert(t.c_str(), (int)t.size());
            }
        }
        if (in.key_select_all) { st->anchor = 0; st->caret = len; }
        else if (st->caret != before && !shift) st->anchor = st->caret;
        else if (st->caret != before && shift) { /* anchor stays: that IS a selection */ }
        if (changed) st->anchor = st->caret;
    }

    // ---- keep the caret in view --------------------------------------------
    // Only when it MOVED, or when the text under it changed. Doing this
    // unconditionally is why the wheel did nothing: scroll away, and the very
    // next frame pulled the view back onto the caret.
    if (caret_input || changed) st->follow_caret = 1;
    if (user_scrolled) st->follow_caret = 0;   // the wheel has the last word
    st->last_caret = st->caret;
    st->have_last = 1;
    if (st->follow_caret) {
        st->follow_caret = 0;
        float cx, cy;
        caret_xy(st->caret, &cx, &cy);
        float rel_y = cy - (y + 4.0f);
        if (rel_y - st->scroll_y < 0.0f) st->scroll_y = rel_y;
        if (rel_y - st->scroll_y > h - LH - 8.0f) st->scroll_y = rel_y - (h - LH - 8.0f);
        float rel_x = cx - TEXT_X;
        if (rel_x - st->scroll_x < 0.0f) st->scroll_x = rel_x;
        if (rel_x - st->scroll_x > VIEW_W - 20.0f) st->scroll_x = rel_x - (VIEW_W - 20.0f);
    }
    float max_y = (float)nlines * LH - (h - 8.0f);
    if (max_y < 0.0f) max_y = 0.0f;
    if (st->scroll_y > max_y) st->scroll_y = max_y;
    if (st->scroll_y < 0.0f) st->scroll_y = 0.0f;
    if (st->scroll_x < 0.0f) st->scroll_x = 0.0f;

    // ---- draw ---------------------------------------------------------------
    const uint32_t C_TEXT    = sty->text;
    const uint32_t C_COMMENT = rgba(0x6A, 0x9B, 0x6A, 255);
    const uint32_t C_STRING  = rgba(0xCE, 0x9A, 0x63, 255);
    const uint32_t C_NUMBER  = rgba(0xB5, 0xCE, 0xA8, 255);
    const uint32_t C_KEYWORD = rgba(0x86, 0xB3, 0xE8, 255);
    const uint32_t C_LINENO  = sty->text_dim;

    dai_ui_clip_begin(ui, x + 1.0f, y + 1.0f, w - 2.0f, h - 2.0f);
    int first_line = (int)(st->scroll_y / LH);
    if (first_line < 0) first_line = 0;
    int last_line = first_line + (int)(h / LH) + 2;
    if (last_line > nlines) last_line = nlines;
    int caret_line = code_line_of(buf, st->caret);
    int lo = sel_lo(), hi = sel_hi();

    // A block comment can start on a line above the first one drawn, so the
    // scan begins at the top of the file. Only the state is carried, not the
    // drawing - this costs one pass over the bytes and nothing else.
    bool in_block = false;
    {
        int upto = code_offset_of_line(buf, first_line);
        for (int i = 0; i + 1 < upto; ++i) {
            if (!in_block && buf[i] == '/' && buf[i + 1] == '*') { in_block = true; ++i; }
            else if (in_block && buf[i] == '*' && buf[i + 1] == '/') { in_block = false; ++i; }
        }
    }

    for (int ln = first_line; ln < last_line; ++ln) {
        float ry = y + 4.0f + (float)ln * LH - st->scroll_y;
        int ls = code_offset_of_line(buf, ln), le = code_line_end(buf, ls);

        if (ln == caret_line && st->focused)
            dai_ui_rect(ui, x + GUT + 1.0f, ry - 1.0f, w - GUT - 2.0f, LH,
                        (sty->accent & 0x00FFFFFFu) | 0x18000000u);

        char nb[16];
        std::snprintf(nb, sizeof(nb), "%d", ln + 1);
        float nw = dai_ui_text_width(ui, nb);
        if (!st->plain) dai_ui_text(ui, x + GUT - 8.0f - nw, ry, nb,
                    ln == caret_line ? sty->text : C_LINENO);

        // selection band
        if (hi > lo && hi > ls && lo <= le) {
            int a = lo > ls ? lo : ls, b = hi < le ? hi : le;
            float ax = TEXT_X + code_run_w(ui, buf + ls, a - ls) - st->scroll_x;
            float bw = code_run_w(ui, buf + a, b - a);
            if (b == le && hi > le) bw += 6.0f;      // the newline, visibly
            dai_ui_rect(ui, ax, ry - 1.0f, bw, LH, (sty->accent & 0x00FFFFFFu) | 0x66000000u);
        }

        // ---- one line, token by token -------------------------------------
        float tx = TEXT_X - st->scroll_x;
        int i = ls;
        while (i < le) {
            int start = i;
            uint32_t col = C_TEXT;
            if (in_block) {
                while (i < le && !(buf[i] == '*' && i + 1 < le && buf[i + 1] == '/')) ++i;
                if (i < le) { i += 2; in_block = false; }
                col = C_COMMENT;
            } else if (buf[i] == '/' && i + 1 < le && buf[i + 1] == '/') {
                i = le; col = C_COMMENT;
            } else if (buf[i] == '/' && i + 1 < le && buf[i + 1] == '*') {
                in_block = true; i += 2;
                while (i < le && !(buf[i] == '*' && i + 1 < le && buf[i + 1] == '/')) ++i;
                if (i < le) { i += 2; in_block = false; }
                col = C_COMMENT;
            } else if (buf[i] == '"' || buf[i] == '\'') {
                char q = buf[i++];
                while (i < le && buf[i] != q) { if (buf[i] == '\\' && i + 1 < le) ++i; ++i; }
                if (i < le) ++i;
                col = C_STRING;
            } else if (buf[i] >= '0' && buf[i] <= '9') {
                while (i < le && ((buf[i] >= '0' && buf[i] <= '9') || buf[i] == '.')) ++i;
                col = C_NUMBER;
            } else if (code_is_word(buf[i])) {
                while (i < le && code_is_word(buf[i])) ++i;
                col = code_is_keyword(buf + start, i - start) ? C_KEYWORD : C_TEXT;
            } else {
                ++i;
                col = C_TEXT;
            }
            int n = i - start;
            if (n > 0 && tx < x + w) {
                char tmp[512];
                int cn = n < (int)sizeof(tmp) - 1 ? n : (int)sizeof(tmp) - 1;
                std::memcpy(tmp, buf + start, (size_t)cn);
                tmp[cn] = 0;
                if (tx + code_run_w(ui, tmp, cn) > x + GUT)   // skip what is left of view
                    dai_ui_text(ui, tx, ry, tmp, col);
                tx += code_run_w(ui, tmp, cn);
            }
        }
    }

    // the caret itself
    if (st->focused) {
        st->blink += 1.0f / 60.0f;
        if (st->blink > 1.06f) st->blink = 0.0f;
        if (st->blink < 0.66f) {
            float cx, cy;
            caret_xy(st->caret, &cx, &cy);
            dai_ui_rect(ui, cx - st->scroll_x, cy - st->scroll_y - 1.0f, 1.5f, LH, sty->text);
        }
    }
    dai_ui_clip_end(ui);

    // ---- autocomplete ------------------------------------------------------
    // Built here, at the end, because it depends on the word under the caret
    // AFTER this frame's typing. The list is what the engine offers plus what
    // the file already contains, and nothing else: no parse, no types, no
    // guessing what an expression evaluates to. A half parser is wrong on
    // exactly the lines you are in the middle of writing.
    {
        // The word being typed: letters back from the caret. A completion
        // that triggers on one character would pop up on every `i`.
        int a = st->caret;
        while (a > 0 && code_is_word(buf[a - 1])) --a;
        std::string prefix(buf + a, buf + st->caret);
        // A dotted call is one word for this purpose: typing "input.k" should
        // offer input.key, and the dot is not a word character.
        // Walk back over EVERY `word.` in front of the caret, not just one:
        // `self.transform.pos` has to be matched whole. Stopping after the
        // first dot is why typing `self.` offered nothing - the prefix became
        // "self." and no entry in the table ever started with that, and
        // `self.transform.` became "transform." which matched nothing either.
        while (true) {
            if (a > 0 && buf[a - 1] == '.') {
                int b = a - 1;
                while (b > 0 && code_is_word(buf[b - 1])) --b;
                if (b == a - 1) break;                  // a lone dot, not a chain
                a = b;
            } else if (a > 1 && buf[a - 1] == '>' && buf[a - 2] == '-') {
                int b = a - 2;
                while (b > 0 && code_is_word(buf[b - 1])) --b;
                if (b == a - 2) break;
                a = b;
            } else {
                break;
            }
        }
        prefix = std::string(buf + a, buf + st->caret);
        // 1.5 is a number, not a chain, and must not complete to anything.
        if (!prefix.empty() && prefix[0] >= '0' && prefix[0] <= '9') {
            a = st->caret;
            prefix.clear();
        }

        std::vector<AcHit> hits;
        std::vector<std::string> words;
        // ONE character is enough. Two meant the list never appeared for the
        // thing you were most likely to want it for - `a`, `s`, `me.` - and a
        // completion you have to earn is one people stop waiting for.
        // Typing anything new is a new question, so the refusal expires.
        if (st->ac_off && (int)prefix.size() != st->ac_off_len) st->ac_off = 0;
        st->ac_off_len = (int)prefix.size();
        if (st->focused && !st->ac_off && prefix.size() >= 1 && lang != DAI_CODE_LANG_NONE) {
            const AcEntry *table = lang == DAI_CODE_LANG_CPP ? AC_CPP : AC_JS;
            size_t count = lang == DAI_CODE_LANG_CPP
                         ? sizeof(AC_CPP) / sizeof(AC_CPP[0])
                         : sizeof(AC_JS) / sizeof(AC_JS[0]);
            for (size_t i = 0; i < count; ++i)
                if (std::strncmp(table[i].text, prefix.c_str(), prefix.size()) == 0)
                    hits.push_back(AcHit{ table[i].text, table[i].hint });
            // Members of whatever is left of the first dot. No types are
            // known here and none are guessed: it is a list of names, and a
            // name that does not apply is one Escape away.
            size_t dot = prefix.find('.');
            if (dot != std::string::npos) {
                std::string root = prefix.substr(0, dot);
                std::string rest = prefix.substr(dot + 1);
                if (!root.empty() && !ac_root_is_api(root)) {
                    const AcEntry *mt = lang == DAI_CODE_LANG_CPP ? AC_NODE_CPP : AC_NODE;
                    size_t mn = lang == DAI_CODE_LANG_CPP
                              ? sizeof(AC_NODE_CPP) / sizeof(AC_NODE_CPP[0])
                              : sizeof(AC_NODE) / sizeof(AC_NODE[0]);
                    for (size_t mi = 0; mi < mn; ++mi) {
                        if (std::strncmp(mt[mi].text, rest.c_str(), rest.size()) != 0) continue;
                        std::string full = root + "." + mt[mi].text;
                        bool dup = false;
                        for (const AcHit &hh2 : hits) if (hh2.text == full) { dup = true; break; }
                        if (!dup) hits.push_back(AcHit{ full, mt[mi].hint });
                    }
                }
            }
            ac_identifiers(buf, prefix, a, words);
        }
        int total = (int)hits.size() + (int)words.size();
        if (total > 8) total = 8;
        st->ac_open = total;
        st->ac_start = a;
        if (st->ac_sel >= total) st->ac_sel = 0;
        if (st->ac_sel < 0) st->ac_sel = 0;

        if (total > 0 && ac_take) {
            const char *pick = st->ac_sel < (int)hits.size()
                             ? hits[(size_t)st->ac_sel].text.c_str()
                             : words[(size_t)(st->ac_sel - (int)hits.size())].c_str();
            // Replace the typed prefix, do not append to it.
            int plen = st->caret - a;
            if (plen > 0) {
                std::memmove(buf + a, buf + st->caret, (size_t)(len - st->caret + 1));
                len -= plen;
                st->caret = a;
                st->anchor = a;
            }
            int n = (int)std::strlen(pick);
            if ((size_t)(len + n + 1) <= buf_size) {
                std::memmove(buf + st->caret + n, buf + st->caret, (size_t)(len - st->caret + 1));
                std::memcpy(buf + st->caret, pick, (size_t)n);
                len += n;
                st->caret += n;
                st->anchor = st->caret;
                changed = 1;
            }
            st->ac_open = 0;
            st->follow_caret = 1;
        } else if (total > 0) {
            // Under the caret, or above it when there is no room below - a
            // list that falls off the bottom of the panel is a list you
            // cannot read the last entry of.
            float cx2, cy2;
            {
                int ls = code_line_start(buf, a);
                cx2 = TEXT_X + code_run_w(ui, buf + ls, a - ls) - st->scroll_x;
                cy2 = y + 4.0f + (float)code_line_of(buf, a) * LH - st->scroll_y;
            }
            const float RH = dai_font_line_height(ui->font) + 4.0f;
            float lw = 180.0f;
            for (const AcHit &e : hits) {
                float tw2 = dai_ui_text_width(ui, e.text.c_str()) +
                            (e.hint ? dai_ui_text_width(ui, e.hint) + 24.0f : 0.0f) + 24.0f;
                if (tw2 > lw) lw = tw2;
            }
            if (lw > w - 20.0f) lw = w - 20.0f;
            float lh2 = RH * (float)total + 4.0f;
            float ly = cy2 + LH + 2.0f;
            if (ly + lh2 > y + h) ly = cy2 - lh2 - 2.0f;
            if (ly < y) ly = y;
            float lx = cx2;
            if (lx + lw > x + w - 4.0f) lx = x + w - 4.0f - lw;
            if (lx < x + 2.0f) lx = x + 2.0f;

            dai_ui_layer_push(ui, DAI_LAYER_POPUP);
            dai_ui_rrect(ui, lx, ly, lw, lh2, 4.0f, sty->panel);
            dai_ui_rect_outline(ui, lx, ly, lw, lh2, 1.0f, sty->accent);
            for (int i = 0; i < total; ++i) {
                float ry = ly + 2.0f + RH * (float)i;
                bool on = i == st->ac_sel;
                if (on) dai_ui_rect(ui, lx + 1.0f, ry, lw - 2.0f, RH, sty->button_active);
                const char *label = i < (int)hits.size() ? hits[(size_t)i].text.c_str()
                                                         : words[(size_t)(i - (int)hits.size())].c_str();
                const char *hint = i < (int)hits.size() ? hits[(size_t)i].hint : "in this file";
                dai_ui_text(ui, lx + 8.0f, ry + 2.0f, label, on ? 0xFFFFFFFFu : sty->text);
                if (hint) {
                    float hw2 = dai_ui_text_width(ui, hint);
                    if (lx + 8.0f + dai_ui_text_width(ui, label) + 12.0f + hw2 < lx + lw - 6.0f)
                        dai_ui_text(ui, lx + lw - 6.0f - hw2, ry + 2.0f, hint, sty->text_dim);
                }
            }
            dai_ui_layer_pop(ui);
        }
    }

    return changed;
}

int dai_ui_option(dai_ui *ui, const char *label, int *value,
                  const char *const *items, int count) {
    if (!ui || !value || !items || count <= 0) return 0;
    float h = widget_height(ui), x, y, w;
    field_rect(ui, label, &x, &y, &w, h);
    return dai_ui_option_at(ui, label ? label : "opt", x, y, w, h, value, items, count);
}

int dai_ui_option_at(dai_ui *ui, const char *id_str, float x, float y, float w, float h,
                     int *value, const char *const *items, int count) {
    if (!ui || !value || !items || count <= 0) return 0;
    uint64_t id = hash_id(id_str ? id_str : "opt", x, y);
    if (*value < 0) *value = 0;
    if (*value >= count) *value = count - 1;

    bool open = (ui->popup_open_id == id);
    bool over = open ? inside(ui, x, y, w, h) : inside_chk(ui, x, y, w, h);
    if (over) { ui->hot = id; ui->mouse_over_ui = true; }
    int changed = 0;
    bool pressed = ui->input.mouse_down && !ui->prev.mouse_down;

    if (over && pressed && !open) {
        // Opening is a state change that has to survive the frame - which is
        // the whole reason the old version was a click-to-cycle button: an
        // immediate mode widget with no retained state cannot stay open.
        ui->popup_open_id = id;
        ui->popup_nav = *value;
        ui->popup_opened_frame_id = id;
        open = true;
    }

    // ---- the closed field
    dai_ui_rrect(ui, x, y + 1.0f, w, h - 2.0f, ui->style.rounding,
                 open ? ui->style.button_hover : (over ? ui->style.button_hover : ui->style.track));
    dai_ui_rect_outline(ui, x, y + 1.0f, w, h - 2.0f, 1.0f,
                        open ? ui->style.accent : ui->style.panel_border);
    dai_ui_text(ui, x + 6.0f, y + (h - dai_font_line_height(ui->font)) * 0.5f,
                items[*value], ui->style.text);
    {   // the arrow, drawn: the font has no U+25BE, and a missing glyph is
        // what put a '?' in every dropdown in this editor for weeks
        float cx = x + w - 11.0f, cy = y + h * 0.5f;
        dai_ui_line(ui, cx - 3.5f, cy - 2.0f, cx, cy + 2.0f, 1.5f, ui->style.text_dim);
        dai_ui_line(ui, cx, cy + 2.0f, cx + 3.5f, cy - 2.0f, 1.5f, ui->style.text_dim);
    }
    if (!open) return 0;

    // ---- the open list: its own root, its own layer, above everything
    float row = dai_font_line_height(ui->font) + 6.0f;
    float lh = row * (float)count + 4.0f;
    float lx = x, ly = y + h;
    float lw = w < 90.0f ? 90.0f : w;
    for (int i = 0; i < count; ++i) {
        float tw = dai_ui_text_width(ui, items[i]) + 28.0f;
        if (tw > lw) lw = tw;
    }
    if (ly + lh > ui->height) {           // no room below: flip above the field
        float above = y - lh;
        ly = above >= 0.0f ? above : ui->height - lh - 2.0f;
    }
    if (lx + lw > ui->width) lx = ui->width - lw - 2.0f;
    if (lx < 0) lx = 0;
    if (ly < 0) ly = 0;

    int save_layer = ui->cur_layer;
    std::vector<dai_ui::Clip> save_clips;
    save_clips.swap(ui->clips);                 // a list is never clipped by its panel
    bool save_blocked = ui->blocked;
    bool save_in_popup = ui->in_popup;
    ui->blocked = false;
    ui->in_popup = false;
    ui->cur_layer = DAI_LAYER_POPUP;
    dai_ui_root_begin(ui, "##dropdown", lx, ly, lw, lh);

    dai_ui_rect(ui, lx + 2.0f, ly + 3.0f, lw, lh, ui->style.shadow);
    dai_ui_rect(ui, lx, ly, lw, lh, ui->style.panel);
    dai_ui_rect_outline(ui, lx, ly, lw, lh, 1.0f, ui->style.accent);

    float mx = ui->input.mouse_x, my = ui->input.mouse_y;
    bool over_list = mx >= lx && mx < lx + lw && my >= ly && my < ly + lh;
    int hovered = over_list ? (int)((my - ly - 2.0f) / row) : -1;
    if (hovered >= count) hovered = -1;
    if (hovered >= 0) ui->popup_nav = hovered;

    // Keyboard: the highlight moves, the VALUE does not - committing on every
    // arrow press is the cycling behaviour again, just with a list drawn.
    if (ui->input.key_down_arrow || ui->input.key_up_arrow) {
        ui->popup_nav += ui->input.key_down_arrow ? 1 : -1;
        if (ui->popup_nav < 0) ui->popup_nav = 0;
        if (ui->popup_nav >= count) ui->popup_nav = count - 1;
    }
    if (ui->input.key_home) ui->popup_nav = 0;
    if (ui->input.key_end)  ui->popup_nav = count - 1;

    for (int i = 0; i < count; ++i) {
        float ry = ly + 2.0f + row * (float)i;
        bool sel = (i == *value);
        if (i == ui->popup_nav)
            dai_ui_rect(ui, lx + 1.0f, ry, lw - 2.0f, row, ui->style.accent);
        if (sel) {   // a tick on the current value, like every real dropdown
            float cx = lx + 8.0f, cy = ry + row * 0.5f;
            dai_ui_line(ui, cx - 3.0f, cy, cx - 1.0f, cy + 3.0f, 1.6f, ui->style.text);
            dai_ui_line(ui, cx - 1.0f, cy + 3.0f, cx + 4.0f, cy - 3.5f, 1.6f, ui->style.text);
        }
        dai_ui_text(ui, lx + 18.0f, ry + 3.0f, items[i], ui->style.text);
    }

    int commit = -1;
    if (pressed && ui->popup_opened_frame_id != id) {
        if (hovered >= 0) commit = hovered;
        else if (!over_list) { ui->popup_open_id = 0; }   // click outside = cancel
    }
    if (ui->input.key_enter && ui->popup_nav >= 0) commit = ui->popup_nav;
    if (ui->input.key_escape) ui->popup_open_id = 0;
    if (commit >= 0) {
        if (commit != *value) { *value = commit; changed = 1; }
        ui->popup_open_id = 0;
    }

    dai_ui_root_end(ui);
    ui->cur_layer = save_layer;
    ui->clips.swap(save_clips);
    ui->blocked = save_blocked;
    ui->in_popup = save_in_popup;
    ui->mouse_over_ui = true;
    if (ui->popup_open_id == id) ui->popup_was_open = true;   // blocks everyone else
    ui->popup_opened_frame_id = 0;
    return changed;
}

} // extern "C"

namespace {

// ---- the shared text editor ---------------------------------------------
//
// One caret, one selection, one buffer - reused by every field on screen. The
// rules are the ones every other text box on the machine follows, which is the
// whole point: an inspector field that needs a special trick to fix a typo is
// a field people retype from scratch.
//
//   click            focus and select ALL (so typing replaces the value)
//   click again      put the caret where you clicked
//   drag             select a range
//   double click     select all again
//   arrows/Home/End  move, +Shift extends the selection
//   Ctrl+A           select all
//   Backspace/Del    the selection, or one code point
//   Enter / Tab      commit;  Escape  cancel;  click away  commit

size_t utf8_prev(const char *s, size_t i) {
    if (i == 0) return 0;
    size_t n = 1;
    while (n < i && ((unsigned char)s[i - n] & 0xC0) == 0x80) ++n;
    return i - n;
}
size_t utf8_next(const char *s, size_t len, size_t i) {
    if (i >= len) return len;
    size_t n = 1;
    while (i + n < len && ((unsigned char)s[i + n] & 0xC0) == 0x80) ++n;
    return i + n;
}

float text_w_n(dai_ui *ui, const char *s, size_t n) {
    char tmp[160];
    if (n >= sizeof(tmp)) n = sizeof(tmp) - 1;
    std::memcpy(tmp, s, n);
    tmp[n] = 0;
    return dai_ui_text_width(ui, tmp);
}

// The byte offset whose x is closest to mx. Measured with the real font, not
// with an average character width, or the caret lands a letter off on any
// proportional font - which is every font.
uint32_t caret_at(dai_ui *ui, const char *s, float x0, float mx) {
    size_t len = std::strlen(s);
    size_t best = 0;
    float bestd = 1e9f;
    for (size_t i = 0;; ) {
        float d = std::fabs(mx - (x0 + text_w_n(ui, s, i)));
        if (d < bestd) { bestd = d; best = i; }
        if (i >= len) break;
        i = utf8_next(s, len, i);
    }
    return (uint32_t)best;
}

void edit_selection(const dai_ui *ui, uint32_t *lo, uint32_t *hi) {
    uint32_t a = ui->edit.cursor, b = ui->edit.anchor;
    *lo = a < b ? a : b;
    *hi = a < b ? b : a;
}

bool edit_erase_selection(dai_ui *ui) {
    uint32_t lo, hi;
    edit_selection(ui, &lo, &hi);
    if (hi <= lo) return false;
    char *b = ui->edit.buf;
    size_t len = std::strlen(b);
    std::memmove(b + lo, b + hi, len - hi + 1);
    ui->edit.cursor = ui->edit.anchor = lo;
    return true;
}

// Numeric fields accept digits, ONE leading sign and ONE dot. Letters are not
// silently swallowed by atof() later, they are refused here - a field that
// eats keystrokes and then keeps the old value is worse than one that beeps.
bool edit_accepts(const dai_ui *ui, uint32_t cp) {
    if (!ui->edit.numeric) return cp >= 0x20 && cp != 0x7F;
    const char *b = ui->edit.buf;
    if (cp >= '0' && cp <= '9') return true;
    if (cp == '-') return ui->edit.cursor == 0 && std::strchr(b, '-') == nullptr;
    if (cp == '.' || cp == ',') return std::strchr(b, '.') == nullptr;
    return false;
}

int edit_insert_cp(dai_ui *ui, uint32_t cp) {
    if (cp == ',' && ui->edit.numeric) cp = '.';
    char enc[5];
    int n = 0;
    if (cp < 0x80) { enc[n++] = (char)cp; }
    else if (cp < 0x800) { enc[n++] = (char)(0xC0 | (cp >> 6)); enc[n++] = (char)(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000) { enc[n++] = (char)(0xE0 | (cp >> 12)); enc[n++] = (char)(0x80 | ((cp >> 6) & 0x3F)); enc[n++] = (char)(0x80 | (cp & 0x3F)); }
    else { enc[n++] = (char)(0xF0 | (cp >> 18)); enc[n++] = (char)(0x80 | ((cp >> 12) & 0x3F)); enc[n++] = (char)(0x80 | ((cp >> 6) & 0x3F)); enc[n++] = (char)(0x80 | (cp & 0x3F)); }
    enc[n] = 0;
    char *b = ui->edit.buf;
    size_t len = std::strlen(b);
    if (len + (size_t)n + 1 > sizeof(ui->edit.buf)) return 0;
    std::memmove(b + ui->edit.cursor + n, b + ui->edit.cursor, len - ui->edit.cursor + 1);
    std::memcpy(b + ui->edit.cursor, enc, (size_t)n);
    ui->edit.cursor += (uint32_t)n;
    ui->edit.anchor = ui->edit.cursor;
    return 1;
}

void edit_open(dai_ui *ui, uint64_t id, const char *text, bool numeric, bool select_all) {
    dai_ui::Edit &e = ui->edit;
    e.id = id;
    e.editing = true;
    e.numeric = numeric;
    e.opened_now = true;
    e.scroll = 0;
    std::snprintf(e.buf, sizeof(e.buf), "%s", text ? text : "");
    uint32_t len = (uint32_t)std::strlen(e.buf);
    if (select_all) { e.anchor = 0; e.cursor = len; }
    else            { e.anchor = e.cursor = len; }
}

void edit_close(dai_ui *ui) {
    ui->edit.editing = false;
    ui->edit.dragging = false;
    ui->edit.id = 0;
}

// Returns 1 when the buffer changed. *commit on Enter/Tab, *cancel on Escape.
int edit_keys(dai_ui *ui, bool *commit, bool *cancel) {
    dai_ui::Edit &e = ui->edit;
    const dai_ui_input &in = ui->input;
    *commit = false;
    *cancel = false;
    int changed = 0;
    size_t len = std::strlen(e.buf);
    bool shift = in.key_shift != 0;

    if (in.key_select_all) { e.anchor = 0; e.cursor = (uint32_t)len; }

    if (in.key_left) {
        uint32_t lo, hi; edit_selection(ui, &lo, &hi);
        if (!shift && hi > lo) e.cursor = lo;
        else e.cursor = (uint32_t)utf8_prev(e.buf, e.cursor);
        if (!shift) e.anchor = e.cursor;
    }
    if (in.key_right) {
        uint32_t lo, hi; edit_selection(ui, &lo, &hi);
        if (!shift && hi > lo) e.cursor = hi;
        else e.cursor = (uint32_t)utf8_next(e.buf, len, e.cursor);
        if (!shift) e.anchor = e.cursor;
    }
    if (in.key_home) { e.cursor = 0; if (!shift) e.anchor = 0; }
    if (in.key_end)  { e.cursor = (uint32_t)len; if (!shift) e.anchor = e.cursor; }

    if (in.key_backspace) {
        if (edit_erase_selection(ui)) changed = 1;
        else if (e.cursor > 0) {
            size_t p = utf8_prev(e.buf, e.cursor);
            std::memmove(e.buf + p, e.buf + e.cursor, len - e.cursor + 1);
            e.cursor = e.anchor = (uint32_t)p;
            changed = 1;
        }
    }
    if (in.key_delete) {
        len = std::strlen(e.buf);
        if (edit_erase_selection(ui)) changed = 1;
        else if (e.cursor < len) {
            size_t nx = utf8_next(e.buf, len, e.cursor);
            std::memmove(e.buf + e.cursor, e.buf + nx, len - nx + 1);
            e.anchor = e.cursor;
            changed = 1;
        }
    }
    for (int i = 0; i < 8 && in.text[i]; ++i) {
        uint32_t cp = in.text[i];
        if (cp == '\r' || cp == '\n' || cp == '\t' || cp == 0x1B || cp == 8) continue;
        if (!edit_accepts(ui, cp)) continue;
        edit_erase_selection(ui);
        changed |= edit_insert_cp(ui, cp);
    }
    if (in.key_enter || in.key_tab) *commit = true;
    if (in.key_escape) *cancel = true;
    return changed;
}

// Selection block, text, caret - clipped to the box, scrolled so the caret is
// always visible. A value longer than its field otherwise puts the caret
// somewhere off screen and typing looks like nothing is happening.
void edit_draw(dai_ui *ui, float x, float y, float w, float h, uint32_t col, float pad) {
    dai_ui::Edit &e = ui->edit;
    float avail = w - pad * 2.0f;
    if (avail < 8.0f) avail = 8.0f;
    float caret_w = text_w_n(ui, e.buf, e.cursor);
    float full = dai_ui_text_width(ui, e.buf);
    if (full <= avail) e.scroll = 0.0f;
    else {
        if (caret_w - e.scroll > avail) e.scroll = caret_w - avail;
        if (caret_w - e.scroll < 0.0f)  e.scroll = caret_w;
        if (full - e.scroll < avail)    e.scroll = full - avail;
        if (e.scroll < 0.0f) e.scroll = 0.0f;
    }
    float tx = x + pad - e.scroll;
    e.text_x = tx;

    ui->clips.push_back(dai_ui::Clip{ x + 1.0f, y, x + w - 1.0f, y + h });
    uint32_t lo, hi;
    edit_selection(ui, &lo, &hi);
    if (hi > lo) {
        float sx0 = tx + text_w_n(ui, e.buf, lo);
        float sx1 = tx + text_w_n(ui, e.buf, hi);
        dai_ui_rect(ui, sx0, y + 2.0f, sx1 - sx0, h - 4.0f, rgba(0x2C, 0x5D, 0x87, 255));
    }
    float ty = y + (h - dai_font_line_height(ui->font)) * 0.5f;
    dai_ui_text(ui, tx, ty, e.buf, col);
    dai_ui_rect(ui, tx + caret_w, y + 2.0f, 1.0f, h - 4.0f, rgba(0xE0, 0xE0, 0xE0, 255));
    ui->clips.pop_back();
}

// The whole field: hit testing, focus, mouse selection, keys, drawing.
// `live` mirrors the buffer back on every keystroke (a name field), otherwise
// the caller reads it on commit (a numeric field parses it).
int text_field_impl(dai_ui *ui, uint64_t id, float x, float y, float w, float h,
                    char *buf, size_t buf_size, bool numeric, bool live,
                    int *commit_out, uint32_t text_col, float pad) {
    if (commit_out) *commit_out = 0;
    bool over = inside_chk(ui, x, y, w, h);
    bool editing = ui->edit.editing && ui->edit.id == id;
    if (over) {
        ui->hot = id;
        ui->mouse_over_ui = true;
        ui->cursor_want = DAI_CURSOR_TEXT;
    }
    int changed = 0;
    bool pressed = ui->input.mouse_down && !ui->prev.mouse_down;
    float mx = ui->input.mouse_x;

    auto commit_now = [&]() {
        if (!live) {
            std::snprintf(buf, buf_size, "%s", ui->edit.buf);
            changed = 1;
        }
        if (commit_out) *commit_out = 1;
        edit_close(ui);
        editing = false;
    };

    if (pressed) {
        if (over && !editing) {
            // First click: the whole value is selected, so typing replaces it -
            // which is what an inspector field is for.
            edit_open(ui, id, buf, numeric, true);
            editing = true;
            ui->edit.dragging = true;
            ui->edit.anchor = 0;
            ui->edit.cursor = (uint32_t)std::strlen(ui->edit.buf);
        } else if (over && editing) {
            if (ui->input.double_click) {
                ui->edit.anchor = 0;
                ui->edit.cursor = (uint32_t)std::strlen(ui->edit.buf);
            } else {
                uint32_t c = caret_at(ui, ui->edit.buf, ui->edit.text_x, mx);
                ui->edit.cursor = ui->edit.anchor = c;
                ui->edit.dragging = true;
            }
        } else if (editing) {
            commit_now();
        }
    }
    // Dragging the pointer selects - but not on the frame the field was opened
    // by that same click, or a click with one pixel of jitter unselects the
    // value it just selected.
    if (editing && ui->edit.dragging && ui->input.mouse_down && !pressed && !ui->edit.opened_now) {
        ui->edit.cursor = caret_at(ui, ui->edit.buf, ui->edit.text_x, mx);
    }
    if (editing) {
        bool commit = false, cancel = false;
        changed |= edit_keys(ui, &commit, &cancel);
        if (live) {
            std::snprintf(buf, buf_size, "%s", ui->edit.buf);
        }
        if (cancel) { edit_close(ui); editing = false; }
        else if (commit) commit_now();
    }

    if (editing) {
        edit_draw(ui, x, y, w, h, text_col ? text_col : ui->style.text, pad);
    }
    if (editing) ui->edit.opened_now = false;
    // Taken here, not on the way in: a click OPENS the edit in the middle of
    // this function, and a field that is opened and closed again in the same
    // frame (Enter, Escape, a click on the way out) must not be marked at all.
    if (ui->edit.editing && ui->edit.id == id) ui->edit_seen = id;
    return changed;
}

} // namespace

extern "C" {

int dai_ui_input_text(dai_ui *ui, const char *label, char *buf, size_t buf_size) {
    if (!ui || !buf || buf_size < 2) return 0;
    float h = widget_height(ui), x, y, w;
    field_rect(ui, label, &x, &y, &w, h);
    uint64_t id = hash_id(label ? label : "text", x, y);
    float by = y + 1.0f, bh = h - 2.0f;
    bool editing = ui->edit.editing && ui->edit.id == id;

    dai_ui_rect(ui, x, by, w, bh, ui->style.track);
    dai_ui_rect_outline(ui, x, by, w, bh, 1.0f,
                        editing ? ui->style.accent : ui->style.panel_border);
    int changed = text_field_impl(ui, id, x, by, w, bh, buf, buf_size, false, true,
                                  nullptr, ui->style.text, 4.0f);
    if (!(ui->edit.editing && ui->edit.id == id))
        dai_ui_text(ui, x + 4.0f, by + (bh - dai_font_line_height(ui->font)) * 0.5f, buf,
                    ui->style.text);
    return changed;
}

void dai_ui_text_focus_next(dai_ui *ui) { if (ui) ui->focus_next_field = 1; }

const char *dai_ui_hot_label(const dai_ui *ui) { return ui ? ui->hot_label : nullptr; }

int dai_ui_text_field(dai_ui *ui, const char *id_str, float x, float y, float w, float h,
                      char *buf, size_t buf_size, int *commit) {
    if (commit) *commit = 0;
    if (!ui || !buf || buf_size < 2) return 0;
    uint64_t id = hash_id(id_str ? id_str : "field", x, y);
    // A create flow armed this field: take focus without waiting for a click,
    // text selected - "new file, type the name right there", like Unity.
    if (ui->focus_next_field) {
        ui->focus_next_field = 0;
        // Whatever had the keyboard gives it up: a picker that opens with a
        // search box and does not GET the keys is a picker where typing does
        // nothing, which is exactly how "the search does not work" looks.
        if (ui->edit.editing && ui->edit.id != id) edit_close(ui);
        if (!ui->edit.editing) { edit_open(ui, id, buf, false, true); ui->edit.opened_now = false; }
    }
    bool editing = ui->edit.editing && ui->edit.id == id;
    dai_ui_rrect(ui, x, y, w, h, ui->style.rounding, ui->style.track);
    dai_ui_rect_outline(ui, x, y, w, h, 1.0f,
                        editing ? ui->style.accent : ui->style.panel_border);
    int changed = text_field_impl(ui, id, x, y, w, h, buf, buf_size, false, true,
                                  commit, ui->style.text, 4.0f);
    if (!(ui->edit.editing && ui->edit.id == id))
        dai_ui_text(ui, x + 4.0f, y + (h - dai_font_line_height(ui->font)) * 0.5f, buf,
                    ui->style.text);
    return changed;
}

int dai_ui_tree_item(dai_ui *ui, const char *label, int depth, int has_children,
                     int *open, int selected) {
    if (!ui || !label) return 0;
    float h = dai_font_line_height(ui->font) + 2.0f;
    float x, y;
    next_rect(ui, 0, h, &x, &y);
    float w = (ui->in_panel ? ui->panel_w - ui->style.padding * 2 : ui->width);
    float indent = 12.0f * (float)(depth < 0 ? 0 : depth);

    uint64_t id = hash_id(label, x, y);
    bool over = inside_chk(ui, x, y, w, h);
    if (over) { ui->hot = id; ui->mouse_over_ui = true; }

    float arrow_w = 14.0f;
    bool on_arrow = has_children && open &&
                    ui->input.mouse_x >= x + indent && ui->input.mouse_x < x + indent + arrow_w &&
                    ui->input.mouse_y >= y && ui->input.mouse_y < y + h;
    int clicked = 0;
    if (over && ui->input.mouse_down && !ui->prev.mouse_down) {
        // The fold arrow must not also select: hitting the triangle to expand
        // a group and losing the current selection is maddening.
        if (on_arrow) *open = !*open;
        else clicked = 1;
    }

    if (selected)   dai_ui_rect(ui, x, y, w, h, ui->style.accent);
    else if (over)  dai_ui_rect(ui, x, y, w, h, ui->style.button_hover);
    if (has_children && open) {
        const char *chev = *open ? "chevron-down" : "chevron-right";
        if (dai_ui_has_icon(ui, chev)) {
            float isz = dai_icons_size(ui->icons);
            if (isz <= 0.0f || isz > h) isz = h - 2.0f;
            dai_ui_icon_at(ui, chev, x + indent + 1.0f, y + (h - isz) * 0.5f, isz,
                           ui->style.text_dim);
        } else {
            dai_ui_text(ui, x + indent + 2.0f, y + 1.0f, *open ? "▾" : "▸",
                        ui->style.text_dim);
        }
    }
    dai_ui_text(ui, x + indent + arrow_w + 2.0f, y + 1.0f, label,
                selected ? ui->style.text : ui->style.text);
    return clicked;
}

int dai_ui_tree_item_ex(dai_ui *ui, const char *label, int depth,
                        int has_children, int *open, int selected) {
    return dai_ui_tree_item_icon(ui, nullptr, label, depth, has_children, open, selected);
}

// The colour of the LAST row's label, consumed by dai_ui_tree_item_icon on the
// next call. A parameter would have meant touching every call site for one
// case; this is the same trick the style stack is, scoped to one row.
static uint32_t g_tree_label_col = 0;
void dai_ui_tree_label_color(dai_ui *ui, uint32_t rgba) { (void)ui; g_tree_label_col = rgba; }

int dai_ui_tree_item_icon(dai_ui *ui, const char *icon, const char *label, int depth,
                          int has_children, int *open, int selected) {
    if (!ui || !label) return 0;
    float h = dai_font_line_height(ui->font) + 2.0f;
    float x, y;
    next_rect(ui, 0, h, &x, &y);
    float w = (ui->in_panel ? ui->panel_w - ui->style.padding * 2 : ui->width);
    float indent = 12.0f * (float)(depth < 0 ? 0 : depth);

    ui->last_x = x; ui->last_y = y; ui->last_w = w; ui->last_h = h;
    uint64_t id = hash_id(label, x, y);
    bool over = inside_chk(ui, x, y, w, h);
    if (over) { ui->hot = id; ui->mouse_over_ui = true; }

    float arrow_w = 14.0f;
    bool on_arrow = has_children && open &&
                    ui->input.mouse_x >= x + indent && ui->input.mouse_x < x + indent + arrow_w &&
                    ui->input.mouse_y >= y && ui->input.mouse_y < y + h;
    int clicked = 0;
    if (over && ui->input.mouse_down && !ui->prev.mouse_down) {
        if (on_arrow) *open = !*open;
        else clicked = 1;
    }
    // The right button folds nothing and selects nothing - it is the menu's
    // button, and the caller decides what the menu says.
    if (over && ui->input.right_down && !ui->prev.right_down) clicked |= 2;
    if (over) clicked |= 4;   // hover report: drag & drop aims at rows, not panels

    if (selected)   dai_ui_rect(ui, x, y, w, h, ui->style.accent);
    else if (over)  dai_ui_rect(ui, x, y, w, h, ui->style.button_hover);
    if (has_children && open) {
        const char *chev = *open ? "chevron-down" : "chevron-right";
        if (dai_ui_has_icon(ui, chev)) {
            float isz = dai_icons_size(ui->icons);
            if (isz <= 0.0f || isz > h) isz = h - 2.0f;
            dai_ui_icon_at(ui, chev, x + indent + 1.0f, y + (h - isz) * 0.5f, isz,
                           ui->style.text_dim);
        } else {
            // Drawn, not typed: this font has no U+25B8/U+25BE, and a missing
            // glyph renders as a '?' - which is what the hierarchy showed.
            float ax = x + indent + 4.0f, ay = y + h * 0.5f;
            if (*open) {
                dai_ui_line(ui, ax - 3.0f, ay - 1.5f, ax, ay + 2.5f, 1.5f, ui->style.text_dim);
                dai_ui_line(ui, ax, ay + 2.5f, ax + 3.0f, ay - 1.5f, 1.5f, ui->style.text_dim);
            } else {
                dai_ui_line(ui, ax - 1.5f, ay - 3.0f, ax + 2.5f, ay, 1.5f, ui->style.text_dim);
                dai_ui_line(ui, ax + 2.5f, ay, ax - 1.5f, ay + 3.0f, 1.5f, ui->style.text_dim);
            }
        }
    }
    float tx = x + indent + arrow_w + 2.0f;
    if (icon && dai_ui_has_icon(ui, icon)) {
        float isz = dai_icons_size(ui->icons);
        if (isz <= 0.0f || isz > h) isz = h - 2.0f;
        // Tinted like the text, dimmer when the row is not selected: an icon
        // column that shouts is a column you read instead of the names.
        dai_ui_icon_at(ui, icon, tx, y + (h - isz) * 0.5f, isz,
                       selected ? ui->style.text : ui->style.text_dim);
        tx += isz + 4.0f;
    }
    dai_ui_text(ui, tx, y + 1.0f, label,
                g_tree_label_col ? g_tree_label_col : ui->style.text);
    g_tree_label_col = 0;      // one row only: it is set immediately before
    return clicked;
}

int dai_ui_tree_rename(dai_ui *ui, char *buf, size_t buf_size, int depth,
                       int has_children, int *open) {
    if (!ui || !buf || buf_size < 2) return -1;
    float h = dai_font_line_height(ui->font) + 2.0f;
    float x, y;
    next_rect(ui, 0, h, &x, &y);
    float w = (ui->in_panel ? ui->panel_w - ui->style.padding * 2 : ui->width);
    float indent = 12.0f * (float)(depth < 0 ? 0 : depth) + 14.0f;

    uint64_t id = hash_id("rename", x, y);
    // The row was just created: take focus without waiting for a click, with
    // the name selected - F2 then typing replaces it, like every file manager.
    if (!ui->edit.editing) {
        edit_open(ui, id, buf, false, true);
        ui->edit.opened_now = false;
    }
    bool focused = ui->edit.editing && ui->edit.id == id;

    dai_ui_rect(ui, x + indent, y + 1.0f, w - indent, h - 2.0f, ui->style.track);
    dai_ui_rect_outline(ui, x + indent, y + 1.0f, w - indent, h - 2.0f, 1.0f,
                        ui->style.accent);
    int commit = 0;
    text_field_impl(ui, id, x + indent, y + 1.0f, w - indent, h - 2.0f, buf, buf_size,
                    false, true, &commit, ui->style.text, 4.0f);
    if (!(ui->edit.editing && ui->edit.id == id)) {
        dai_ui_text(ui, x + indent + 4.0f, y + 1.0f, buf, ui->style.text);
        // Focus went somewhere else entirely (another field): that is a commit
        // too, or the row would sit there forever waiting for an Enter.
        if (!focused) commit = 1;
    }
    (void)has_children; (void)open;
    return commit;
}

// ------------------------------------------------------------- scrolling

void dai_ui_scroll_begin(dai_ui *ui, const char *id_str, float height) {
    if (!ui) return;
    float x, y;
    // Place, do not advance: the region clips what is inside it, so reserving
    // its height in the layout moved whatever follows by the region's height
    // EVERY frame - which is exactly the snap-back when you scrolled down.
    x = ui->cursor_x; y = ui->cursor_y;
    float w = (ui->in_panel ? ui->panel_w - ui->style.padding * 2 : ui->width);
    uint64_t id = hash_id(id_str ? id_str : "scroll", x, y);
    float &off = ui->scroll_of(id);

    // Only scroll what CAN scroll. The limit is last frame's - one frame of
    // lag on a value that only changes when the panel's contents change, and
    // the alternative is a visible jump every time the wheel is touched over
    // a list that already fits.
    float limit = ui->scroll_max_of(id);
    if (inside_chk(ui, x, y, w, height)) {
        ui->mouse_over_ui = true;
        if (ui->input.wheel != 0.0f && limit > 0.0f) off -= ui->input.wheel * 32.0f;
    }
    if (off < 0.0f) off = 0.0f;
    if (off > limit) off = limit;

    ui->scroll_stack.push_back(dai_ui::ScrollFrame{ id, x, y, w, height, y });
    // The bar lives in the last ten pixels; nothing else may be laid out
    // there or every field's button ends up underneath it.
    ui->scroll_saved_panel_w.push_back(ui->panel_w);
    if (ui->in_panel && ui->panel_w > 60.0f) ui->panel_w -= 10.0f;
    ui->clips.push_back(dai_ui::Clip{ x, y, x + w, y + height });
    // The layout continues inside the region, shifted by the scroll offset.
    ui->cursor_x = x;
    ui->cursor_y = y - off;
}

void dai_ui_scroll_reveal(dai_ui *ui, float y, float h) {
    if (!ui || ui->scroll_stack.empty()) return;
    const dai_ui::ScrollFrame &f = ui->scroll_stack.back();
    float &off = ui->scroll_of(f.id);
    float top = f.y + 2.0f, bot = f.y + f.h - 2.0f;
    if (y < top)              off -= (top - y);
    else if (y + h > bot)     off += (y + h - bot);
    if (off < 0.0f) off = 0.0f;
}

void dai_ui_scroll_end(dai_ui *ui) {
    if (!ui || ui->scroll_stack.empty()) return;
    dai_ui::ScrollFrame f = ui->scroll_stack.back();
    ui->scroll_stack.pop_back();
    if (!ui->scroll_saved_panel_w.empty()) {
        ui->panel_w = ui->scroll_saved_panel_w.back();
        ui->scroll_saved_panel_w.pop_back();
    }
    if (!ui->clips.empty()) ui->clips.pop_back();

    float &off = ui->scroll_of(f.id);
    // The cursor sits one `spacing` past the last widget - that gap is not
    // content, and counting it made a panel that fits exactly report a few
    // pixels of overflow. The wheel then moved those few pixels and the
    // clamp put them straight back: scrolling that only ever jittered.
    float content = (ui->cursor_y + off) - f.start_y - ui->style.spacing;
    float max_off = content - f.h;
    if (max_off < 2.0f) max_off = 0.0f;   // under two pixels is not scrolling
    if (off > max_off) off = max_off;
    // What the NEXT frame's wheel is allowed to do.
    ui->scroll_max_of(f.id) = max_off;

    if (max_off > 0.0f) {
        float track_x = f.x + f.w - 7.0f;
        float track_w = 6.0f;
        float frac = f.h / (content > 0 ? content : 1.0f);
        float bar_h = f.h * (frac > 1 ? 1 : frac);
        if (bar_h < 20.0f) bar_h = 20.0f;
        float t = off / max_off;
        float bar_y = f.y + (f.h - bar_h) * t;
        // Draggable: press anywhere on the bar and the offset follows the
        // pointer, press the track and it jumps there. Both are what every
        // other scrollbar does, and neither existed.
        float mx = ui->input.mouse_x, my = ui->input.mouse_y;
        bool over = mx >= track_x - 3.0f && mx < track_x + track_w + 3.0f &&
                    my >= f.y && my < f.y + f.h;
        bool pressed = ui->input.mouse_down && !ui->prev.mouse_down;
        if (over && pressed && inside_chk(ui, f.x, f.y, f.w, f.h)) {
            ui->scroll_drag_id = f.id;
            ui->scroll_grab = (my >= bar_y && my < bar_y + bar_h) ? (my - bar_y) : bar_h * 0.5f;
        }
        if (!ui->input.mouse_down) ui->scroll_drag_id = 0;
        if (ui->scroll_drag_id == f.id && ui->input.mouse_down) {
            float span = f.h - bar_h;
            float rel = span > 0.5f ? (my - ui->scroll_grab - f.y) / span : 0.0f;
            if (rel < 0.0f) rel = 0.0f;
            if (rel > 1.0f) rel = 1.0f;
            off = rel * max_off;
            bar_y = f.y + span * rel;
            ui->mouse_over_ui = true;
        }
        uint32_t bc = (over || ui->scroll_drag_id == f.id) ? ui->style.accent
                                                           : ui->style.button_hover;
        dai_ui_rrect(ui, track_x, f.y, track_w, f.h, 3.0f, ui->style.track);
        dai_ui_rrect(ui, track_x, bar_y, track_w, bar_h, 3.0f, bc);
    } else if (ui->scroll_drag_id == f.id) {
        ui->scroll_drag_id = 0;
    }
    ui->cursor_x = ui->in_panel ? ui->panel_x + ui->style.padding : 0.0f;
    ui->cursor_y = f.y + f.h + ui->style.spacing;
}

// ---------------------------------------------------------------- sprites

int num_field_at_impl(dai_ui *ui, float x, float y, float w, float h, float *value,
                      float step, float min, float max, const char *id_str,
                      bool with_label, uint32_t accent, const char *axis);
int num_field_at(dai_ui *ui, float x, float y, float w, float h, float *value,
                 float step, float min, float max, const char *id_str,
                 bool with_label, uint32_t accent, const char *axis);

int dai_ui_num_editing(const dai_ui *ui) { return ui && ui->edit.editing ? 1 : 0; }

int dai_ui_right_down(const dai_ui *ui) { return ui ? ui->input.right_down : 0; }
int dai_ui_right_pressed(const dai_ui *ui) {
    return ui ? (ui->input.right_down && !ui->prev.right_down) : 0;
}

int dai_ui_icon_button_at(dai_ui *ui, const char *name, float x, float y,
                          float w, float h, int active) {
    if (!ui) return 0;
    uint64_t id = hash_id(name ? name : "icon", x, y);
    bool over = inside_chk(ui, x, y, w, h);
    if (over) { ui->hot = id; ui->mouse_over_ui = true; }
    bool pressed = false;
    if (over && ui->input.mouse_down && !ui->prev.mouse_down) ui->active = id;
    if (ui->active == id && !ui->input.mouse_down) { pressed = over; ui->active = 0; }

    uint32_t bg = active ? ui->style.accent : (over ? ui->style.button_hover : ui->style.button);
    dai_ui_rrect(ui, x, y, w, h, ui->style.rounding, bg);
    float isz = h - 10.0f;
    dai_ui_icon_at(ui, name, x + (w - isz) * 0.5f, y + 5.0f, isz,
                   active ? 0xFF101010u : ui->style.text);
    return pressed ? 1 : 0;
}

void dai_ui_popup_panel_begin(dai_ui *ui, dai_ui_popup *m, float w, float min_h) {
    if (!ui || !m) return;
    float x = m->x, y = m->y;
    float h = min_h > 0.0f ? min_h : 400.0f;   // clipped to content by the panel
    if (x + w > ui->width) x = ui->width - w - 4.0f;
    if (y + h > ui->height) y = ui->height - h - 4.0f;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    // Same treatment as dai_ui_popup_menu: own layer, no clip, click outside
    // dismisses. The panel state lives on dai_ui so _end can restore it.
    ui->pp_save_layer = ui->cur_layer;
    ui->pp_save_clips.swap(ui->clips);
    ui->cur_layer = (1 << 20) - 2;
    ui->in_popup = false;
    ui->pp_menu = m;
    float mx = ui->input.mouse_x, my = ui->input.mouse_y;
    bool over = mx >= x && mx < x + w && my >= y && my < y + h;
    if (ui->input.mouse_down && !ui->prev.mouse_down && !over) m->open = 0;
    dai_ui_rect(ui, x + 2.0f, y + 3.0f, w, h, ui->style.shadow);
    dai_ui_rect(ui, x, y, w, h, ui->style.panel);
    dai_ui_rect_outline(ui, x, y, w, h, 1.0f, ui->style.panel_border);
    dai_ui_panel_begin(ui, x + 4.0f, y + 4.0f, w - 8.0f, h - 8.0f, nullptr);
}

void dai_ui_popup_panel_end(dai_ui *ui) {
    if (!ui) return;
    dai_ui_panel_end(ui);
    ui->cur_layer = ui->pp_save_layer;
    ui->clips.swap(ui->pp_save_clips);
    if (ui->input.key_escape && ui->pp_menu) ui->pp_menu->open = 0;
    ui->pp_menu = nullptr;
}

void dai_ui_popup_open(dai_ui_popup *m, float x, float y) {
    if (!m) return;
    m->x = x; m->y = y; m->open = 1; m->placed = 0;
}

void dai_ui_popup_close(dai_ui_popup *m) { if (m) m->open = 0; }

int dai_ui_popup_menu(dai_ui *ui, dai_ui_popup *m,
                      const dai_ui_menu_item *items, uint32_t count) {
    if (!ui || !m || !m->open) return -2;
    // An empty menu is not a menu. Closing it here is the difference between
    // "nothing happened" and a button that stays stuck open for ever.
    if (!items || !count) { m->open = 0; return -1; }

    float row_h = dai_font_line_height(ui->font) + 8.0f;
    float pad = 4.0f;
    float w = 0.0f;
    for (uint32_t i = 0; i < count; ++i) {
        float tw = dai_ui_text_width(ui, items[i].label ? items[i].label : "");
        if (items[i].shortcut) tw += 24.0f + dai_ui_text_width(ui, items[i].shortcut);
        if (tw > w) w = tw;
    }
    w += 46.0f + pad * 2.0f;
    float h = row_h * (float)count + pad * 2.0f;

    // Keep the whole thing on screen - a menu that opens half off the right
    // edge because you right clicked the last row is its own bug report.
    float x = m->x, y = m->y;
    if (x + w > ui->width) x = ui->width - w - 4.0f;
    if (y + h > ui->height) y = ui->height - h - 4.0f;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    // Never let the menu land beside the pointer: it is dismissed by a press
    // that is not over it, and the press that OPENED it is still the current
    // one on the frame it first draws. ONCE, on the first frame - doing it
    // every frame is a menu that follows the mouse around the screen.
    if (!m->placed) {
        float mx0 = ui->input.mouse_x, my0 = ui->input.mouse_y;
        if (mx0 >= 0.0f && my0 >= 0.0f) {
            if (mx0 < x)          x = mx0 - 6.0f;
            if (mx0 >= x + w)     x = mx0 - w + 6.0f;
            if (my0 < y)          y = my0 - 6.0f;
            if (my0 >= y + h)     y = my0 - h + 6.0f;
            if (x < 0) x = 0;
            if (y < 0) y = 0;
        }
        m->x = x; m->y = y;      // frozen: the menu does not move again
        m->placed = 1;
    }

    int result = -2;
    float mx = ui->input.mouse_x, my = ui->input.mouse_y;
    bool over = mx >= x && mx < x + w && my >= y && my < y + h;

    // Above every window, clipped to nothing: a context menu is never behind
    // the panel that summoned it.
    int save_layer = ui->cur_layer;
    std::vector<dai_ui::Clip> save_clips;
    save_clips.swap(ui->clips);
    ui->cur_layer = (1 << 20) - 1;      // just under the tooltip
    ui->in_popup = false;               // the menu itself is always hittable

    // A press OUTSIDE dismisses - either button. It used to be the left one
    // only, and that is the bug behind "I right clicked into nothing and it
    // said transform copied": the right press did not close the open menu, it
    // just moved on, and the next left click landed on whatever row happened
    // to be under the cursor.
    if (((ui->input.mouse_down && !ui->prev.mouse_down) ||
         (ui->input.right_down && !ui->prev.right_down)) && !over) {
        m->open = 0;
        result = -1;                    // dismissed
    }

    dai_ui_rect(ui, x + 2.0f, y + 3.0f, w, h, ui->style.shadow);
    dai_ui_rect(ui, x, y, w, h, ui->style.panel);
    dai_ui_rect_outline(ui, x, y, w, h, 1.0f, ui->style.panel_border);

    int hovered = -1;
    if (over && result == -2)
        hovered = (int)((my - y - pad) / row_h);
    // A caption row is not a choice: it must not highlight and it must not be
    // returned. A confirm box built from "the question" plus "the action"
    // otherwise reads as two answers to a yes/no question, which is one
    // answer too many.
    if (hovered >= 0 && hovered < (int)count && items[hovered].header) hovered = -1;

    for (uint32_t i = 0; i < count; ++i) {
        float ry = y + pad + row_h * (float)i;
        bool head = items[i].header != 0;
        if ((int)i == hovered)
            dai_ui_rect(ui, x + 1.0f, ry, w - 2.0f, row_h, ui->style.button_hover);
        float tx = x + 8.0f;
        if (items[i].icon && dai_ui_has_icon(ui, items[i].icon)) {
            float isz = row_h - 8.0f;
            dai_ui_icon_at(ui, items[i].icon, tx, ry + 4.0f, isz, ui->style.text_dim);
            tx += isz + 6.0f;
        }
        dai_ui_text(ui, tx, ry + 4.0f, items[i].label,
                    head ? ui->style.text_dim : ui->style.text);
        if (head) {
            float ry2 = ry + row_h - 1.0f;
            dai_ui_rect(ui, x + 6.0f, ry2, w - 12.0f, 1.0f, ui->style.panel_border);
        }
        if (items[i].shortcut) {
            float sw = dai_ui_text_width(ui, items[i].shortcut);
            dai_ui_text(ui, x + w - sw - 8.0f, ry + 4.0f, items[i].shortcut,
                        ui->style.text_dim);
        }
    }

    if ((int)hovered >= 0 && ui->input.mouse_down && !ui->prev.mouse_down) {
        result = hovered;
        m->open = 0;
    }

    ui->cur_layer = save_layer;
    ui->clips.swap(save_clips);
    ui->in_popup = true;                // everyone else this frame: dead
    ui->popup_was_open = true;          // and next frame's start as well
    ui->mouse_over_ui = true;
    return result;
}

// ---------------------------------------------------------------- numeric

namespace {

void num_fmt(char *buf, size_t n, float v) {
    std::snprintf(buf, n, "%s", trim_number(v).c_str());
}

float num_parse(const char *buf, float fallback) {
    if (!buf || !*buf) return fallback;
    char *end = nullptr;
    float v = std::strtof(buf, &end);
    if (end == buf) return fallback;     // nothing parseable: keep the old one
    return v;
}

} // namespace

int dai_ui_num_field(dai_ui *ui, const char *label, float *value,
                     float step, float min, float max, const char *id) {
    if (!ui || !value) return 0;
    float h = widget_height(ui), x, y, w;
    field_rect(ui, label, &x, &y, &w, h);
    float lx = g_label_x, ly = g_label_y, lw = g_label_w;
    const char *ids = id ? id : (label ? label : "num");
    int changed = num_field_at(ui, x, y, w, h, value, step, min, max, ids, true, 0, nullptr);
    changed |= label_scrub(ui, hash_id(ids, lx, ly) ^ 0xD1B54A32D192ED03ull,
                           lx, ly, lw, h, value, step, min, max);
    return changed;
}

int num_field_at(dai_ui *ui, float x, float y, float w, float h, float *value,
                 float step, float min, float max, const char *id_str,
                 bool with_label, uint32_t accent, const char *axis) {
    return num_field_at_impl(ui, x, y, w, h, value, step, min, max, id_str,
                             with_label, accent, axis);
}

int num_field_at_impl(dai_ui *ui, float x, float y, float w, float h, float *value,
                      float step, float min, float max, const char *id_str,
                      bool with_label, uint32_t accent, const char *axis) {
    uint64_t id = hash_id(id_str, x, y);
    float mx = ui->input.mouse_x, my = ui->input.mouse_y;
    bool editing = ui->edit.editing && ui->edit.id == id;

    // The axis letter sits OUTSIDE the box, the way Unity draws it, and it is
    // a handle: hover it and drag sideways to change the value. That is why it
    // is not just decoration painted inside the field.
    float lw = 0.0f;
    bool axis_over = false;
    if (axis) {
        lw = 13.0f;
        axis_over = !ui->in_popup && !ui->blocked &&
                    mx >= x && mx < x + lw && my >= y && my < y + h;
    }
    float bx = x + lw, bw = w - lw;
    if (axis_over) { ui->hot = id; ui->mouse_over_ui = true; ui->cursor_want = DAI_CURSOR_SIZE_WE; }

    int changed = 0;

    // ---- the box is a real text field
    dai_ui_rect(ui, bx, y + 1.0f, bw, h - 2.0f, ui->style.track);
    dai_ui_rect_outline(ui, bx, y + 1.0f, bw, h - 2.0f, 1.0f,
                        editing ? ui->style.accent : ui->style.panel_border);

    char buf[64];
    num_fmt(buf, sizeof(buf), *value);
    int commit = 0;
    int typed = text_field_impl(ui, id, bx, y + 1.0f, bw, h - 2.0f, buf, sizeof(buf),
                                true, false, &commit, ui->style.text, 5.0f);
    editing = ui->edit.editing && ui->edit.id == id;
    if (typed) {
        float v = num_parse(buf, *value);
        if (min < max) v = v < min ? min : (v > max ? max : v);
        if (v != *value) { *value = v; changed = 1; }
    }

    // ---- axis drag: hold the letter, move sideways. Never while typing - one
    //      click must not do both.
    if (axis && !editing) {
        if (axis_over && ui->input.mouse_down && !ui->prev.mouse_down) {
            ui->active = id;
            ui->drag_accum = 0.0f;
        }
        if (ui->active == id && ui->input.mouse_down) {
            ui->cursor_want = DAI_CURSOR_SIZE_WE;
            float dx = mx - ui->prev.mouse_x;
            if (dx != 0.0f) {
                float v = *value + dx * step;
                if (min < max) v = v < min ? min : (v > max ? max : v);
                if (v != *value) { *value = v; changed = 1; }
            }
        }
        if (ui->active == id && !ui->input.mouse_down) ui->active = 0;
    }

    bool dragging = axis && ui->active == id;
    if (axis) {
        uint32_t col = accent ? accent : ui->style.text_dim;
        if (axis_over || dragging) col = 0xFFFFFFFFu;
        dai_ui_text(ui, x + 2.0f, y + (h - dai_font_line_height(ui->font)) * 0.5f, axis, col);
    }
    if (!editing) {
        char shown[64];
        num_fmt(shown, sizeof(shown), *value);
        dai_ui_text(ui, bx + 5.0f, y + (h - dai_font_line_height(ui->font)) * 0.5f, shown,
                    ui->style.text);
    }
    (void)with_label;
    return changed;
}

int dai_ui_num_vec3(dai_ui *ui, const char *label, float *xyz, float step) {
    if (!ui || !xyz) return 0;
    if (step <= 0.0f) step = 0.01f;
    float h = widget_height(ui), x, y, w;
    field_rect(ui, label, &x, &y, &w, h);
    const char *names[3] = { "X", "Y", "Z" };
    // Axis colours match the gizmo, so a field and an arm are obviously the
    // same thing.
    const uint32_t cols[3] = { rgba(230, 64, 64, 255), rgba(90, 217, 77, 255),
                               rgba(77, 128, 242, 255) };
    float gap = 4.0f;
    float each = (w - gap * 2.0f) / 3.0f;
    int changed = 0;
    for (int i = 0; i < 3; ++i) {
        float fx = x + (each + gap) * (float)i;
        char key[64];
        std::snprintf(key, sizeof(key), "%s:%c", label ? label : "vec", names[i][0]);
        changed |= num_field_at(ui, fx, y, each, h, &xyz[i], step, 0.0f, 0.0f,
                                key, false, cols[i], names[i]);
    }
    return changed;
}

// The same, at a place the caller picked. The HUD is not a stack of rows, so
// it cannot use the layout cursor - and neither can anything else drawn into
// a viewport rectangle.
void dai_ui_image_at(dai_ui *ui, dai_texture tex, float x, float y, float w, float h,
                     float u0, float v0, float u1, float v1, uint32_t tint) {
    if (!ui) return;
    ui->quad(tex, x, y, x + w, y + h, u0, v0, u1, v1, tint ? tint : 0xFFFFFFFFu);
}

void dai_ui_image(dai_ui *ui, dai_texture tex, float w, float h,
                  float u0, float v0, float u1, float v1, uint32_t tint) {
    if (!ui) return;
    float x, y;
    next_rect(ui, w, h, &x, &y);
    ui->quad(tex, x, y, x + w, y + h, u0, v0, u1, v1, tint ? tint : 0xFFFFFFFFu);
}

int dai_ui_image_button(dai_ui *ui, dai_texture tex, float w, float h,
                        float u0, float v0, float u1, float v1) {
    if (!ui) return 0;
    float x, y;
    next_rect(ui, w, h, &x, &y);
    char key[64];
    std::snprintf(key, sizeof(key), "img%u_%.1f_%.1f", tex, u0, v0);
    uint64_t id = hash_id(key, x, y);
    bool over = inside_chk(ui, x, y, w, h);
    if (over) { ui->hot = id; ui->mouse_over_ui = true; }
    bool pressed = false;
    if (over && ui->input.mouse_down && !ui->prev.mouse_down) ui->active = id;
    if (ui->active == id && !ui->input.mouse_down) { pressed = over; ui->active = 0; }

    dai_ui_rect(ui, x - 2, y - 2, w + 4, h + 4,
                ui->active == id ? ui->style.button_active : over ? ui->style.button_hover : ui->style.button);
    ui->quad(tex, x, y, x + w, y + h, u0, v0, u1, v1, 0xFFFFFFFFu);
    return pressed ? 1 : 0;
}

// ------------------------------------------------------------------ arrays

int dai_ui_array_begin(dai_ui *ui, const char *label, int *count,
                       int *open, int min_n, int max_n) {
    if (!ui || !count) return 0;
    float h = 20.0f;
    float x = ui->in_panel ? ui->panel_x + ui->style.padding : 0.0f;
    float w = (ui->in_panel ? ui->panel_w - ui->style.padding * 2 : ui->width);
    float y = ui->cursor_y;
    dai_ui_advance(ui, 0, h + 1.0f);

    bool over_head = inside_chk(ui, x, y, w - 56.0f, h);
    bool pressed = ui->input.mouse_down && !ui->prev.mouse_down;
    if (over_head) { ui->mouse_over_ui = true; ui->cursor_want = DAI_CURSOR_HAND; }
    int is_open = open ? (*open ? 1 : 0) : 1;
    if (over_head && pressed && open) { *open = !*open; is_open = *open ? 1 : 0; }

    // The foldout, then the name. No background: Unity's array header is part
    // of the component it lives in, not a box inside it.
    const char *chev = is_open ? DAI_ICON_CHEVRON_D : DAI_ICON_CHEVRON_R;
    float isz = 12.0f;
    if (dai_ui_has_icon(ui, chev))
        dai_ui_icon_at(ui, chev, x + 2.0f, y + (h - isz) * 0.5f, isz,
                       over_head ? ui->style.text : ui->style.text_dim);
    float lh = dai_font_line_height(ui->font);
    dai_ui_text(ui, x + 18.0f, y + (h - lh) * 0.5f, label ? label : "Array",
                ui->style.text);

    // The size, on the right, typeable - that is where Unity puts it and
    // typing 4 into it is how you make four of something.
    float bw = 48.0f;
    float fv = (float)*count;
    if (num_field_at(ui, x + w - bw, y + 1.0f, bw, h - 2.0f, &fv, 1.0f,
                     (float)min_n, (float)max_n, "arrsize", false, 0, nullptr)) {
        int nv = (int)(fv + 0.5f);
        if (nv < min_n) nv = min_n;
        if (nv > max_n) nv = max_n;
        *count = nv;
    }
    return is_open;
}

int dai_ui_array_object_row(dai_ui *ui, int index, const char *value,
                            const char *icon) {
    if (!ui) return 0;
    float h = widget_height(ui);
    float x = ui->in_panel ? ui->panel_x + ui->style.padding : 0.0f;
    float w = (ui->in_panel ? ui->panel_w - ui->style.padding * 2 : ui->width);
    float y = ui->cursor_y;
    dai_ui_advance(ui, 0, h + 1.0f);

    // The handle. It does not drag yet; it is what tells you the row belongs
    // to a list rather than being another field that happens to be numbered.
    float hx = x + 12.0f, hy = y + h * 0.5f - 3.0f;
    for (int k = 0; k < 3; ++k)
        dai_ui_rect(ui, hx, hy + (float)k * 3.0f, 9.0f, 1.0f, ui->style.text_dim);

    char lbl[32];
    std::snprintf(lbl, sizeof(lbl), "Element %d", index);
    float lh = dai_font_line_height(ui->font);
    dai_ui_text(ui, x + 28.0f, y + (h - lh) * 0.5f, lbl, ui->style.text_dim);

    float fx = x + (ui->style.label_w > 90.0f ? ui->style.label_w : 90.0f);
    float fw = x + w - fx;
    if (fw < 60.0f) { fx = x + w * 0.45f; fw = w * 0.55f; }
    float bw = h;
    float vw = fw - bw - 2.0f;
    if (vw < 20.0f) { vw = fw; bw = 0.0f; }

    bool over_f = inside_chk(ui, fx, y, vw, h);
    bool over_b = bw > 0.0f && inside_chk(ui, fx + vw + 2.0f, y, bw, h);
    char rid[24];
    std::snprintf(rid, sizeof(rid), "arrrow%d", index);
    uint64_t aid = hash_id(rid, fx, y);
    if (over_f || over_b) { ui->mouse_over_ui = true; ui->cursor_want = DAI_CURSOR_HAND; }
    if ((over_f || over_b) && ui->input.mouse_down && !ui->prev.mouse_down) ui->active = aid;
    bool pressed = false;
    if (ui->active == aid && !ui->input.mouse_down) { pressed = over_f || over_b; ui->active = 0; }

    dai_ui_rrect(ui, fx, y + 1.0f, vw, h - 2.0f, ui->style.rounding,
                 over_f ? ui->style.button_hover : ui->style.track);
    dai_ui_rect_outline(ui, fx, y + 1.0f, vw, h - 2.0f, 1.0f, ui->style.panel_border);
    float isz = dai_icons_size(ui->icons);
    if (isz <= 0.0f || isz > h - 4.0f) isz = h - 6.0f;
    float tx = fx + 5.0f;
    if (icon && dai_ui_has_icon(ui, icon)) {
        dai_ui_icon_at(ui, icon, tx, y + (h - isz) * 0.5f, isz, ui->style.accent);
        tx += isz + 4.0f;
    }
    dai_ui_text(ui, tx, y + (h - lh) * 0.5f, value ? value : "None", ui->style.text);
    if (bw > 0.0f) {
        float bx = fx + vw + 2.0f;
        dai_ui_rrect(ui, bx, y + 1.0f, bw, h - 2.0f, ui->style.rounding,
                     over_b ? ui->style.button_hover : ui->style.button);
        if (dai_ui_has_icon(ui, "target"))
            dai_ui_icon_at(ui, "target", bx + (bw - isz) * 0.5f, y + (h - isz) * 0.5f,
                           isz, ui->style.text);
    }
    return pressed ? 1 : 0;
}

int dai_ui_button_fit(dai_ui *ui, const char *utf8) {
    if (!ui || !utf8) return 0;
    float h = widget_height(ui);
    float w = dai_ui_text_width(ui, utf8) + ui->style.padding * 3.0f;
    float avail = (ui->in_panel ? ui->panel_w - ui->style.padding * 2 : ui->width);
    if (w > avail) w = avail;
    float x, y;
    next_rect(ui, w, h, &x, &y);
    uint64_t id = hash_id(utf8, x, y);
    bool over = inside_chk(ui, x, y, w, h);
    if (over) { ui->hot = id; ui->hot_label = utf8; ui->mouse_over_ui = true;
                ui->cursor_want = DAI_CURSOR_HAND; }
    bool pressed = false;
    if (over && ui->input.mouse_down && !ui->prev.mouse_down) ui->active = id;
    if (ui->active == id && !ui->input.mouse_down) { pressed = over; ui->active = 0; }
    uint32_t col = ui->style.button;
    if (ui->active == id) col = ui->style.button_active;
    else if (over) col = ui->style.button_hover;
    dai_ui_rrect(ui, x, y, w, h, ui->style.rounding, col);
    float tw = dai_ui_text_width(ui, utf8);
    dai_ui_text(ui, x + (w - tw) * 0.5f, y + ui->style.row_pad * 0.5f, utf8, ui->style.text);
    return pressed ? 1 : 0;
}

int dai_ui_array_end(dai_ui *ui, int count, int min_n, int max_n) {
    if (!ui) return 0;
    float h = 18.0f;
    float x = ui->in_panel ? ui->panel_x + ui->style.padding : 0.0f;
    float w = (ui->in_panel ? ui->panel_w - ui->style.padding * 2 : ui->width);
    float y = ui->cursor_y;
    dai_ui_advance(ui, 0, h + 3.0f);

    float bw = 22.0f;
    float px = x + w - bw * 2.0f - 1.0f, mx2 = x + w - bw;
    int add = dai_ui_icon_button_at(ui, DAI_ICON_PLUS, px, y, bw, h, 0);
    // A minus glyph the set does not have: two pixels of line, exactly where
    // the plus has its horizontal bar, so the pair reads as a pair.
    bool over_m = inside_chk(ui, mx2, y, bw, h);
    bool pressed = ui->input.mouse_down && !ui->prev.mouse_down;
    if (over_m) { ui->mouse_over_ui = true; ui->cursor_want = DAI_CURSOR_HAND; }
    dai_ui_rrect(ui, mx2, y, bw, h, ui->style.rounding,
                 over_m ? ui->style.button_hover : ui->style.button);
    dai_ui_rect(ui, mx2 + bw * 0.5f - 4.0f, y + h * 0.5f - 1.0f, 8.0f, 2.0f,
                count > min_n ? ui->style.text : ui->style.text_dim);
    int sub = (over_m && pressed) ? 1 : 0;

    if (add && count < max_n) return +1;
    if (sub && count > min_n) return -1;
    return 0;
}

// ------------------------------------------------------------ segmented

int dai_ui_segmented(dai_ui *ui, const char *const *labels, int count, int *value) {
    if (!ui || !labels || count <= 0 || !value) return 0;
    float h = widget_height(ui) + 4.0f;
    float x = ui->in_panel ? ui->panel_x + ui->style.padding : 0.0f;
    float w = (ui->in_panel ? ui->panel_w - ui->style.padding * 2 : ui->width);
    float y = ui->cursor_y;
    dai_ui_advance(ui, 0, h + ui->style.spacing);

    // One track, N cells, the selected one lifted out of it. Marking the
    // selection by wrapping the label in [brackets] is a debug print, not a
    // control - it does not say "these are the choices" at a glance.
    dai_ui_rrect(ui, x, y, w, h, ui->style.rounding + 1.0f, ui->style.track);
    dai_ui_rect_outline(ui, x, y, w, h, 1.0f, ui->style.panel_border);

    int changed = 0;
    float cw = w / (float)count;
    float lh = dai_font_line_height(ui->font);
    for (int i = 0; i < count; ++i) {
        float cx = x + cw * (float)i;
        bool over = inside_chk(ui, cx, y, cw, h);
        bool sel = (*value == i);
        if (over) { ui->mouse_over_ui = true; ui->cursor_want = DAI_CURSOR_HAND; }
        if (sel)
            dai_ui_rrect(ui, cx + 2.0f, y + 2.0f, cw - 4.0f, h - 4.0f,
                         ui->style.rounding, ui->style.button_active);
        else if (over)
            dai_ui_rrect(ui, cx + 2.0f, y + 2.0f, cw - 4.0f, h - 4.0f,
                         ui->style.rounding, ui->style.button_hover);
        const char *lbl = labels[i] ? labels[i] : "";
        float tw = dai_ui_text_width(ui, lbl);
        dai_ui_text(ui, cx + (cw - tw) * 0.5f, y + (h - lh) * 0.5f, lbl,
                    sel ? ui->style.text : ui->style.text_dim);
        if (over && ui->input.mouse_down && !ui->prev.mouse_down && *value != i) {
            *value = i;
            changed = 1;
        }
    }
    return changed;
}

// A section heading: the label, and a rule that runs to the right edge. The
// inspector already groups things; the settings page grouped nothing, so it
// read as one list of forty unrelated rows.
void dai_ui_section(dai_ui *ui, const char *title) {
    if (!ui) return;
    float x = ui->in_panel ? ui->panel_x + ui->style.padding : 0.0f;
    float w = (ui->in_panel ? ui->panel_w - ui->style.padding * 2 : ui->width);
    float y = ui->cursor_y;
    float lh = dai_font_line_height(ui->font);
    dai_ui_advance(ui, 0, lh + 12.0f);
    dai_ui_text(ui, x, y + 6.0f, title ? title : "", ui->style.text);
    float tw = dai_ui_text_width(ui, title ? title : "") + 10.0f;
    if (tw < w - 8.0f)
        dai_ui_rect(ui, x + tw, y + 6.0f + lh * 0.5f, w - tw, 1.0f, ui->style.panel_border);
}

} // extern "C"
