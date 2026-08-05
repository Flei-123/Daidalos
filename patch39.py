#!/usr/bin/env python3
# patch39 - the console filter chips: a blue glyph stopped hiding on a blue
# plate, and a coloured icon may finally be dimmed without being repainted.
import sys, os
ROOT = os.path.dirname(os.path.abspath(__file__))
def rd(p):
    with open(os.path.join(ROOT, p), 'r', encoding='utf-8') as f: return f.read()
def wr(p, s):
    full = os.path.join(ROOT, p); bak = full + '.bak_p39'
    if not os.path.exists(bak):
        with open(bak, 'w', encoding='utf-8') as f: f.write(rd(p))
    with open(full, 'w', encoding='utf-8') as f: f.write(s)
def sub1(t, old, new, what):
    if old not in t: print('MISS: ' + what); sys.exit(1)
    if t.count(old) != 1: print('AMBIG %d: %s' % (t.count(old), what)); sys.exit(1)
    return t.replace(old, new)

# ===================================================== 1. opacity vs. colour
# dai_ui_icon_at threw the caller's colour away wholesale for a coloured icon.
# That is right for the HUE - a yellow warning triangle tinted with the text
# colour is a grey triangle - and wrong for the ALPHA: "draw this at half
# strength" says nothing about hue, and without it there is no way to show a
# coloured icon in an inactive state except by not drawing it.
#
# It also read ui->icons before checking ui. Nobody hit it because every
# caller has a UI, but it is still a load through a pointer we are about to
# test for null.
s = rd('src/dai_ui.cpp')
s = sub1(s,
"""void dai_ui_icon_at(dai_ui *ui, const char *name, float x, float y,
                    float size, uint32_t color) {
    // A colored icon is its own picture; tinting it with the text color is
    // what turned the yellow warning triangle gray.
    if (ui && ui->icons && dai_icons_colored(ui->icons, name))
        color = 0xFFFFFFFFu;
    if (!ui || !ui->icons || !name) return;""",
"""void dai_ui_icon_at(dai_ui *ui, const char *name, float x, float y,
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
        color = 0x00FFFFFFu | (color & 0xFF000000u);""",
    'icon_at alpha')

# The tail then re-defaulted a zero color, which cannot happen any more.
s = sub1(s,
"""    ui->quad(ui->icon_tex, x, y, x + size, y + size, u0, v0, u1, v1,
             color ? color : ui->style.text);
}""",
"""    ui->quad(ui->icon_tex, x, y, x + size, y + size, u0, v0, u1, v1, color);
}""",
    'icon_at tail')
wr('src/dai_ui.cpp', s)

# ======================================================== 2. the chip itself
s = rd('src/dai_editor_ui.cpp')
s = sub1(s,
"""// The project window's body, so it can live in a dock panel of any size.
// The console: engine messages and script print(), filterable by level.
static void console_body(dai_editor_ui *p, float px, float py, float pw, float ph) {""",
"""// One console filter chip: the level's icon in its OWN colours, the count in
// the level's colour, on a plate that is never the level's colour.
//
// It used to be a browser_row, and a browser_row fills a SELECTED row with
// st->accent - which is blue. The info icon is also blue (COLOR_RULES paints
// it #4C8CCE) and a coloured icon refuses to be tinted, so switching the info
// filter on drew a blue glyph onto a blue plate and the chip went blank at
// exactly the moment it mattered. Warnings and errors only survived by luck
// of hue; nothing about that arrangement was deliberate.
//
// The fix is not a different blue. It is that state never rides on the fill:
// off is the chrome one step DARKER than the panel, on is one step lighter
// plus a rule underneath in the level's colour, which is the same "this tab
// is the live one" language the tab bars already speak. The glyph keeps its
// own colours in both states and only loses opacity when the filter is off -
// so the chip reads on all three of the dark themes, and would read on a
// light one, without the icon's hue entering into it at all.
static int console_chip(dai_editor_ui *p, float x, float y, float w, float h,
                        const char *icon, const char *label, uint32_t level_col,
                        int on) {
    dai_ui *ui = p->ui;
    const dai_ui_style *st = dai_ui_style_of(ui);
    float mx = 0, my = 0;
    int pressed = 0;
    dai_ui_mouse(ui, &mx, &my, nullptr, &pressed);
    bool over = mx >= x && mx < x + w && my >= y && my < y + h;
    uint32_t bg = on ? (over ? st->button_hover : st->button)
                     : (over ? st->button      : st->titlebar);
    dai_ui_rrect(ui, x, y, w, h, 4.0f, bg);
    dai_ui_rect_outline(ui, x, y, w, h, 1.0f, on ? level_col : st->panel_border);
    if (on) dai_ui_rect(ui, x + 3.0f, y + h - 3.0f, w - 6.0f, 2.0f, level_col);
    const float IS = 14.0f;
    float tx = x + 7.0f;
    if (icon && dai_ui_has_icon(ui, icon)) {
        dai_ui_icon_at(ui, icon, tx, y + (h - IS) * 0.5f, IS,
                       on ? 0xFFFFFFFFu : 0x7AFFFFFFu);
        tx += IS + 5.0f;
    }
    dai_ui_text(ui, tx, y + (h - dai_ui_text_height(ui)) * 0.5f, label,
                on ? level_col : st->text_dim);
    return over && pressed && !dai_ui_popup_active(ui);
}

// The project window's body, so it can live in a dock panel of any size.
// The console: engine messages and script print(), filterable by level.
static void console_body(dai_editor_ui *p, float px, float py, float pw, float ph) {""",
    'console_chip helper')

# The counts read in the level's colour, and info gets the icon's blue rather
# than the body text's grey - a count of zero errors and a count of zero
# infos looked identical.
s = sub1(s,
"""    static const char *LEVEL_NAME[3] = { "Info", "Warnings", "Errors" };
    const uint32_t LEVEL_COL[3] = { st->text_dim, rgba(230, 190, 90, 255), rgba(235, 105, 95, 255) };""",
"""    static const char *LEVEL_NAME[3] = { "Info", "Warnings", "Errors" };
    const uint32_t LEVEL_COL[3] = { st->text_dim, rgba(230, 190, 90, 255), rgba(235, 105, 95, 255) };
    // What the chips use. Info is the icon's own blue instead of the body
    // text's grey: on a chip, "dim grey" is what OFF already means.
    const uint32_t CHIP_COL[3] = { rgba(0x76, 0xB4, 0xF0, 255), LEVEL_COL[1], LEVEL_COL[2] };""",
    'chip colours')

s = sub1(s,
"""    static const char *LEVEL_ICON[3] = { DAI_ICON_INFO, DAI_ICON_WARNING, DAI_ICON_ERROR };
    for (int i = 0; i < 3; ++i) {
        char lbl[48];
        std::snprintf(lbl, sizeof(lbl), "%u", counts[i]);
        float w = dai_ui_text_width(ui, lbl) + 34.0f;
        if (browser_row(p, bx, py + 4.0f, w, BTN_H, LEVEL_ICON[i], lbl, p->log_show[i]))
            p->log_show[i] = !p->log_show[i];
        bx += w + 4.0f;
    }""",
"""    static const char *LEVEL_ICON[3] = { DAI_ICON_INFO, DAI_ICON_WARNING, DAI_ICON_ERROR };
    // Named while the panel is wide enough to say the words. A bare "0 0 0"
    // next to three small glyphs is a puzzle the first time you meet it, and
    // the console is usually docked across the whole bottom of the editor
    // where there is room for the answer.
    float need = 0.0f;
    for (int i = 0; i < 3; ++i) {
        char t[64];
        std::snprintf(t, sizeof(t), "%s  %u", LEVEL_NAME[i], counts[i]);
        need += dai_ui_text_width(ui, t) + 37.0f;
    }
    bool wide = bx + need < px + pw - 8.0f;
    for (int i = 0; i < 3; ++i) {
        char lbl[64];
        if (wide) std::snprintf(lbl, sizeof(lbl), "%s  %u", LEVEL_NAME[i], counts[i]);
        else      std::snprintf(lbl, sizeof(lbl), "%u", counts[i]);
        float w = dai_ui_text_width(ui, lbl) + 33.0f;
        if (console_chip(p, bx, py + 4.0f, w, BTN_H, LEVEL_ICON[i], lbl,
                         CHIP_COL[i], p->log_show[i]))
            p->log_show[i] = !p->log_show[i];
        bx += w + 4.0f;
    }""",
    'chips in console_body')
wr('src/dai_editor_ui.cpp', s)
print('patch39 ok')
