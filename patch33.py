#!/usr/bin/env python3
# patch33 - the black Settings panel, pickers that closed themselves, the
# scrollbar sitting on top of the buttons, active vs visible, escape.
import sys, os
ROOT = os.path.dirname(os.path.abspath(__file__))
def rd(p):
    with open(os.path.join(ROOT, p), 'r', encoding='utf-8') as f: return f.read()
def wr(p, s):
    full = os.path.join(ROOT, p); bak = full + '.bak_p33'
    if not os.path.exists(bak):
        with open(bak, 'w', encoding='utf-8') as f: f.write(rd(p))
    with open(full, 'w', encoding='utf-8') as f: f.write(s)
def sub1(t, old, new, what):
    if old not in t: print('MISS: ' + what); sys.exit(1)
    if t.count(old) != 1: print('AMBIG %d: %s' % (t.count(old), what)); sys.exit(1)
    return t.replace(old, new)

# ==================================================== 1. the black Settings
# dai_dock_add put the tab back into the tree but never took the title off the
# CLOSED list - which is saved with the layout. The tab bar then drew a tab
# that dai_dock_panel refused to hand a body to: a panel you can select and
# that stays black for ever. One line, and it survived a restart.
s = rd('src/dai_dock.cpp')
s = sub1(s,
"""void dai_dock_add(dai_dock *d, const char *title, int edge, float fraction) {
    if (!d || !title || !*title) return;""",
"""void dai_dock_add(dai_dock *d, const char *title, int edge, float fraction) {
    if (!d || !title || !*title) return;
    // Anything being ADDED is by definition not closed. Leaving it on the
    // closed list gave a tab in the tree that dai_dock_panel refuses - the
    // Settings panel that opened as a black rectangle, permanently, because
    // the closed list is saved with the layout.
    for (size_t i = 0; i < d->closed.size(); ++i)
        if (d->closed[i] == title) { d->closed.erase(d->closed.begin() + (long)i); break; }""",
'dock add unclose')

# The same trap one level down: a tab dropped into a leaf, or split off into
# its own, must not stay "closed" either.
s = sub1(s,
"""void add_tab_to(Node *leaf, const std::string &title, bool select, int at = -1) {""",
"""void unclose(dai_dock *d, const std::string &title);

void add_tab_to(Node *leaf, const std::string &title, bool select, int at = -1) {""",
'dock unclose decl')
s = sub1(s,
"""void enforce_pair(dai_dock *d, const std::string &keep) {""",
"""void unclose(dai_dock *d, const std::string &title) {
    for (size_t i = 0; i < d->closed.size(); ++i)
        if (d->closed[i] == title) { d->closed.erase(d->closed.begin() + (long)i); return; }
}

void enforce_pair(dai_dock *d, const std::string &keep) {""",
'dock unclose impl')
wr('src/dai_dock.cpp', s)

# ============================================== 2. pickers that closed at once
# An object field opened its picker on the PRESS. The menu was then drawn in
# the same frame, saw that same press, decided it was a click outside itself
# (a menu narrower than the 150 px it was offset by never contains the
# pointer) and dismissed itself. One frame of menu, every time.
#
# Fixed at both ends: the field reports on RELEASE like every other button,
# and a menu opens where the pointer can actually be inside it.
s = rd('src/dai_ui.cpp')
s = sub1(s,
"""    bool over_f = inside_chk(ui, x, y, fw, h);
    bool over_b = inside_chk(ui, x + fw + 2.0f, y, bw, h);
    bool pressed = ui->input.mouse_down && !ui->prev.mouse_down;
    if (over_f || over_b) { ui->mouse_over_ui = true; ui->cursor_want = DAI_CURSOR_HAND; }""",
"""    bool over_f = inside_chk(ui, x, y, fw, h);
    bool over_b = inside_chk(ui, x + fw + 2.0f, y, bw, h);
    uint64_t oid = hash_id(label ? label : "objfield", x, y);
    if (over_f || over_b) { ui->mouse_over_ui = true; ui->cursor_want = DAI_CURSOR_HAND; }
    if ((over_f || over_b) && ui->input.mouse_down && !ui->prev.mouse_down) ui->active = oid;
    bool pressed = false;
    if (ui->active == oid && !ui->input.mouse_down) {
        pressed = over_f || over_b;
        ui->active = 0;
    }""",
'object field release')
s = sub1(s,
"""    dai_ui_text(ui, tx, y + (h - dai_font_line_height(ui->font)) * 0.5f,
                value ? value : "None", ui->style.text);

    if (fw < w) {""",
"""    dai_ui_text(ui, tx, y + (h - dai_font_line_height(ui->font)) * 0.5f,
                value ? value : "None", ui->style.text);

    if (fw < w) {""",
'noop keep')
s = sub1(s, "    return (over_f || over_b) && pressed ? 1 : 0;\n}\n\nvoid dai_ui_searchlist_open",
            "    return pressed ? 1 : 0;\n}\n\nvoid dai_ui_searchlist_open",
         'object field return')

# the array row, same rule
s = sub1(s,
"""    bool over_f = inside_chk(ui, fx, y, vw, h);
    bool over_b = bw > 0.0f && inside_chk(ui, fx + vw + 2.0f, y, bw, h);
    bool pressed = ui->input.mouse_down && !ui->prev.mouse_down;
    if (over_f || over_b) { ui->mouse_over_ui = true; ui->cursor_want = DAI_CURSOR_HAND; }""",
"""    bool over_f = inside_chk(ui, fx, y, vw, h);
    bool over_b = bw > 0.0f && inside_chk(ui, fx + vw + 2.0f, y, bw, h);
    char rid[24];
    std::snprintf(rid, sizeof(rid), "arrrow%d", index);
    uint64_t aid = hash_id(rid, fx, y);
    if (over_f || over_b) { ui->mouse_over_ui = true; ui->cursor_want = DAI_CURSOR_HAND; }
    if ((over_f || over_b) && ui->input.mouse_down && !ui->prev.mouse_down) ui->active = aid;
    bool pressed = false;
    if (ui->active == aid && !ui->input.mouse_down) { pressed = over_f || over_b; ui->active = 0; }""",
'array row release')
s = sub1(s, "    return (over_f || over_b) && pressed ? 1 : 0;\n}\n\nint dai_ui_array_end",
            "    return pressed ? 1 : 0;\n}\n\nint dai_ui_array_end",
         'array row return')

# A menu whose top left is 150 px left of the pointer but which is only 130 px
# wide does not contain the pointer. Put the corner AT the pointer and let the
# screen clamp move it - then "outside" always means outside.
s = sub1(s,
"""    float x = m->x, y = m->y;
    if (x + w > ui->width) x = ui->width - w - 4.0f;
    if (y + h > ui->height) y = ui->height - h - 4.0f;
    if (x < 0) x = 0;
    if (y < 0) y = 0;""",
"""    float x = m->x, y = m->y;
    if (x + w > ui->width) x = ui->width - w - 4.0f;
    if (y + h > ui->height) y = ui->height - h - 4.0f;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    // Never let the menu land beside the pointer: it is dismissed by a press
    // that is not over it, and the press that OPENED it is still the current
    // one on the frame it first draws.
    {
        float mx0 = ui->input.mouse_x, my0 = ui->input.mouse_y;
        if (mx0 >= 0.0f && my0 >= 0.0f) {
            if (mx0 < x)          x = mx0 - 6.0f;
            if (mx0 >= x + w)     x = mx0 - w + 6.0f;
            if (my0 < y)          y = my0 - 6.0f;
            if (my0 >= y + h)     y = my0 - h + 6.0f;
            if (x < 0) x = 0;
            if (y < 0) y = 0;
        }
    }""",
'popup follows pointer')

# The search list has to block the widgets under it too, or the same press
# that picks a material also re-opens the picker beneath the list.
s = sub1(s,
"""    // Keep the whole list above every panel it was opened from.
    dai_ui_layer_push(ui, DAI_LAYER_POPUP);""",
"""    ui->popup_was_open = true;      // next frame's widgets underneath are dead
    ui->mouse_over_ui = true;
    // Keep the whole list above every panel it was opened from.
    dai_ui_layer_push(ui, DAI_LAYER_POPUP);""",
'searchlist blocks')

# =============================== 3. the scrollbar sat on top of the buttons
# The bar is drawn at the right edge of the region and the widgets were laid
# out to the FULL width - so the material picker's target button, the +/- pair
# and every dropdown arrow shared their last ten pixels with it. Reserve the
# strip in the layout instead.
s = sub1(s,
"""    ui->scroll_stack.push_back(dai_ui::ScrollFrame{ id, x, y, w, height, y });""",
"""    ui->scroll_stack.push_back(dai_ui::ScrollFrame{ id, x, y, w, height, y });
    // The bar lives in the last ten pixels; nothing else may be laid out
    // there or every field's button ends up underneath it.
    ui->scroll_saved_panel_w.push_back(ui->panel_w);
    if (ui->in_panel && ui->panel_w > 60.0f) ui->panel_w -= 10.0f;""",
'scroll reserve')
s = sub1(s,
"""    dai_ui::ScrollFrame f = ui->scroll_stack.back();
    ui->scroll_stack.pop_back();""",
"""    dai_ui::ScrollFrame f = ui->scroll_stack.back();
    ui->scroll_stack.pop_back();
    if (!ui->scroll_saved_panel_w.empty()) {
        ui->panel_w = ui->scroll_saved_panel_w.back();
        ui->scroll_saved_panel_w.pop_back();
    }""",
'scroll restore')
s = sub1(s, "    uint64_t scroll_drag_id = 0;   // the bar the pointer is holding",
            "    std::vector<float> scroll_saved_panel_w;   // width the region borrowed from\n    uint64_t scroll_drag_id = 0;   // the bar the pointer is holding",
         'scroll saved field')
s = sub1(s, "    ui->scroll_stack.clear();",
            "    ui->scroll_stack.clear();\n    ui->scroll_saved_panel_w.clear();",
         'scroll saved clear')
wr('src/dai_ui.cpp', s)

# ================================================ 4. active is not invisible
s = rd('include/dai_doc.h')
s = sub1(s, """    int      hidden;

    uint32_t user_data;""",
"""    int      hidden;            /* the RENDERER is off: the object still
                                   exists, still collides, still runs its
                                   scripts. This is Unity's MeshRenderer
                                   checkbox, not the object's.                */
    int      disabled;          /* the OBJECT is off: nothing is drawn, no
                                   body, no scripts. Unity's checkbox next to
                                   the name. Sharing one flag with `hidden` is
                                   what made "activate the camera" switch the
                                   mesh renderer on.                          */

    uint32_t user_data;""",
'doc disabled field')
wr('include/dai_doc.h', s)

s = rd('src/dai_doc_text.cpp')
s = sub1(s, '        if (r.hidden != def.hidden)             put(s, "  hidden %d\\n", r.hidden);',
            '        if (r.hidden != def.hidden)             put(s, "  hidden %d\\n", r.hidden);\n'
            '        if (r.disabled != def.disabled)         put(s, "  disabled %d\\n", r.disabled);',
         'doc text write')
s = sub1(s, '        else if (key == "hidden") { ok = parse_i32(after, &rec.hidden); }',
            '        else if (key == "hidden") { ok = parse_i32(after, &rec.hidden); }\n'
            '        else if (key == "disabled") { ok = parse_i32(after, &rec.disabled); }',
         'doc text read')
wr('src/dai_doc_text.cpp', s)

s = rd('src/dai_doc_sync.cpp')
s = sub1(s, "    d.invisible = r.hidden;",
            "    d.invisible = r.hidden || r.disabled;",
         'sync invisible')
s = sub1(s, "            dai_scene_set_visible(s->scene, l.entity, !r.hidden);",
            "            dai_scene_set_visible(s->scene, l.entity, !(r.hidden || r.disabled));",
         'sync set visible')
s = sub1(s, "    return r.no_body || (r.no_collider && r.no_rigidbody);",
            "    // A disabled object has no physics at all - that is what the\n"
            "    // checkbox next to the name means everywhere else.\n"
            "    return r.disabled || r.no_body || (r.no_collider && r.no_rigidbody);",
         'sync physicsless')
s = sub1(s, "    return a.shape != b.shape || a.motion != b.motion || a.no_body != b.no_body ||",
            "    return a.disabled != b.disabled ||\n"
            "           a.shape != b.shape || a.motion != b.motion || a.no_body != b.no_body ||",
         'sync rebuild on disable')
wr('src/dai_doc_sync.cpp', s)

# ============================================== 5. the inspector, once more
s = rd('src/dai_editor_ui.cpp')
s = sub1(s,
"""        {
            int on = !r.hidden;
            if (on) {""",
"""        {
            int on = !r.disabled;
            if (on) {""",
'header checkbox state')
s = sub1(s,
"""            if (over_box && p2)
                r.hidden = !r.hidden;""",
"""            // The OBJECT, not its renderer. They were the same flag, which
            // is why ticking a camera back on switched its Mesh Renderer on.
            if (over_box && p2)
                r.disabled = !r.disabled;""",
'header checkbox toggle')

# Colour belongs to the material now.
i0 = s.index("        // Show what the object IS, not what the document happens to store. A")
i1 = s.index("        // ---- Materials, Unity's array")
s = s[:i0] + """        // Colour used to sit here. It belongs to the MATERIAL - a surface
        // property next to the material that owns it is two places the same
        // fact can disagree, and the array below is where materials live.
""" + s[i1:]

# A prefab reads blue, like Unity's.
s = sub1(s, '    if (e == "daidalos" || e == "prefab")             return DAI_ICON_SCENE;',
            '    if (e == "daidalos" || e == "prefab")             return DAI_ICON_C_PREFAB;',
         'prefab icon')

# Settings: the button opens AND focuses the tab it already is.
s = sub1(s,
"""    if (dai_ui_icon_button(p->ui, DAI_ICON_SETTINGS, "Settings", p->settings_open))
        p->settings_open = !p->settings_open;""",
"""    if (dai_ui_icon_button(p->ui, DAI_ICON_SETTINGS, "Settings", p->settings_open)) {
        // Wherever it is - a tab behind another one, a floating window, or
        // nowhere yet - one click brings it to the front. Toggling a flag and
        // hoping was how it ended up selected but never focused.
        p->settings_open = 1;
        dai_dock_open(p->dock, "Settings");
        dai_dock_focus(p->dock, "Settings");
    }""",
'settings button')

# The status bar says what the pixels really are.
s = sub1(s,
"""    std::snprintf(right, sizeof(right), "viewport %.0fx%.0f", p->view_w, p->view_h);""",
"""    {
        float sc = dai_ui_scale_get(p->ui);
        if (sc > 1.01f || sc < 0.99f)
            std::snprintf(right, sizeof(right), "viewport %.0fx%.0f  (%.0fx%.0f px, UI %.0f%%)",
                          p->view_w, p->view_h, p->view_w * sc, p->view_h * sc, sc * 100.0f);
        else
            std::snprintf(right, sizeof(right), "viewport %.0fx%.0f", p->view_w, p->view_h);
    }""",
'status bar scale')

# The browser rows: the label sat on a fixed 3 px offset, so it drifted down
# out of the row as soon as the font grew.
s = sub1(s,
"""    dai_ui_text(ui, tx, y + 3.0f, label, st->text);
    return over && pressed && !dai_ui_popup_active(ui);""",
"""    dai_ui_text(ui, tx, y + (h - dai_ui_text_height(ui)) * 0.5f, label, st->text);
    return over && pressed && !dai_ui_popup_active(ui);""",
'browser row centre')
wr('src/dai_editor_ui.cpp', s)

# The blue prefab tile.
s = rd('include/dai_icons.h')
s = sub1(s, '#define DAI_ICON_C_SPRITE    "c-sprite"',
            '#define DAI_ICON_C_SPRITE    "c-sprite"\n#define DAI_ICON_C_PREFAB    "c-prefab"',
         'prefab icon name')
wr('include/dai_icons.h', s)

s = rd('src/dai_icons.cpp')
s = sub1(s, '{ "folder", STROKE_HEAD',
"""{ "c-prefab", FILL_HEAD
  "<rect x='2' y='2' width='20' height='20' rx='5' fill='#3B7DD8'/>"
  "<path d='M12 5.6 18.6 9.2v6.4L12 19.2 5.4 15.6V9.2z' fill='#FFFFFF'/>"
  "<circle cx='12' cy='12.4' r='2.1' fill='#3B7DD8'/>" TAIL },
{ "folder", STROKE_HEAD""",
'prefab tile')
wr('src/dai_icons.cpp', s)

# ==================================================== 6. escape is not quit
s = rd('src/rhi_vulkan_window_win32.cpp')
s = sub1(s,
"""        set_key(w, wp, true);
        if (wp == VK_ESCAPE) w->open = false;
        return 0;""",
"""        set_key(w, wp, true);
        // Escape is NOT quit. It cancels a menu, a rename, a drag - and an
        // editor that shuts down when you back out of a text field loses
        // work for a living.
        return 0;""",
'no escape quit')
wr('src/rhi_vulkan_window_win32.cpp', s)
print('patch33 ok')
