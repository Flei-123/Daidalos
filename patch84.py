#!/usr/bin/env python3
# patch84 - the caret still stopped the wheel, and Settings gets an About tab.
import sys, os
ROOT = os.path.dirname(os.path.abspath(__file__))
def rd(p):
    with open(os.path.join(ROOT, p), 'r', encoding='utf-8') as f: return f.read()
def wr(p, s):
    full = os.path.join(ROOT, p); bak = full + '.bak_p84'
    if not os.path.exists(bak):
        with open(bak, 'w', encoding='utf-8') as f: f.write(rd(p))
    with open(full, 'w', encoding='utf-8') as f: f.write(s)
def sub1(t, old, new, what):
    if new in t: print('skip (already applied): ' + what); return t
    if old not in t: print('MISS: ' + what); sys.exit(1)
    if t.count(old) != 1: print('AMBIG %d: %s' % (t.count(old), what)); sys.exit(1)
    return t.replace(old, new)

s = rd('src/dai_ui.cpp')

# Comparing this frame's caret with last frame's was the wrong test: anything
# that touches the caret at all - including the drag handler re-deriving it
# from a mouse position that has not moved - reads as "the caret moved", and
# the view snaps back. The reveal now happens only when an INPUT moved it,
# recorded where that input is handled, and a wheel turn cancels it outright:
# scrolling is the user saying where they want to look, and nothing in the
# same frame gets to overrule that.
s = sub1(s,
"""    if (over) {
        ui->cursor_want = DAI_CURSOR_TEXT;
        st->scroll_y -= ui->input.wheel * LH * 3.0f;
    }""",
"""    bool user_scrolled = false;
    if (over) {
        ui->cursor_want = DAI_CURSOR_TEXT;
        if (ui->input.wheel != 0.0f) {
            st->scroll_y -= ui->input.wheel * LH * 3.0f;
            user_scrolled = true;
        }
    }""",
    'wheel marks itself')

s = sub1(s,
"""    // ---- keyboard -----------------------------------------------------------
    int changed = 0;""",
"""    // ---- keyboard -----------------------------------------------------------
    int changed = 0;
    // Set by the handlers below when they move the caret on purpose. This is
    // the only thing that makes the view follow it.
    bool caret_input = false;""",
    'caret_input flag')

s = sub1(s,
"""    if (st->focused) {
        const dai_ui_input &in = ui->input;
        bool shift = in.key_shift != 0;
        int before = st->caret;""",
"""    if (st->focused) {
        const dai_ui_input &in = ui->input;
        bool shift = in.key_shift != 0;
        int before = st->caret;
        if (in.text[0] || in.key_enter || in.key_tab || in.key_backspace ||
            in.key_delete || in.key_left || in.key_right || in.key_home ||
            in.key_end || in.key_up_arrow || in.key_down_arrow ||
            in.key_paste || in.key_cut)
            caret_input = true;""",
    'keyboard sets caret_input')

s = sub1(s,
"""    if (pressed && over && mx > x + GUT) {
        st->caret = st->anchor = offset_at(mx, my);
        st->dragging = 1;""",
"""    if (pressed && over && mx > x + GUT) {
        st->caret = st->anchor = offset_at(mx, my);
        st->dragging = 1;
        st->follow_caret = 1;          // a click is a place you asked to be""",
    'click sets follow')

s = sub1(s,
"""    bool caret_moved = !st->have_last || st->last_caret != st->caret || changed;
    st->last_caret = st->caret;
    st->have_last = 1;
    if (caret_moved) {""",
"""    if (caret_input || changed) st->follow_caret = 1;
    if (user_scrolled) st->follow_caret = 0;   // the wheel has the last word
    st->last_caret = st->caret;
    st->have_last = 1;
    if (st->follow_caret) {
        st->follow_caret = 0;""",
    'reveal follows input only')
wr('src/dai_ui.cpp', s)

s = rd('include/dai_ui.h')
s = sub1(s,
"""    int   last_caret;   /* what the caret was last frame - see the widget    */
    int   have_last;""",
"""    int   last_caret;   /* what the caret was last frame - see the widget    */
    int   have_last;
    int   follow_caret; /* one shot: bring the caret into view on this draw  */""",
    'follow_caret field')
wr('include/dai_ui.h', s)

# ============================================== the About tab in Settings
s = rd('src/dai_editor_ui.cpp')
s = sub1(s,
"""    if (dai_ui_option(ui, "Theme", &p->settings_theme, THEMES, 3)) {""",
"""    if (dai_ui_option(ui, "Theme", &p->settings_theme, THEMES, 3)) {""",
    'settings marker (unchanged)')

s = sub1(s,
"""    dai_ui_label(ui, "Values apply immediately.");""",
"""    dai_ui_label(p->ui, "Values apply immediately.");
    // ---- About ------------------------------------------------------------
    // Version, where things are, and whether there is a newer one. Everything
    // a bug report needs, in the one place people already open when something
    // is wrong - and none of it is worth a panel of its own.
    dai_ui_separator(ui);
    dai_ui_label(ui, "About");
    dai_ui_label_fmt(ui, "Version   %s", dai_version());
    dai_ui_label_fmt(ui, "Projects  %s", p->about_projects[0] ? p->about_projects : "(unset)");
    dai_ui_label_fmt(ui, "Assets    %s", p->about_assets[0] ? p->about_assets : "(no project open)");
    if (p->about_status[0]) dai_ui_label_fmt(ui, "Update    %s", p->about_status);
    if (dai_ui_button(ui, "Check for updates")) p->want_update_check = 1;
    if (dai_ui_button(ui, "Copy this to the clipboard")) {
        char all[900];
        std::snprintf(all, sizeof(all),
                      "DAIDALOS %s\\nprojects: %s\\nassets: %s\\nupdate: %s",
                      dai_version(), p->about_projects, p->about_assets,
                      p->about_status[0] ? p->about_status : "not checked");
        dai_editor_ui_clipboard_set(p, 0, all);
        dai_editor_ui_toast(p, "copied - paste it into a bug report", 2.0f);
    }""",
    'about block')

s = sub1(s,
"""    int    want_save = 0, want_refresh = 0;""",
"""    int    want_save = 0, want_refresh = 0;
    // What the About block shows. The editor knows none of it - the host owns
    // the disk and the updater - so it is pushed in and simply displayed.
    char about_projects[320] = { 0 };
    char about_assets[320] = { 0 };
    char about_status[160] = { 0 };
    int  want_update_check = 0;""",
    'about state')

s = sub1(s,
"""int dai_editor_ui_take_refresh(dai_editor_ui *p) {""",
"""void dai_editor_ui_about(dai_editor_ui *p, const char *projects_dir,
                         const char *assets_dir, const char *update_status) {
    if (!p) return;
    if (projects_dir) std::snprintf(p->about_projects, sizeof(p->about_projects), "%s", projects_dir);
    if (assets_dir)   std::snprintf(p->about_assets, sizeof(p->about_assets), "%s", assets_dir);
    if (update_status) std::snprintf(p->about_status, sizeof(p->about_status), "%s", update_status);
}

int dai_editor_ui_take_update_check(dai_editor_ui *p) {
    if (!p || !p->want_update_check) return 0;
    p->want_update_check = 0;
    return 1;
}

int dai_editor_ui_take_refresh(dai_editor_ui *p) {""",
    'about api')
wr('src/dai_editor_ui.cpp', s)

s = rd('include/dai_editor_ui.h')
s = sub1(s,
"""DAI_API int  dai_editor_ui_take_refresh(dai_editor_ui *p);""",
"""DAI_API int  dai_editor_ui_take_refresh(dai_editor_ui *p);

/* What the About block in Settings shows. Pushed by the host each frame (it is
 * three strings) because the editor knows neither where the projects live nor
 * whether an update exists. `update_status` is free text - "up to date",
 * "2026.08.05-9 available", "could not reach the server". */
DAI_API void dai_editor_ui_about(dai_editor_ui *p, const char *projects_dir,
                                 const char *assets_dir, const char *update_status);
/* 1 once, when the user pressed "Check for updates". */
DAI_API int  dai_editor_ui_take_update_check(dai_editor_ui *p);""",
    'about decl')
wr('include/dai_editor_ui.h', s)
print('patch84 ok')
