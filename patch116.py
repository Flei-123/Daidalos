import io

# ===========================================================================
# Ein Farbwaehler. Drei Zahlen namens X, Y und Z sind keine Farbe - man kann
# sie lesen und weiss trotzdem nicht, wie es aussieht.
# ===========================================================================
p = 'include/dai_ui.h'
s = io.open(p, encoding='utf-8').read()
old = """DAI_API void dai_ui_help(dai_ui *ui, const char *text);"""
new = """/* A colour, as a colour. `rgb` is three floats 0..1, edited in place; returns
 * 1 on any frame it changed.
 *
 * It exists because "Color X 0.82 Y 0.24 Z 0.2" is not a colour, it is three
 * numbers wearing a colour's name - you can read all three and still not know
 * what it looks like. The row shows a SWATCH; clicking it opens the wheel,
 * the value bar, and RGB/HSV/Hex, which is the arrangement Blender, Krita and
 * every paint program settled on independently.
 *
 * `id` has to be unique per field, like every other stateful widget here. */
DAI_API int  dai_ui_color(dai_ui *ui, const char *label, float *rgb, const char *id);

DAI_API void dai_ui_help(dai_ui *ui, const char *text);"""
assert s.count(old) == 1, 'help decl not found'
s = s.replace(old, new)
io.open(p, 'w', encoding='utf-8').write(s)

p = 'src/dai_ui.cpp'
s = io.open(p, encoding='utf-8').read()
old = """void dai_ui_help(dai_ui *ui, const char *text) {"""
new = """// ---------------------------------------------------------------- colour
//
// The wheel is hue by angle and saturation by radius, drawn as a triangle fan
// so it costs one draw call and no texture. Value gets its own bar on the
// right, because a wheel that dims with V is a wheel you cannot pick a dark
// colour on - the ring you want goes black before you reach it.
namespace {

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
    const int SEG = 48, RINGS = 6;
    for (int i = 0; i < SEG; ++i) {
        float a0 = (float)i / SEG * 6.2831853f, a1 = (float)(i + 1) / SEG * 6.2831853f;
        for (int rr = 0; rr < RINGS; ++rr) {
            float r0 = rad * (float)rr / RINGS, r1 = rad * (float)(rr + 1) / RINGS;
            float c0[3], c1[3];
            hsv_to_rgb((float)i / SEG, (float)rr / RINGS, v, c0);
            hsv_to_rgb((float)i / SEG, (float)(rr + 1) / RINGS, v, c1);
            uint32_t col = pack_rgb(c1, 1.0f);
            dai_ui_quad_raw(ui, cx + std::cos(a0) * r0, cy + std::sin(a0) * r0,
                                cx + std::cos(a1) * r0, cy + std::sin(a1) * r0,
                                cx + std::cos(a1) * r1, cy + std::sin(a1) * r1,
                                cx + std::cos(a0) * r1, cy + std::sin(a0) * r1, col);
            (void)c0;
        }
    }
    // The marker sits where the current colour is.
    {
        float ang = h * 6.2831853f, rr = sv * rad;
        float mx2 = cx + std::cos(ang) * rr, my2 = cy + std::sin(ang) * rr;
        dai_ui_circle_outline(ui, mx2, my2, 5.0f, 2.0f, 0xFF000000u);
        dai_ui_circle_outline(ui, mx2, my2, 6.5f, 1.5f, 0xFFFFFFFFu);
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
    for (int i = 0; i < 32; ++i) {
        float t0 = (float)i / 32.0f, t1 = (float)(i + 1) / 32.0f;
        float c[3];
        hsv_to_rgb(h, sv, 1.0f - t1, c);
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
            dai_ui_num_field_at(ui, ui->color_mode == 0 ? names_rgb[i] : names_hsv[i],
                                &vals[i], 0.005f, 0.0f, 1.0f, fid,
                                px + 8.0f, ny, w - 16.0f, rh - 2.0f);
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

void dai_ui_help(dai_ui *ui, const char *text) {"""
assert s.count(old) == 1, 'help def not found'
s = s.replace(old, new)

old = """    int code_focus = 0;"""
new = """    int code_focus = 0;
    uint64_t color_open = 0;      // which colour field has its panel down
    int      color_drag = 0;      // 0 none, 1 wheel, 2 value bar
    int      color_mode = 0;      // 0 RGB, 1 HSV, 2 Hex"""
assert s.count(old) == 1, 'code_focus member not found'
s = s.replace(old, new)
io.open(p, 'w', encoding='utf-8').write(s)
print('dai_ui_color written')
