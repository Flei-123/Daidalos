#!/usr/bin/env python3
# patch36 - menus that stay put, a keyboard the camera can take back, a
# segmented control instead of [brackets], and a toolbar that reads.
import sys, os
ROOT = os.path.dirname(os.path.abspath(__file__))
def rd(p):
    with open(os.path.join(ROOT, p), 'r', encoding='utf-8') as f: return f.read()
def wr(p, s):
    full = os.path.join(ROOT, p); bak = full + '.bak_p36'
    if not os.path.exists(bak):
        with open(bak, 'w', encoding='utf-8') as f: f.write(rd(p))
    with open(full, 'w', encoding='utf-8') as f: f.write(s)
def sub1(t, old, new, what):
    if old not in t: print('MISS: ' + what); sys.exit(1)
    if t.count(old) != 1: print('AMBIG %d: %s' % (t.count(old), what)); sys.exit(1)
    return t.replace(old, new)

# =================================================== 1. the menu that followed
# Yesterday's fix pulled the menu under the pointer so it could not dismiss
# itself on the press that opened it. It did that on EVERY frame - so the menu
# then followed the mouse around the screen. Place it once, when it opens.
s = rd('include/dai_ui.h')
s = sub1(s, "typedef struct dai_ui_popup {",
            "typedef struct dai_ui_popup {\n    int      placed;     /* 0 until the first draw has put it on screen */",
         'popup placed field')
wr('include/dai_ui.h', s)

s = rd('src/dai_ui.cpp')
s = sub1(s,
"""    // Never let the menu land beside the pointer: it is dismissed by a press
    // that is not over it, and the press that OPENED it is still the current
    // one on the frame it first draws.
    {
        float mx0 = ui->input.mouse_x, my0 = ui->input.mouse_y;
        if (mx0 >= 0.0f && my0 >= 0.0f) {""",
"""    // Never let the menu land beside the pointer: it is dismissed by a press
    // that is not over it, and the press that OPENED it is still the current
    // one on the frame it first draws. ONCE, on the first frame - doing it
    // every frame is a menu that follows the mouse around the screen.
    if (!m->placed) {
        float mx0 = ui->input.mouse_x, my0 = ui->input.mouse_y;
        if (mx0 >= 0.0f && my0 >= 0.0f) {""",
'popup place once')
s = sub1(s,
"""            if (x < 0) x = 0;
            if (y < 0) y = 0;
        }
    }

    int result = -2;""",
"""            if (x < 0) x = 0;
            if (y < 0) y = 0;
        }
        m->x = x; m->y = y;      // frozen: the menu does not move again
        m->placed = 1;
    }

    int result = -2;""",
'popup freeze')
s = sub1(s,
"""void dai_ui_popup_open(dai_ui_popup *m, float x, float y) {
    if (!m) return;
    m->x = x; m->y = y; m->open = 1;
}""",
"""void dai_ui_popup_open(dai_ui_popup *m, float x, float y) {
    if (!m) return;
    m->x = x; m->y = y; m->open = 1; m->placed = 0;
}""",
'popup open resets')

# The searchlist had the same job and the same fix: it is placed where it was
# opened, and it stays there.
s = sub1(s,
"""    float x = s->x, y = s->y;
    if (x + W > ui->width - 4.0f) x = ui->width - 4.0f - W;
    if (x < 4.0f) x = 4.0f;""",
"""    float x = s->x, y = s->y;
    if (x + W > ui->width - 4.0f) x = ui->width - 4.0f - W;
    if (x < 4.0f) x = 4.0f;
    s->x = x;                    // remember it: a list that re-derives its
                                 // place every frame drifts with the panel""",
'searchlist stable x')

# ============================================ 2. the keyboard, back to the camera
s = sub1(s, "int  dai_ui_text_active(const dai_ui *ui) { return ui && ui->edit.editing ? 1 : 0; }",
"""int  dai_ui_text_active(const dai_ui *ui) { return ui && ui->edit.editing ? 1 : 0; }
// Give the keyboard back. A field keeps focus until something takes it, and
// "something" has to include the scene view: W A S D typed into an invisible
// text box is the bug where the camera turns but never moves.
void dai_ui_text_defocus(dai_ui *ui) {
    if (!ui) return;
    ui->edit.editing = false;
    ui->edit.id = 0;
}""",
'defocus impl')

# ================================================== 3. the segmented control
MARKER = '} // extern "C"'
idx = s.rfind(MARKER)
assert idx > 0
s = s[:idx] + """// ------------------------------------------------------------ segmented

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

""" + s[idx:]
wr('src/dai_ui.cpp', s)

s = rd('include/dai_ui.h')
s = sub1(s, "DAI_API void dai_ui_separator(dai_ui *ui);",
"""DAI_API void dai_ui_separator(dai_ui *ui);
/* One track, N cells, one of them lit: the control every settings page has
 * and the reason none of them writes [Preferences] to mark the current one.
 * Returns 1 the frame the choice changes. */
DAI_API int  dai_ui_segmented(dai_ui *ui, const char *const *labels, int count, int *value);
/* A heading with a rule running off to the right. */
DAI_API void dai_ui_section(dai_ui *ui, const char *title);
/* Drop the keyboard focus. The scene view calls this when a camera gesture
 * starts: a text field that still owns W A S D is a camera that only turns. */
DAI_API void dai_ui_text_defocus(dai_ui *ui);""",
'segmented decl')
wr('include/dai_ui.h', s)

# ======================================================== 4. the editor side
s = rd('src/dai_editor_ui.cpp')
s = sub1(s,
"""    dai_ui_row(ui, 22.0f);
    if (dai_ui_button(ui, p->settings_tab == 0 ? "[Preferences]" : "Preferences")) p->settings_tab = 0;
    if (dai_ui_button(ui, p->settings_tab == 1 ? "[Project Settings]" : "Project Settings")) p->settings_tab = 1;
    if (dai_ui_button(ui, p->settings_tab == 2 ? "[Gizmos]" : "Gizmos")) p->settings_tab = 2;
    dai_ui_row_end(ui);
    dai_ui_separator(ui);""",
"""    {
        static const char *const TABS[] = { "Preferences", "Project", "Gizmos" };
        dai_ui_segmented(ui, TABS, 3, &p->settings_tab);
    }""",
'settings segmented')
s = sub1(s, """    dai_ui_label(ui, "Appearance");""", """    dai_ui_section(ui, "Appearance");""", 'appearance section')

# A camera gesture takes the keyboard back.
s = sub1(s,
"""    int cam_used = dai_editor_cam_update(p->ed, &ci);""",
"""    // The right button in the scene view means "I am flying now": whatever
    // text field still had the keyboard gives it up, or W A S D go on being
    // typed into it and the camera only ever turns.
    if (ci.mouse_right && !p->prev_right_down && !over_ui) dai_ui_text_defocus(p->ui);
    int cam_used = dai_editor_cam_update(p->ed, &ci);""",
'defocus on fly')

# The Window button gets its own icon, and a Focus button lands next to it.
s = sub1(s,
"""    if (dai_ui_icon_button(p->ui, DAI_ICON_LAYERS, "Window", p->menu_window.open)) {""",
"""    if (dai_ui_icon_button(p->ui, DAI_ICON_WINDOW, "Window", p->menu_window.open)) {""",
'window icon')
s = sub1(s,
"""    // Layouts: several named arrangements, Unity's layout dropdown.
    dai_ui_toolbar_gap(p->ui, 10.0f);""",
"""    // Frame the selection, the way F does in the viewport - and reveal it in
    // the hierarchy, because "where is that object" is two questions.
    dai_ui_toolbar_gap(p->ui, 10.0f);
    if (dai_ui_icon_button(p->ui, DAI_ICON_TARGET, "Focus selection (F)", 0) &&
        dai_editor_selection_count(p->ed) > 0) {
        dai_editor_cam_focus(p->ed);
        p->reveal_selection = 1;
    }

    // Layouts: several named arrangements, Unity's layout dropdown.
    dai_ui_toolbar_gap(p->ui, 10.0f);""",
'focus button')
s = sub1(s, "    int   fold_materials = 1;      // the Materials array, open like Unity's",
            "    int   fold_materials = 1;      // the Materials array, open like Unity's\n"
            "    int   reveal_selection = 0;    // scroll the hierarchy to the selection",
         'reveal field')
wr('src/dai_editor_ui.cpp', s)

# the window icon
s = rd('include/dai_icons.h')
s = sub1(s, '#define DAI_ICON_LAYERS     "layers"',
            '#define DAI_ICON_LAYERS     "layers"\n#define DAI_ICON_WINDOW     "window"',
         'window icon name')
wr('include/dai_icons.h', s)

s = rd('src/dai_icons.cpp')
s = sub1(s, '{ "minus", SOLID_HEAD',
"""{ "window", STROKE_HEAD
  "<rect x='2.5' y='3.5' width='19' height='17' rx='2'/>"
  "<line x1='2.5' y1='8.5' x2='21.5' y2='8.5'/><line x1='9.5' y1='8.5' x2='9.5' y2='20.5'/>" TAIL },
{ "minus", SOLID_HEAD""",
'window icon svg')
wr('src/dai_icons.cpp', s)
print('patch36 ok')
