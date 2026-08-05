#!/usr/bin/env python3
# patch28 - DPI aware UI.
#
# The editor is DPI aware on Windows, so it renders into REAL pixels: on a
# 125% display 13 px of font is 10.4 "desktop" pixels. That is why the whole
# interface looked small and soft next to the vector icons - the icons are
# rasterised from paths, the text is not.
#
# The fix is one global scale. Everything the editor lays out stays in LOGICAL
# units; the font and the icons are rasterised at logical * scale REAL pixels
# and report logical metrics; the UI's vertex list is multiplied by the scale
# on the way out. Text lands on the pixel grid 1:1 and the layout keeps its
# proportions.
import re, sys, os

ROOT = os.path.dirname(os.path.abspath(__file__))

def rd(p):
    with open(os.path.join(ROOT, p), 'r', encoding='utf-8') as f: return f.read()

def wr(p, s):
    full = os.path.join(ROOT, p)
    bak = full + '.bak_p28'
    if not os.path.exists(bak):
        with open(bak, 'w', encoding='utf-8') as f: f.write(rd(p))
    with open(full, 'w', encoding='utf-8') as f: f.write(s)

def sub1(text, old, new, what):
    if old not in text:
        print('MISS: ' + what); sys.exit(1)
    if text.count(old) != 1:
        print('AMBIGUOUS (%d): %s' % (text.count(old), what)); sys.exit(1)
    return text.replace(old, new)

# ---------------------------------------------------------------- dai_font.h
s = rd('include/dai_font.h')
s = sub1(s,
"DAI_API dai_font *dai_font_load_ui(float pixel_height, char *err, size_t err_len);",
"""DAI_API dai_font *dai_font_load_ui(float pixel_height, char *err, size_t err_len);

/* The same font, rasterised for a scaled display. `pixel_height` is the size
 * the LAYOUT wants (13 px of interface); `scale` is the desktop's, so the
 * glyphs are rasterised at pixel_height * scale REAL pixels and every metric
 * this font reports is divided back down. The caller keeps laying out in 13 px
 * and the texture holds 16 - which is the only arrangement where a 125%
 * display gets sharp text without a different layout. */
DAI_API dai_font *dai_font_load_ui_scaled(float pixel_height, float scale,
                                          char *err, size_t err_len);""",
'font.h decl')
wr('include/dai_font.h', s)

# -------------------------------------------------------------- dai_font.cpp
s = rd('src/dai_font.cpp')
s = sub1(s,
"dai_font *dai_font_load_ui(float pixel_height, char *err, size_t err_len) {",
"""/* Divide every reported metric by `div`, leaving the ATLAS alone: the glyphs
 * keep their real pixels, the layout gets logical ones. f->scale is only read
 * by line_height/ascent after this point, so scaling it here is enough. */
static void font_to_logical(dai_font *f, float div) {
    if (!f || div <= 1.0001f) return;
    float inv = 1.0f / div;
    for (auto &kv : f->glyphs) {
        dai_glyph &g = kv.second;
        g.x0 *= inv; g.y0 *= inv; g.x1 *= inv; g.y1 *= inv; g.advance *= inv;
    }
    f->scale *= inv;
}

dai_font *dai_font_load_ui_scaled(float pixel_height, float scale,
                                  char *err, size_t err_len) {
    if (!(scale > 0.0f)) scale = 1.0f;
    /* Whole real pixels: a 16.25 px raster puts every baseline half a texel
     * off the grid, which is the blur this whole change exists to remove. */
    float real = (float)(int)(pixel_height * scale + 0.5f);
    if (real < 2.0f) real = 2.0f;
    dai_font *f = dai_font_load_ui(real, err, err_len);
    if (!f) return nullptr;
    font_to_logical(f, real / pixel_height);
    return f;
}

dai_font *dai_font_load_ui(float pixel_height, char *err, size_t err_len) {""",
'font.cpp impl')
wr('src/dai_font.cpp', s)

# --------------------------------------------------------------- dai_icons.h
s = rd('include/dai_icons.h')
s = sub1(s,
"DAI_API float    dai_icons_size(const dai_icons *ic);",
"""DAI_API float    dai_icons_size(const dai_icons *ic);
/* What the LAYOUT should treat the icons as being, when they were rasterised
 * larger for a scaled display: create at 16 * 1.25 real pixels, display at 16.
 * Zero (the default) means "the size they were rasterised at". */
DAI_API void     dai_icons_display_size(dai_icons *ic, float logical_px);""",
'icons.h decl')
wr('include/dai_icons.h', s)

s = rd('src/dai_icons.cpp')
s = sub1(s, "    uint32_t aw = 0, ah = 0;\n    bool dirty = true;\n};",
            "    uint32_t aw = 0, ah = 0;\n    float disp = 0.0f;   // logical size, 0 = same as `size`\n    bool dirty = true;\n};",
         'icons struct')
s = sub1(s,
"float    dai_icons_size(const dai_icons *ic)  { return ic ? ic->size : 0.0f; }",
"""float    dai_icons_size(const dai_icons *ic)  {
    return ic ? (ic->disp > 0.0f ? ic->disp : ic->size) : 0.0f;
}
void dai_icons_display_size(dai_icons *ic, float logical_px) {
    if (ic) ic->disp = logical_px > 0.0f ? logical_px : 0.0f;
}""",
'icons size')
wr('src/dai_icons.cpp', s)

# ------------------------------------------------------------------ dai_ui.h
s = rd('include/dai_ui.h')
s = sub1(s,
"DAI_API void dai_ui_begin(dai_ui *ui, float width, float height, const dai_ui_input *in);",
"""DAI_API void dai_ui_begin(dai_ui *ui, float width, float height, const dai_ui_input *in);
/* Display scale. The UI keeps laying itself out in LOGICAL pixels - the host
 * feeds it a logical size and a logical pointer - and the vertex list that
 * comes out is multiplied by this on the way to the renderer. 1.25 on a 125%
 * desktop; the font and the icons must be rasterised at that scale too, or
 * the text is simply magnified. */
DAI_API void  dai_ui_scale_set(dai_ui *ui, float scale);
DAI_API float dai_ui_scale_get(const dai_ui *ui);""",
'ui.h scale')
wr('include/dai_ui.h', s)

# ---------------------------------------------------------------- dai_ui.cpp
s = rd('src/dai_ui.cpp')
s = sub1(s, "uint32_t dai_ui_draws(dai_ui *ui, const dai_ui_draw **out) {",
"""void dai_ui_scale_set(dai_ui *ui, float scale) {
    if (!ui) return;
    if (!(scale > 0.05f) || scale > 8.0f) scale = 1.0f;
    ui->out_scale = scale;
}
float dai_ui_scale_get(const dai_ui *ui) { return ui ? ui->out_scale : 1.0f; }

uint32_t dai_ui_draws(dai_ui *ui, const dai_ui_draw **out) {""",
'ui.cpp scale api')

# the field
s = sub1(s, "    std::vector<ScrollFrame> scroll_stack;",
            "    std::vector<ScrollFrame> scroll_stack;\n    float out_scale = 1.0f;   // logical -> real pixels, applied at the very end",
         'ui.cpp scale field')

# apply on the way out
s = sub1(s,
"""    for (Batch &b : ui->batches) {
        if (b.verts.empty()) continue;
        dai_ui_draw d{};""",
"""    // Logical pixels became real pixels here, once, for everything: scaling
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
        dai_ui_draw d{};""",
'ui.cpp scale apply')
wr('src/dai_ui.cpp', s)

# ------------------------------------------------------------- editor_demo.cpp
s = rd('examples/editor_demo.cpp')
s = sub1(s,
"static float        *g_ui_scale_out = nullptr;   // prefs.ui_scale lives in main()",
"""static float        *g_ui_scale_out = nullptr;   // prefs.ui_scale lives in main()
// The desktop's scale (1.25 at 125%) times whatever the user chose in
// Settings. Everything the editor lays out is in logical pixels; this is the
// only number that turns them into the real ones the window is made of.
static float         g_dpi = 1.0f;""",
'demo globals')

s = sub1(s,
"""    dai_font *nf = dai_font_load_ui(px, err, sizeof(err));
    if (!nf) { std::printf("font reload failed: %s\\n", err); return; }""",
"""    dai_font *nf = dai_font_load_ui_scaled(px, g_dpi, err, sizeof(err));
    if (!nf) { std::printf("font reload failed: %s\\n", err); return; }""",
'demo apply_font')

s = sub1(s,
"""    dai_font *font = dai_font_load_ui(13.0f, err, sizeof(err));""",
"""    // The desktop's scale factor, asked for ONCE the window exists. 13 px of
    // interface is 13 px of interface at every zoom level; what changes is how
    // many real pixels the glyphs are rasterised into.
    g_dpi = dai_window_dpi_scale(win);
    if (!(g_dpi > 0.5f) || g_dpi > 4.0f) g_dpi = 1.0f;
    std::printf("display scale: %.2f\\n", g_dpi);
    dai_font *font = dai_font_load_ui_scaled(13.0f, g_dpi, err, sizeof(err));""",
'demo font load')

s = sub1(s,
"""    dai_icons *icons = dai_icons_create(16.0f);""",
"""    dai_icons *icons = dai_icons_create(16.0f * g_dpi);
    if (icons) dai_icons_display_size(icons, 16.0f);""",
'demo icons')

s = sub1(s,
"""    dai_ui *ui = dai_ui_create(font, font_tex);""",
"""    dai_ui *ui = dai_ui_create(font, font_tex);
    dai_ui_scale_set(ui, g_dpi);""",
'demo ui scale')

# ---- the frame: logical everywhere, real only where the renderer is fed
s = sub1(s,
"""        if (ww != dai_render_width(r) || wh != dai_render_height(r)) {
            if (dai_render_resize(r, ww, wh) == DAI_OK)
                dai_editor_camera_viewport(ed, (float)ww, (float)wh);""",
"""        // Logical size: the renderer owns real pixels, the interface owns
        // logical ones, and the scale is the only bridge between them.
        const float uis = g_dpi;
        const float lw = (float)ww / uis, lh = (float)wh / uis;
        if (ww != dai_render_width(r) || wh != dai_render_height(r)) {
            if (dai_render_resize(r, ww, wh) == DAI_OK)
                dai_editor_camera_viewport(ed, lw, lh);""",
'demo resize')

s = sub1(s,
"""            dai_window_mouse(win, &amx, &amy, nullptr);
            dai_editor_cam_anchor(ed, (float)amx, (float)amy);""",
"""            dai_window_mouse(win, &amx, &amy, nullptr);
            dai_editor_cam_anchor(ed, (float)amx / uis, (float)amy / uis);""",
'demo anchor')

s = sub1(s,
"""        dai_ui_begin(ui, (float)ww, (float)wh, &in);""",
"""        dai_ui_begin(ui, lw, lh, &in);""",
'demo ui_begin')
s = sub1(s,
"""        dai_editor_ui_frame(panels, (float)ww, (float)wh);""",
"""        dai_editor_ui_frame(panels, lw, lh);""",
'demo ui_frame')
s = sub1(s,
"""        float vrx = 0, vry = 0, vrw = (float)ww, vrh = (float)wh;""",
"""        float vrx = 0, vry = 0, vrw = lw, vrh = lh;""",
'demo vr')
s = sub1(s,
"""        dai_render_world_clip(r, vrx, vry, vrw, vrh);""",
"""        dai_render_world_clip(r, vrx * uis, vry * uis, vrw * uis, vrh * uis);""",
'demo world clip')
s = sub1(s,
"""                    dai_render_world_clip2(r, gx, gy, gw, gh);""",
"""                    dai_render_world_clip2(r, gx * uis, gy * uis, gw * uis, gh * uis);""",
'demo world clip2')

# The pointer the UI and the editor see is logical.
s = sub1(s,
"""        dai_editor_cam_input ci{};
        ci.mouse_x = (float)mx; ci.mouse_y = (float)my;""",
"""        dai_editor_cam_input ci{};
        // Logical pointer: everything downstream of here - the UI, the gizmo,
        // picking - lays out in logical pixels, and a pointer in real ones
        // would miss every widget by the scale factor.
        ci.mouse_x = (float)mx / g_dpi; ci.mouse_y = (float)my / g_dpi;""",
'demo ci mouse')
s = sub1(s,
"""        in.mouse_x = (float)mx; in.mouse_y = (float)my;""",
"""        in.mouse_x = (float)mx / uis; in.mouse_y = (float)my / uis;""",
'demo in mouse')

wr('examples/editor_demo.cpp', s)
print('patch28 ok')
