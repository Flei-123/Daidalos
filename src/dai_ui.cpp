// Immediate mode UI. See include/dai_ui.h for the design argument.
//
// No Vulkan here: this file turns widgets into triangles and nothing else.

#include "dai_ui.h"

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

    std::vector<Batch> batches;
    std::vector<dai_ui_draw> draws;

    float width = 0, height = 0;
    dai_ui_input input{};
    dai_ui_input prev{};

    // layout cursor
    float cursor_x = 0, cursor_y = 0;
    float panel_x = 0, panel_y = 0, panel_w = 0;
    bool in_panel = false;
    float row_height = 0;
    bool in_row = false;
    float row_start_y = 0;

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

    uint32_t text_cursor = 0;       // caret in the active text field
    float    drag_accum = 0.0f;     // sub-step remainder of a drag_float

    // ---- windows ----------------------------------------------------------
    struct Win {
        uint64_t id = 0;
        char     title[48] = { 0 };
        float    x = 0, y = 0, w = 0, h = 0;   // last frame's rectangle
        bool     seen = false;                 // drawn this frame
    };
    std::vector<Win> wins;          // z order: back() is in front
    int      cur_layer = 0;
    bool     blocked = false;       // the current window is behind another one
    float dock_x = 0, dock_y = 0, dock_w = 0, dock_h = 0;   // area docked windows divide
    bool  dock_area_set = false;
    uint64_t drag_win = 0;          // window being moved
    uint64_t size_win = 0;          // window being resized
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
    return ui->input.mouse_x >= x && ui->input.mouse_x < x + w &&
           ui->input.mouse_y >= y && ui->input.mouse_y < y + h;
}

} // namespace

extern "C" {

dai_ui_style dai_ui_style_default(void) {
    dai_ui_style s{};
    s.panel        = rgba(24, 26, 32, 235);
    s.panel_border = rgba(70, 78, 94, 255);
    s.text         = rgba(232, 236, 244, 255);
    s.text_dim     = rgba(150, 158, 172, 255);
    s.button       = rgba(52, 58, 72, 255);
    s.button_hover = rgba(74, 84, 104, 255);
    s.button_active= rgba(96, 140, 220, 255);
    s.accent       = rgba(96, 160, 240, 255);
    s.track        = rgba(38, 42, 52, 255);
    // Editor chrome: the surface the viewport is a hole in.
    s.titlebar         = rgba(38, 42, 52, 255);
    s.titlebar_focused = rgba(58, 74, 104, 255);
    s.chrome           = rgba(30, 32, 38, 255);
    s.shadow           = rgba(0, 0, 0, 90);
    // Compact by default. A 13 px font with 8 px of padding and 6 px between
    // every widget is not a dense editor, it is a phone app: the inspector ran
    // off the bottom of the panel with a dozen fields in it.
    s.padding = 5.0f;
    s.spacing = 3.0f;
    s.rounding = 0.0f;
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
dai_ui_style *dai_ui_style_of(dai_ui *ui) { return ui ? &ui->style : nullptr; }

void dai_ui_begin(dai_ui *ui, float width, float height, const dai_ui_input *in) {
    if (!ui) return;
    ui->prev = ui->input;
    if (in) ui->input = *in; else ui->input = dai_ui_input{};
    ui->width = width; ui->height = height;
    ui->batches.clear();
    ui->draws.clear();
    ui->cursor_x = ui->cursor_y = 0;
    ui->in_panel = ui->in_row = false;
    ui->hot = 0;
    ui->mouse_over_ui = false;
    ui->clips.clear();
    ui->scroll_stack.clear();
    ui->cur_layer = 0;
    ui->blocked = false;
    ui->win_depth = 0;
    if (!ui->dock_area_set) { ui->dock_x = 0; ui->dock_y = 0; ui->dock_w = width; ui->dock_h = height; }
    ui->dock_area_set = false;
    for (auto &w : ui->wins) w.seen = false;
    // NOTE: the active widget is cleared in dai_ui_end, not here. Clearing it
    // at the start of the frame means the widget never sees the release that
    // completes its click - which is exactly the bug the button test caught.
}

void dai_ui_end(dai_ui *ui) {
    if (!ui) return;
    if (!ui->input.mouse_down) { ui->active = 0; ui->drag_win = 0; ui->size_win = 0; }
    // A window the host stopped drawing loses its slot in the z order rather
    // than sitting there forever blocking clicks with a stale rectangle.
    ui->wins.erase(std::remove_if(ui->wins.begin(), ui->wins.end(),
                   [](const dai_ui::Win &w) { return !w.seen; }), ui->wins.end());
    // Stable, so within one layer the host's call order still decides.
    std::stable_sort(ui->batches.begin(), ui->batches.end(),
                     [](const Batch &a, const Batch &b) { return a.layer < b.layer; });
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

int dai_ui_wants_mouse(const dai_ui *ui) { return ui && ui->mouse_over_ui ? 1 : 0; }

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

// Where a docked window sits, given its own size along the free axis.
void dock_rect(const dai_ui *ui, int dock, int slot, float own_w, float own_h,
               float *x, float *y, float *w, float *h) {
    float ax = ui->dock_x, ay = ui->dock_y, aw = ui->dock_w, ah = ui->dock_h;
    if (dock == DAI_DOCK_LEFT || dock == DAI_DOCK_RIGHT) {
        float sy = ay, sh = ah;
        if (slot == 1) sh = ah * 0.5f;
        else if (slot == 2) { sy = ay + ah * 0.5f; sh = ah * 0.5f; }
        *x = (dock == DAI_DOCK_LEFT) ? ax : ax + aw - own_w;
        *y = sy; *w = own_w; *h = sh;
    } else {
        float sx = ax, sw = aw;
        if (slot == 1) sw = aw * 0.5f;
        else if (slot == 2) { sx = ax + aw * 0.5f; sw = aw * 0.5f; }
        *x = sx;
        *y = (dock == DAI_DOCK_TOP) ? ay : ay + ah - own_h;
        *w = sw; *h = own_h;
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
    } else if (ui->size_win == id && ui->input.mouse_down) {
        win->w = mx - win->x + ui->drag_dx;
        win->h = my - win->y + ui->drag_dy;
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
        dock_rect(ui, win->dock, win->dock_slot, win->w, win->h, &dx, &dy, &dw, &dh);
        win->x = dx; win->y = dy;
        if (win->dock == DAI_DOCK_LEFT || win->dock == DAI_DOCK_RIGHT) win->h = dh;
        else                                                          win->w = dw;
    }

    float body_h = win->collapsed ? 0.0f : win->h - bar;
    float full_h = bar + body_h;

    dai_ui::Win *rec = ui->find_win(id);
    if (rec) {
        rec->x = win->x; rec->y = win->y; rec->w = win->w; rec->h = full_h;
        rec->seen = true;
        std::snprintf(rec->title, sizeof(rec->title), "%s", title ? title : "");
    }

    ui->blocked = ui->covered_by_higher(id, mx, my);
    ui->cur_layer = ui->layer_of(id);
    ui->win_depth++;

    bool over_win = !ui->blocked && mx >= win->x && mx < win->x + win->w &&
                    my >= win->y && my < win->y + full_h;
    if (over_win) ui->mouse_over_ui = true;

    bool over_bar = over_win && my < win->y + bar;
    float grip = 14.0f;
    bool over_grip = over_win && !win->collapsed &&
                     mx > win->x + win->w - grip && my > win->y + full_h - grip;

    if (pressed && over_win) {
        ui->raise(id);
        ui->cur_layer = ui->layer_of(id);
        if (over_grip) {
            ui->size_win = id;
            ui->drag_dx = win->x + win->w - mx;
            ui->drag_dy = win->y + full_h - my;
        } else if (over_bar) {
            // The fold arrow is a button, not a drag handle.
            if (mx < win->x + bar) win->collapsed = !win->collapsed;
            else { ui->drag_win = id; ui->drag_dx = mx - win->x; ui->drag_dy = my - win->y; }
        }
    }

    bool focused = !ui->wins.empty() && ui->wins.back().id == id;

    // shadow, body, title bar, border - in that order
    dai_ui_rect(ui, win->x + 3.0f, win->y + 3.0f, win->w, full_h + 1.0f, st.shadow);
    if (!win->collapsed) dai_ui_rect(ui, win->x, win->y + bar, win->w, body_h, st.panel);
    dai_ui_rect(ui, win->x, win->y, win->w, bar, focused ? st.titlebar_focused : st.titlebar);
    dai_ui_rect_outline(ui, win->x, win->y, win->w, full_h, st.border,
                        focused ? st.accent : st.panel_border);

    dai_ui_text(ui, win->x + 5.0f, win->y + 3.0f, win->collapsed ? "\xe2\x96\xb8" : "\xe2\x96\xbe",
                st.text_dim);
    dai_ui_text(ui, win->x + bar, win->y + 3.0f, title ? title : "", st.text);

    if (win->collapsed) return 0;

    if (!ui->blocked) {
        dai_ui_rect(ui, win->x + win->w - grip, win->y + full_h - grip, grip - 2.0f, 2.0f,
                    over_grip ? st.accent : st.panel_border);
        dai_ui_rect(ui, win->x + win->w - 4.0f, win->y + full_h - grip, 2.0f, grip - 2.0f,
                    over_grip ? st.accent : st.panel_border);
    }

    // Where it would land if the button came up now. Drawn after the window so
    // it is visible over it - a drag with no feedback is a guess.
    if (drop_dock != DAI_DOCK_NONE) {
        float dx, dy, dw, dh;
        dock_rect(ui, drop_dock, drop_slot, win->w, win->h, &dx, &dy, &dw, &dh);
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
    if (ui->win_depth > 0) {
        ui->win_depth--;
        if (!ui->clips.empty()) ui->clips.pop_back();
    }
    ui->in_panel = false;
    ui->in_row = false;
    ui->blocked = false;
    ui->cur_layer = 0;
}

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
    if (inside(ui, x, y, w, h)) ui->mouse_over_ui = true;
}

void dai_ui_panel_end(dai_ui *ui) {
    if (!ui) return;
    ui->in_panel = false;
    if (ui->in_row) {                 // close an open row with the panel
        ui->in_row = false;
        ui->cursor_y = ui->row_start_y + ui->row_height + ui->style.spacing;
    }
}

void dai_ui_row(dai_ui *ui, float height) {
    if (!ui) return;
    ui->in_row = true;
    ui->row_height = height > 0 ? height : dai_font_line_height(ui->font) + ui->style.row_pad;
    ui->row_start_y = ui->cursor_y;
}

void dai_ui_spacing(dai_ui *ui, float px) { if (ui) ui->cursor_y += px; }

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

void dai_ui_label(dai_ui *ui, const char *utf8) {
    if (!ui) return;
    float x, y;
    next_rect(ui, 0, dai_font_line_height(ui->font), &x, &y);
    dai_ui_text(ui, x, y, utf8, ui->style.text);
}

void dai_ui_label_fmt(dai_ui *ui, const char *fmt, ...) {
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
    bool over = inside(ui, x, y, w, h);
    if (over) { ui->hot = id; ui->mouse_over_ui = true; }
    bool pressed = false;
    if (over && ui->input.mouse_down && !ui->prev.mouse_down) ui->active = id;
    if (ui->active == id && !ui->input.mouse_down) { pressed = over; ui->active = 0; }

    uint32_t col = ui->style.button;
    if (ui->active == id) col = ui->style.button_active;
    else if (over) col = ui->style.button_hover;
    dai_ui_rect(ui, x, y, w, h, col);
    float tw = dai_ui_text_width(ui, utf8);
    dai_ui_text(ui, x + (w - tw) * 0.5f, y + ui->style.row_pad * 0.5f, utf8, ui->style.text);
    return pressed ? 1 : 0;
}

int dai_ui_checkbox(dai_ui *ui, const char *utf8, int *value) {
    if (!ui || !value) return 0;
    float h = widget_height(ui);
    float x, y;
    next_rect(ui, 0, h, &x, &y);
    float box = h - 8.0f;
    uint64_t id = hash_id(utf8, x, y);
    bool over = inside(ui, x, y, box + 8.0f + dai_ui_text_width(ui, utf8), h);
    if (over) { ui->hot = id; ui->mouse_over_ui = true; }
    int changed = 0;
    if (over && ui->input.mouse_down && !ui->prev.mouse_down) { *value = !*value; changed = 1; }

    dai_ui_rect(ui, x, y + 4.0f, box, box, ui->style.track);
    dai_ui_rect_outline(ui, x, y + 4.0f, box, box, 1.0f, ui->style.panel_border);
    if (*value) dai_ui_rect(ui, x + 3.0f, y + 7.0f, box - 6.0f, box - 6.0f, ui->style.accent);
    dai_ui_text(ui, x + box + 8.0f, y + ui->style.row_pad * 0.5f, utf8, ui->style.text);
    return changed;
}

int dai_ui_slider(dai_ui *ui, const char *utf8, float *value, float min, float max) {
    if (!ui || !value || max <= min) return 0;
    float h = widget_height(ui);
    float x, y;
    next_rect(ui, 0, h, &x, &y);
    float w = (ui->in_panel ? ui->panel_w - ui->style.padding * 2 : ui->width);

    uint64_t id = hash_id(utf8, x, y);
    bool over = inside(ui, x, y, w, h);
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

int dai_ui_header(dai_ui *ui, const char *title, int *open, int *enabled) {
    if (!ui || !title) return 0;
    float h = dai_font_line_height(ui->font) + 6.0f;
    float x, y;
    next_rect(ui, 0, h, &x, &y);
    float w = (ui->in_panel ? ui->panel_w - ui->style.padding * 2 : ui->width);

    uint64_t id = hash_id(title, x, y);
    bool over = inside(ui, x, y, w, h);
    if (over) { ui->hot = id; ui->mouse_over_ui = true; }
    int result = 0;
    float box = h - 6.0f;
    bool on_box = enabled && over && ui->input.mouse_x > x + w - box - 6.0f;
    if (over && ui->input.mouse_down && !ui->prev.mouse_down) {
        if (on_box) { *enabled = !*enabled; result = 2; }
        else if (open) { *open = !*open; result = 1; }
    }

    dai_ui_rect(ui, x, y, w, h, over ? ui->style.button_hover : ui->style.button);
    dai_ui_rect(ui, x, y, 3.0f, h, ui->style.accent);
    dai_ui_text(ui, x + 8.0f, y + 2.0f, (open && !*open) ? "\xe2\x96\xb8" : "\xe2\x96\xbe",
                ui->style.text_dim);
    dai_ui_text(ui, x + 22.0f, y + 2.0f, title, ui->style.text);
    if (enabled) {
        float bx = x + w - box - 4.0f, by = y + 3.0f;
        dai_ui_rect(ui, bx, by, box, box, ui->style.track);
        dai_ui_rect_outline(ui, bx, by, box, box, 1.0f, ui->style.panel_border);
        if (*enabled) dai_ui_rect(ui, bx + 3.0f, by + 3.0f, box - 6.0f, box - 6.0f, ui->style.accent);
    }
    return result;
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

void field_rect(dai_ui *ui, const char *label, float *x, float *y, float *w, float h) {
    float rx, ry;
    next_rect(ui, 0, h, &rx, &ry);
    float full = (ui->in_panel ? ui->panel_w - ui->style.padding * 2 : ui->width);
    if (label && *label) {
        float lw = ui->style.label_w > 0 ? ui->style.label_w : 62.0f;
        dai_ui_text(ui, rx, ry + 2.0f, label, ui->style.text_dim);
        *x = rx + lw;
        *w = full - lw;
    } else {
        *x = rx;
        *w = full;
    }
    *y = ry;
}

std::string trim_number(float v) {
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%.3f", (double)v);
    std::string s(buf);
    if (s.find('.') != std::string::npos) {
        while (!s.empty() && s.back() == '0') s.pop_back();
        if (!s.empty() && s.back() == '.') s.pop_back();
    }
    if (s == "-0") s = "0";
    return s;
}

int drag_float_at(dai_ui *ui, uint64_t id, float x, float y, float w, float h,
                  float *value, float step, const char *prefix, uint32_t accent) {
    bool over = inside(ui, x, y, w, h);
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

int dai_ui_drag_float(dai_ui *ui, const char *label, float *value, float step) {
    if (!ui || !value) return 0;
    if (step <= 0.0f) step = 0.01f;
    float h = widget_height(ui), x, y, w;
    field_rect(ui, label, &x, &y, &w, h);
    return drag_float_at(ui, hash_id(label ? label : "drag", x, y), x, y, w, h,
                         value, step, nullptr, 0);
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

int dai_ui_option(dai_ui *ui, const char *label, int *value,
                  const char *const *items, int count) {
    if (!ui || !value || !items || count <= 0) return 0;
    float h = widget_height(ui), x, y, w;
    field_rect(ui, label, &x, &y, &w, h);
    uint64_t id = hash_id(label ? label : "opt", x, y);
    bool over = inside(ui, x, y, w, h);
    if (over) { ui->hot = id; ui->mouse_over_ui = true; }
    int changed = 0;
    if (over && ui->input.mouse_down && !ui->prev.mouse_down) {
        *value = (*value + 1) % count;
        changed = 1;
    }
    if (*value < 0) *value = 0;
    if (*value >= count) *value = count - 1;
    dai_ui_rect(ui, x, y + 2.0f, w, h - 4.0f, over ? ui->style.button_hover : ui->style.button);
    dai_ui_text(ui, x + 7.0f, y + ui->style.row_pad * 0.5f, items[*value], ui->style.text);
    const char *mark = "▸";
    dai_ui_text(ui, x + w - 12.0f, y + ui->style.row_pad * 0.5f, mark, ui->style.text_dim);
    return changed;
}

int dai_ui_input_text(dai_ui *ui, const char *label, char *buf, size_t buf_size) {
    if (!ui || !buf || buf_size < 2) return 0;
    float h = widget_height(ui), x, y, w;
    field_rect(ui, label, &x, &y, &w, h);
    uint64_t id = hash_id(label ? label : "text", x, y);
    bool over = inside(ui, x, y, w, h);
    if (over) { ui->hot = id; ui->mouse_over_ui = true; }
    if (ui->input.mouse_down && !ui->prev.mouse_down) {
        // Clicking anywhere else drops focus - otherwise typing would keep
        // going into a field the user has visibly left.
        if (over) { ui->active = id; ui->text_cursor = (uint32_t)std::strlen(buf); }
        else if (ui->active == id) ui->active = 0;
    }

    int changed = 0;
    if (ui->active == id) {
        size_t len = std::strlen(buf);
        if (ui->input.key_backspace && len > 0) {
            // Step back over a whole UTF-8 sequence, not one byte, or a
            // backspace on a non ASCII name leaves a broken code point behind.
            size_t n = 1;
            while (n < len && ((unsigned char)buf[len - n] & 0xC0) == 0x80) ++n;
            buf[len - n] = 0;
            changed = 1;
        }
        for (int i = 0; i < 8 && ui->input.text[i]; ++i) {
            uint32_t cp = ui->input.text[i];
            char enc[5];
            int n = 0;
            if (cp < 0x80) { enc[n++] = (char)cp; }
            else if (cp < 0x800) { enc[n++] = (char)(0xC0 | (cp >> 6)); enc[n++] = (char)(0x80 | (cp & 0x3F)); }
            else if (cp < 0x10000) { enc[n++] = (char)(0xE0 | (cp >> 12)); enc[n++] = (char)(0x80 | ((cp >> 6) & 0x3F)); enc[n++] = (char)(0x80 | (cp & 0x3F)); }
            else { enc[n++] = (char)(0xF0 | (cp >> 18)); enc[n++] = (char)(0x80 | ((cp >> 12) & 0x3F)); enc[n++] = (char)(0x80 | ((cp >> 6) & 0x3F)); enc[n++] = (char)(0x80 | (cp & 0x3F)); }
            enc[n] = 0;
            size_t cur = std::strlen(buf);
            if (cur + (size_t)n + 1 <= buf_size) { std::memcpy(buf + cur, enc, (size_t)n + 1); changed = 1; }
        }
        if (ui->input.key_enter) ui->active = 0;
    }

    bool focused = (ui->active == id);
    dai_ui_rect(ui, x, y + 2.0f, w, h - 4.0f, focused ? ui->style.button_active : ui->style.track);
    dai_ui_rect_outline(ui, x, y + 2.0f, w, h - 4.0f, 1.0f,
                        focused ? ui->style.accent : ui->style.panel_border);
    dai_ui_text(ui, x + 7.0f, y + ui->style.row_pad * 0.5f, buf, ui->style.text);
    if (focused) {
        float caret = x + 7.0f + dai_ui_text_width(ui, buf) + 1.0f;
        dai_ui_rect(ui, caret, y + 5.0f, 1.5f, h - 10.0f, ui->style.accent);
    }
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
    bool over = inside(ui, x, y, w, h);
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
    if (has_children && open)
        dai_ui_text(ui, x + indent + 2.0f, y + 1.0f, *open ? "▾" : "▸", ui->style.text_dim);
    dai_ui_text(ui, x + indent + arrow_w + 2.0f, y + 1.0f, label,
                selected ? ui->style.text : ui->style.text);
    return clicked;
}

// ------------------------------------------------------------- scrolling

void dai_ui_scroll_begin(dai_ui *ui, const char *id_str, float height) {
    if (!ui) return;
    float x, y;
    next_rect(ui, 0, height, &x, &y);
    float w = (ui->in_panel ? ui->panel_w - ui->style.padding * 2 : ui->width);
    uint64_t id = hash_id(id_str ? id_str : "scroll", x, y);
    float &off = ui->scroll_of(id);

    if (inside(ui, x, y, w, height)) {
        ui->mouse_over_ui = true;
        if (ui->input.wheel != 0.0f) off -= ui->input.wheel * 32.0f;
    }
    if (off < 0.0f) off = 0.0f;

    ui->scroll_stack.push_back(dai_ui::ScrollFrame{ id, x, y, w, height, y });
    ui->clips.push_back(dai_ui::Clip{ x, y, x + w, y + height });
    // The layout continues inside the region, shifted by the scroll offset.
    ui->cursor_x = x;
    ui->cursor_y = y - off;
}

void dai_ui_scroll_end(dai_ui *ui) {
    if (!ui || ui->scroll_stack.empty()) return;
    dai_ui::ScrollFrame f = ui->scroll_stack.back();
    ui->scroll_stack.pop_back();
    if (!ui->clips.empty()) ui->clips.pop_back();

    float &off = ui->scroll_of(f.id);
    float content = (ui->cursor_y + off) - f.start_y;
    float max_off = content - f.h;
    if (max_off < 0.0f) max_off = 0.0f;
    if (off > max_off) off = max_off;

    if (max_off > 0.0f) {
        float track_x = f.x + f.w - 4.0f;
        float frac = f.h / (content > 0 ? content : 1.0f);
        float bar_h = f.h * (frac > 1 ? 1 : frac);
        if (bar_h < 16.0f) bar_h = 16.0f;
        float t = off / max_off;
        dai_ui_rect(ui, track_x, f.y, 4.0f, f.h, ui->style.track);
        dai_ui_rect(ui, track_x, f.y + (f.h - bar_h) * t, 4.0f, bar_h, ui->style.button_hover);
    }
    ui->cursor_x = ui->in_panel ? ui->panel_x + ui->style.padding : 0.0f;
    ui->cursor_y = f.y + f.h + ui->style.spacing;
}

// ---------------------------------------------------------------- sprites

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
    bool over = inside(ui, x, y, w, h);
    if (over) { ui->hot = id; ui->mouse_over_ui = true; }
    bool pressed = false;
    if (over && ui->input.mouse_down && !ui->prev.mouse_down) ui->active = id;
    if (ui->active == id && !ui->input.mouse_down) { pressed = over; ui->active = 0; }

    dai_ui_rect(ui, x - 2, y - 2, w + 4, h + 4,
                ui->active == id ? ui->style.button_active : over ? ui->style.button_hover : ui->style.button);
    ui->quad(tex, x, y, x + w, y + h, u0, v0, u1, v1, 0xFFFFFFFFu);
    return pressed ? 1 : 0;
}

} // extern "C"
