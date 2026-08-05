#!/usr/bin/env python3
# patch35 - F2 renames what the Project window has, folders included.
import sys, os
ROOT = os.path.dirname(os.path.abspath(__file__))
def rd(p):
    with open(os.path.join(ROOT, p), 'r', encoding='utf-8') as f: return f.read()
def wr(p, s):
    full = os.path.join(ROOT, p); bak = full + '.bak_p35'
    if not os.path.exists(bak):
        with open(bak, 'w', encoding='utf-8') as f: f.write(rd(p))
    with open(full, 'w', encoding='utf-8') as f: f.write(s)
def sub1(t, old, new, what):
    if old not in t: print('MISS: ' + what); sys.exit(1)
    if t.count(old) != 1: print('AMBIG %d: %s' % (t.count(old), what)); sys.exit(1)
    return t.replace(old, new)

s = rd('include/dai_editor_ui.h')
s = sub1(s, "DAI_API void       dai_editor_ui_panel_open(dai_editor_ui *p, const char *title);",
"""DAI_API void       dai_editor_ui_panel_open(dai_editor_ui *p, const char *title);

/* F2 in the Project window: rename whatever was last clicked there - a file
 * OR a folder. Returns 1 when it started one, so the host can fall through to
 * renaming the scene selection when the answer is no. */
DAI_API int        dai_editor_ui_rename_project_pick(dai_editor_ui *p);""",
'rename api')
wr('include/dai_editor_ui.h', s)

s = rd('src/dai_editor_ui.cpp')
s = sub1(s, "    std::string rename_click;",
            "    std::string rename_click;\n    std::string last_pick;         // last row clicked in the Project window",
         'last_pick field')

# a folder row click remembers the folder
s = sub1(s,
"""                    if (browser_row(p, list_x + 2.0f, ry, list_w - 4.0f, ROW,
                                    DAI_ICON_FOLDER, name.c_str(), 0) && clicks_ok) {
                        p->proj_dir = p->proj_dir.empty() ? name : p->proj_dir + "/" + name;""",
"""                    if (browser_row(p, list_x + 2.0f, ry, list_w - 4.0f, ROW,
                                    DAI_ICON_FOLDER, name.c_str(), 0) && clicks_ok) {
                        p->last_pick = ffull;      // F2 renames THIS folder
                        p->proj_dir = p->proj_dir.empty() ? name : p->proj_dir + "/" + name;""",
'folder click remembers')

# a file row click remembers the file
s = sub1(s, """                        p->asset_sel = fi;
                    }""",
            """                        p->asset_sel = fi;
                        p->last_pick = full;
                    }""",
         'file click remembers')

s = sub1(s, "void dai_editor_ui_panel_open(dai_editor_ui *p, const char *title) {",
"""int dai_editor_ui_rename_project_pick(dai_editor_ui *p) {
    if (!p) return 0;
    std::string pick = p->last_pick;
    if (pick.empty() && p->asset_sel >= 0 && p->asset_sel < (int)p->assets.size())
        pick = p->assets[(size_t)p->asset_sel] ? p->assets[(size_t)p->asset_sel] : "";
    if (pick.empty()) return 0;
    // The rename field is drawn in the list of the CURRENT folder, so open
    // the parent first or the row never exists and the rename cancels itself.
    p->rename_asset = pick;
    p->proj_dir = parent_of(pick);
    project_expand_to(p, p->proj_dir);
    std::string base = base_of(pick);
    size_t dot = base.find_last_of('.');
    if (dot != std::string::npos && dot > 0) base.resize(dot);
    std::snprintf(p->rename_asset_buf, sizeof(p->rename_asset_buf), "%s", base.c_str());
    p->rename_seen_active = 0;
    p->proj_tab = 0;
    return 1;
}

void dai_editor_ui_panel_open(dai_editor_ui *p, const char *title) {""",
'rename impl')
wr('src/dai_editor_ui.cpp', s)

s = rd('examples/editor_demo.cpp')
s = sub1(s,
"""        if (dai_window_key_down(win, DAI_KEY_F2) && !prev_f2 && !dai_ui_text_active(ui) &&
            dai_editor_selection_count(ed) > 0)
            dai_editor_ui_rename(panels, dai_editor_selected(ed, 0));""",
"""        if (dai_window_key_down(win, DAI_KEY_F2) && !prev_f2 && !dai_ui_text_active(ui)) {
            // The Project window first: if something was clicked there, F2
            // belongs to it - including a folder, which had no way to be
            // renamed except through the context menu.
            if (!dai_editor_ui_rename_project_pick(panels) &&
                dai_editor_selection_count(ed) > 0)
                dai_editor_ui_rename(panels, dai_editor_selected(ed, 0));
        }""",
'f2 host')
wr('examples/editor_demo.cpp', s)
print('patch35 ok')
