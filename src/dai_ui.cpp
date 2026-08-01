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

    // An open menu swallows every hit test outside its own rectangle - see
    // dai_ui_popup_menu. popup_was_open is written at the end of a frame and
    // read at the start of the next, so the click that dismisses the menu
    // cannot fall through to whatever was underneath it.
    bool in_popup = false;
    bool popup_was_open = false;
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
    s.button       = rgba(0x50, 0x50, 0x50, 255);
    s.button_hover = rgba(0x5D, 0x5D, 0x5D, 255);
    s.button_active= rgba(0x2C, 0x5D, 0x87, 255);   // Unity's selection blue
    s.accent       = rgba(0x2C, 0x5D, 0x87, 255);
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
    ui->cursor_x = ui->cursor_y = 0;
    ui->in_panel = ui->in_row = false;
    ui->hot = 0;
    ui->mouse_over_ui = false;
    ui->clips.clear();
    ui->scroll_stack.clear();
    ui->cur_layer = 0;
    ui->blocked = false;
    ui->win_depth = 0;
    ui->tooltip_on = false;
    ui->cursor_want = DAI_CURSOR_ARROW;
    // An open popup swallows every hit test below its own layer. The flag
    // comes from the END of the previous frame, so the click that closes the
    // menu cannot also press the button it lands on.
    ui->in_popup = ui->popup_was_open;
    ui->popup_was_open = false;
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
    if (!ui->input.mouse_down) {
        ui->active = 0; ui->drag_win = 0; ui->size_win = 0; ui->size_edge = 0;
        ui->edit.dragging = false;
    }
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

void dai_ui_claim_mouse(dai_ui *ui) { if (ui) ui->mouse_over_ui = true; }
int  dai_ui_cursor(const dai_ui *ui) { return ui ? ui->cursor_want : DAI_CURSOR_ARROW; }
void dai_ui_cursor_set(dai_ui *ui, int cursor) { if (ui) ui->cursor_want = cursor; }
int  dai_ui_text_active(const dai_ui *ui) { return ui && ui->edit.editing ? 1 : 0; }

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
    ui->cur_layer = ui->layer_of(id);
    ui->win_depth++;

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

    dai_ui_text(ui, win->x + 5.0f, win->y + 3.0f, win->collapsed ? "\xe2\x96\xb8" : "\xe2\x96\xbe",
                st.text_dim);
    // A viewport window's bar belongs to its tabs - printing the title there
    // too would draw "Scene" under the Scene tab.
    if (!win->viewport)
        dai_ui_text(ui, win->x + bar, win->y + 3.0f, title ? title : "", st.text);

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

void dai_ui_spacing(dai_ui *ui, float px) { if (ui) ui->cursor_y += px; }

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
    bool over = inside_chk(ui, x, y, w, h);
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

int dai_ui_toggle_button(dai_ui *ui, const char *utf8, int active) {
    if (!ui || !utf8) return 0;
    float h = widget_height(ui), x, y;
    next_rect(ui, 0, h, &x, &y);
    float w = (ui->in_panel ? ui->panel_w - ui->style.padding * 2 : ui->width);
    uint64_t id = hash_id(utf8, x, y);
    bool over = inside_chk(ui, x, y, w, h);
    if (over) { ui->hot = id; ui->mouse_over_ui = true; }
    int pressed = 0;
    if (over && ui->input.mouse_down && !ui->prev.mouse_down) ui->active = id;
    if (ui->active == id && !ui->input.mouse_down) { pressed = over ? 1 : 0; ui->active = 0; }
    dai_ui_rect(ui, x, y + 1.0f, w, h - 2.0f,
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
    uint64_t id = hash_id(utf8, x, y);
    bool over = inside_chk(ui, x, y, box + 8.0f + dai_ui_text_width(ui, utf8), h);
    if (over) { ui->hot = id; ui->mouse_over_ui = true; }
    int changed = 0;
    if (over && ui->input.mouse_down && !ui->prev.mouse_down) { *value = !*value; changed = 1; }

    dai_ui_rect(ui, x, y + 4.0f, box, box, over ? ui->style.button_hover : ui->style.track);
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
    float u0, v0, u1, v1;
    if (!dai_icons_uv(ui->icons, name, &u0, &v0, &u1, &v1)) return;
    if (size <= 0.0f) size = dai_icons_size(ui->icons);
    // Snapped to whole pixels. Half a pixel of offset on a 16 px icon made of
    // 1.3 px strokes is the difference between a crisp line and a grey smear.
    x = std::floor(x + 0.5f);
    y = std::floor(y + 0.5f);
    ui->quad(ui->icon_tex, x, y, x + size, y + size, u0, v0, u1, v1,
             color ? color : ui->style.text);
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
    dai_ui_rect(ui, x, y, w, h, bg);

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

int dai_ui_header_icon(dai_ui *ui, const char *icon, const char *title,
                       int *open, int *enabled) {
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
        dai_ui_text(ui, tx, y + 2.0f, folded ? "\xe2\x96\xb8" : "\xe2\x96\xbe", ui->style.text_dim);
        tx += 14.0f;
    }
    if (icon && dai_ui_has_icon(ui, icon)) {
        dai_ui_icon_at(ui, icon, tx, y + (h - isz) * 0.5f, isz, ui->style.accent);
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
    bool over = inside_chk(ui, x, y, w, h);
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
    // A chevron, drawn - the "?" the old "▸" glyph fell back to looked like an
    // error, and it was one: the font simply does not have that code point.
    float cx = x + w - 12.0f, cy = y + h * 0.5f;
    dai_ui_line(ui, cx - 3.0f, cy - 2.5f, cx + 1.0f, cy + 1.5f, 1.5f, ui->style.text_dim);
    dai_ui_line(ui, cx + 1.0f, cy + 1.5f, cx + 5.0f, cy - 2.5f, 1.5f, ui->style.text_dim);
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

int dai_ui_text_field(dai_ui *ui, const char *id_str, float x, float y, float w, float h,
                      char *buf, size_t buf_size, int *commit) {
    if (commit) *commit = 0;
    if (!ui || !buf || buf_size < 2) return 0;
    uint64_t id = hash_id(id_str ? id_str : "field", x, y);
    bool editing = ui->edit.editing && ui->edit.id == id;
    dai_ui_rect(ui, x, y, w, h, ui->style.track);
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
        if (on_arrow) *open = !*open;
        else clicked = 1;
    }
    // The right button folds nothing and selects nothing - it is the menu's
    // button, and the caller decides what the menu says.
    if (over && ui->input.right_down && !ui->prev.right_down) clicked |= 2;

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
            dai_ui_text(ui, x + indent + 2.0f, y + 1.0f, *open ? "\xe2\x96\xbe" : "\xe2\x96\xb8",
                        ui->style.text_dim);
        }
    }
    dai_ui_text(ui, x + indent + arrow_w + 2.0f, y + 1.0f, label, ui->style.text);
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
    next_rect(ui, 0, height, &x, &y);
    float w = (ui->in_panel ? ui->panel_w - ui->style.padding * 2 : ui->width);
    uint64_t id = hash_id(id_str ? id_str : "scroll", x, y);
    float &off = ui->scroll_of(id);

    if (inside_chk(ui, x, y, w, height)) {
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

void dai_ui_popup_open(dai_ui_popup *m, float x, float y) {
    if (!m) return;
    m->x = x; m->y = y; m->open = 1;
}

void dai_ui_popup_close(dai_ui_popup *m) { if (m) m->open = 0; }

int dai_ui_popup_menu(dai_ui *ui, dai_ui_popup *m,
                      const dai_ui_menu_item *items, uint32_t count) {
    if (!ui || !m || !m->open || !items || !count) return -2;

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

    if (ui->input.mouse_down && !ui->prev.mouse_down && !over) {
        m->open = 0;
        result = -1;                    // dismissed
    }

    dai_ui_rect(ui, x + 2.0f, y + 3.0f, w, h, ui->style.shadow);
    dai_ui_rect(ui, x, y, w, h, ui->style.panel);
    dai_ui_rect_outline(ui, x, y, w, h, 1.0f, ui->style.panel_border);

    int hovered = -1;
    if (over && result == -2)
        hovered = (int)((my - y - pad) / row_h);

    for (uint32_t i = 0; i < count; ++i) {
        float ry = y + pad + row_h * (float)i;
        if ((int)i == hovered)
            dai_ui_rect(ui, x + 1.0f, ry, w - 2.0f, row_h, ui->style.button_hover);
        float tx = x + 8.0f;
        if (items[i].icon && dai_ui_has_icon(ui, items[i].icon)) {
            float isz = row_h - 8.0f;
            dai_ui_icon_at(ui, items[i].icon, tx, ry + 4.0f, isz, ui->style.text_dim);
            tx += isz + 6.0f;
        }
        dai_ui_text(ui, tx, ry + 4.0f, items[i].label, ui->style.text);
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
    return num_field_at(ui, x, y, w, h, value, step, min, max,
                        id ? id : (label ? label : "num"), true, 0, nullptr);
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

} // extern "C"
