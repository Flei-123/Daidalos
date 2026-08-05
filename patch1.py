import re, sys, io

def rw(p):
    return io.open(p, encoding='utf-8').read()
def wr(p, s):
    io.open(p, 'w', encoding='utf-8').write(s)

def sub1(s, old, new, tag):
    if old not in s:
        print("MISS", tag); sys.exit(1)
    if s.count(old) != 1:
        print("AMBIG", tag, s.count(old)); sys.exit(1)
    print("ok", tag)
    return s.replace(old, new)

# ---------------------------------------------------------------- dai_ui.cpp
P = 'src/dai_ui.cpp'
s = rw(P)

# 1) field_rect reports the label rect, so a caller can turn the label into a
#    drag handle. Unity drags the LABEL of every numeric field, not just X/Y/Z.
s = sub1(s,
"""void field_rect(dai_ui *ui, const char *label, float *x, float *y, float *w, float h) {
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
}""",
"""// Where the label of the last field_rect went. Reported separately instead of
// through more out parameters because every caller wants the field rect and
// only the numeric ones want the label: the label is a DRAG HANDLE (Unity
// scrubs a value by dragging its name), and a widget that cannot report where
// its name is cannot offer that.
float g_label_x = 0.0f, g_label_y = 0.0f, g_label_w = 0.0f;

void field_rect(dai_ui *ui, const char *label, float *x, float *y, float *w, float h) {
    float rx, ry;
    next_rect(ui, 0, h, &rx, &ry);
    float full = (ui->in_panel ? ui->panel_w - ui->style.padding * 2 : ui->width);
    g_label_x = rx; g_label_y = ry; g_label_w = 0.0f;
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
}""", "field_rect+label_scrub")

# 2) every num_field gets it
s = sub1(s,
"""int dai_ui_num_field(dai_ui *ui, const char *label, float *value,
                     float step, float min, float max, const char *id) {
    if (!ui || !value) return 0;
    float h = widget_height(ui), x, y, w;
    field_rect(ui, label, &x, &y, &w, h);
    return num_field_at(ui, x, y, w, h, value, step, min, max,
                        id ? id : (label ? label : "num"), true, 0, nullptr);
}""",
"""int dai_ui_num_field(dai_ui *ui, const char *label, float *value,
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
}""", "num_field label scrub")

# 3) drag_float / vec3 label scrub as well (they share field_rect)
s = sub1(s,
"""int dai_ui_drag_float(dai_ui *ui, const char *label, float *value, float step) {
    if (!ui || !value) return 0;
    if (step <= 0.0f) step = 0.01f;
    float h = widget_height(ui), x, y, w;
    field_rect(ui, label, &x, &y, &w, h);
    return drag_float_at(ui, hash_id(label ? label : "drag", x, y), x, y, w, h,
                         value, step, nullptr, 0);
}""",
"""int dai_ui_drag_float(dai_ui *ui, const char *label, float *value, float step) {
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
}""", "drag_float label scrub")

# 4) text height, public. The dock draws its own tab bar and has to centre a
#    label in it; without this it hardcoded +3.0f and clipped descenders.
s = sub1(s,
"""float dai_ui_text_width(dai_ui *ui, const char *utf8) {""",
"""float dai_ui_text_height(dai_ui *ui) {
    return ui ? dai_font_line_height(ui->font) : 0.0f;
}

float dai_ui_text_width(dai_ui *ui, const char *utf8) {""", "text_height")

wr(P, s)

P = 'include/dai_ui.h'
s = rw(P)
s = sub1(s,
"DAI_API float dai_ui_text_width(dai_ui *ui, const char *utf8);",
"""DAI_API float dai_ui_text_width(dai_ui *ui, const char *utf8);
/* Line height of the current font. Host chrome that lays itself out (the dock
 * tab bar, the console button row) needs it to centre a label - a hardcoded
 * offset is right at one font size and clips descenders at every other. */
DAI_API float dai_ui_text_height(dai_ui *ui);""", "text_height decl")
wr(P, s)

# --------------------------------------------------------------- dai_dock.cpp
P = 'src/dai_dock.cpp'
s = rw(P)

s = sub1(s,
"const float TAB_H    = 21.0f;   // height of a tab bar",
"""// Tall enough that a 13 px line with descenders fits with air above and below:
// 21 clipped the tail of every 'p' and 'y' in a tab title, which is what "the
// tabs are cut off at the bottom" was.
const float TAB_H    = 26.0f;   // height of a tab bar""", "TAB_H")

s = sub1(s,
"""        dai_ui_rrect_mask(ui, tx, r.y, tw - 1.0f, TAB_H, 5.0f, bg, 0x3);
        if (sel && focused) dai_ui_rect(ui, tx, r.y, tw - 1.0f, 2.0f, st->accent);
        dai_ui_text(ui, tx + 8.0f, r.y + 3.0f, t.c_str(), sel ? st->text : st->text_dim);""",
"""        // The tab body starts 3 px down: a tab that touches the top edge of
        // its bar has no room to look rounded, which is why the corners were
        // invisible no matter what radius they were given.
        float ty = r.y + 3.0f, th = TAB_H - 3.0f;
        dai_ui_rrect_mask(ui, tx, ty, tw - 2.0f, th, 7.0f, bg, 0x3);
        // The accent stripe is INSET, or it paints square corners back over
        // the round ones it is supposed to sit on.
        if (sel && focused)
            dai_ui_rrect_mask(ui, tx + 6.0f, ty, tw - 14.0f, 2.0f, 1.0f, st->accent, 0x3);
        float lh = dai_ui_text_height(ui);
        dai_ui_text(ui, tx + 9.0f, ty + (th - lh) * 0.5f, t.c_str(),
                    sel ? st->text : st->text_dim);""", "tab draw")

wr(P, s)
print("patch1 done")
