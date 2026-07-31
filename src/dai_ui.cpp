// Immediate mode UI. See include/dai_ui.h for the design argument.
//
// No Vulkan here: this file turns widgets into triangles and nothing else.

#include "dai_ui.h"

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
};

uint32_t rgba(int r, int g, int b, int a) {
    return (uint32_t)((a << 24) | (b << 16) | (g << 8) | r);
}

} // namespace

struct dai_ui {
    dai_font *font = nullptr;
    dai_texture font_tex = 0;
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

    Batch &batch(dai_texture tex) {
        if (batches.empty() || batches.back().texture != tex) {
            batches.push_back(Batch{});
            batches.back().texture = tex;
        }
        return batches.back();
    }

    void quad(dai_texture tex, float x0, float y0, float x1, float y1,
              float u0, float v0, float u1, float v1, uint32_t col) {
        Batch &b = batch(tex);
        dai_ui_vertex v[6] = {
            { x0, y0, u0, v0, col }, { x1, y0, u1, v0, col }, { x1, y1, u1, v1, col },
            { x0, y0, u0, v0, col }, { x1, y1, u1, v1, col }, { x0, y1, u0, v1, col },
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
    s.padding = 8.0f;
    s.spacing = 6.0f;
    s.rounding = 0.0f;
    s.border = 1.0f;
    return s;
}

dai_ui *dai_ui_create(dai_font *font, dai_texture font_texture) {
    dai_ui *ui = new dai_ui();
    ui->font = font;
    ui->font_tex = font_texture;
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
    // NOTE: the active widget is cleared in dai_ui_end, not here. Clearing it
    // at the start of the frame means the widget never sees the release that
    // completes its click - which is exactly the bug the button test caught.
}

void dai_ui_end(dai_ui *ui) {
    if (!ui) return;
    if (!ui->input.mouse_down) ui->active = 0;      // release ends any drag
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

int dai_ui_wants_mouse(const dai_ui *ui) { return ui && ui->mouse_over_ui ? 1 : 0; }

// ---------------------------------------------------------------- drawing

void dai_ui_rect(dai_ui *ui, float x, float y, float w, float h, uint32_t color) {
    if (!ui || w <= 0 || h <= 0) return;
    // a 1x1 white texel would be nicer, but the font atlas has an empty border
    // pixel we can point at instead: no extra texture, no state change
    ui->quad(ui->font_tex, x, y, x + w, y + h, 0.0f, 0.0f, 0.0f, 0.0f, color);
}

void dai_ui_rect_outline(dai_ui *ui, float x, float y, float w, float h, float t, uint32_t color) {
    if (!ui) return;
    dai_ui_rect(ui, x, y, w, t, color);
    dai_ui_rect(ui, x, y + h - t, w, t, color);
    dai_ui_rect(ui, x, y, t, h, color);
    dai_ui_rect(ui, x + w - t, y, t, h, color);
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
            ui->quad(ui->font_tex, pen_x + g->x0, pen_y - g->y1, pen_x + g->x1, pen_y - g->y0,
                     g->u0, g->v0, g->u1, g->v1, color);
        pen_x += g->advance;
    }
}

float dai_ui_text_width(dai_ui *ui, const char *utf8) {
    return (ui && ui->font) ? dai_font_measure(ui->font, utf8, nullptr) : 0.0f;
}

// ---------------------------------------------------------------- layout

void dai_ui_panel_begin(dai_ui *ui, float x, float y, float w, float h, const char *title) {
    if (!ui) return;
    dai_ui_rect(ui, x, y, w, h, ui->style.panel);
    if (ui->style.border > 0) dai_ui_rect_outline(ui, x, y, w, h, ui->style.border, ui->style.panel_border);
    ui->panel_x = x; ui->panel_y = y; ui->panel_w = w;
    ui->cursor_x = x + ui->style.padding;
    ui->cursor_y = y + ui->style.padding;
    ui->in_panel = true;
    if (title && *title) {
        dai_ui_text(ui, ui->cursor_x, ui->cursor_y, title, ui->style.text);
        ui->cursor_y += dai_font_line_height(ui->font) + ui->style.spacing;
        dai_ui_rect(ui, x + ui->style.padding, ui->cursor_y - ui->style.spacing * 0.5f,
                    w - ui->style.padding * 2, 1.0f, ui->style.panel_border);
    }
    if (inside(ui, x, y, w, h)) ui->mouse_over_ui = true;
}

void dai_ui_panel_end(dai_ui *ui) { if (ui) ui->in_panel = false; }

void dai_ui_row(dai_ui *ui, float height) {
    if (!ui) return;
    ui->in_row = true;
    ui->row_height = height > 0 ? height : dai_font_line_height(ui->font) + 8.0f;
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

float widget_height(dai_ui *ui) { return dai_font_line_height(ui->font) + 8.0f; }

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
    dai_ui_text(ui, x + (w - tw) * 0.5f, y + 4.0f, utf8, ui->style.text);
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
    dai_ui_text(ui, x + box + 8.0f, y + 4.0f, utf8, ui->style.text);
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
    dai_ui_text(ui, x, y + 4.0f, buf, ui->style.text_dim);
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
        dai_ui_text(ui, x + (w - tw) * 0.5f, y + 4.0f, utf8, ui->style.text);
    }
}

void dai_ui_separator(dai_ui *ui) {
    if (!ui) return;
    float x, y;
    next_rect(ui, 0, 1.0f, &x, &y);
    float w = (ui->in_panel ? ui->panel_w - ui->style.padding * 2 : ui->width);
    dai_ui_rect(ui, x, y, w, 1.0f, ui->style.panel_border);
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
