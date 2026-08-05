#!/usr/bin/env python3
# patch42 - the Project window as a file explorer: OS drops import, and a row
# dragged onto a folder moves there.
import sys, os
ROOT = os.path.dirname(os.path.abspath(__file__))
def rd(p):
    with open(os.path.join(ROOT, p), 'r', encoding='utf-8') as f: return f.read()
def wr(p, s):
    full = os.path.join(ROOT, p); bak = full + '.bak_p42'
    if not os.path.exists(bak):
        with open(bak, 'w', encoding='utf-8') as f: f.write(rd(p))
    with open(full, 'w', encoding='utf-8') as f: f.write(s)
def sub1(t, old, new, what):
    if new in t: print('skip (already applied): ' + what); return t
    if old not in t: print('MISS: ' + what); sys.exit(1)
    if t.count(old) != 1: print('AMBIG %d: %s' % (t.count(old), what)); sys.exit(1)
    return t.replace(old, new)

# ==================================================================== header
s = rd('include/dai_editor_ui.h')
s = sub1(s,
"""/* The host opens a file in the machine's editor (VS Code when it is there).
 * The editor hands over the asset-relative path on a double click. */
DAI_API void dai_editor_ui_open_asset_host(dai_editor_ui *p, dai_editor_ui_rename_fn fn, void *user);""",
"""/* The host opens a file in the machine's editor (VS Code when it is there).
 * The editor hands over the asset-relative path on a double click. */
DAI_API void dai_editor_ui_open_asset_host(dai_editor_ui *p, dai_editor_ui_rename_fn fn, void *user);

/* Importing: copy something from anywhere on disk INTO the project's assets.
 * Same signature as the rename host and the same division of labour - the
 * editor knows which folder is open and what was dropped on it, the host owns
 * the disk. `src` is an absolute OS path (a file or a whole folder), `dest`
 * is asset-relative ("models/crate.glb"). Returns 1 when it landed. */
DAI_API void dai_editor_ui_import_host(dai_editor_ui *p, dai_editor_ui_rename_fn fn, void *user);

/* What the window system says was dropped on the editor: absolute paths, one
 * per line (dai_window_dropped_files hands over exactly this), plus where the
 * pointer was in UI coordinates. The Project window takes the drop when the
 * pointer is over it and imports into the folder it is showing; everything
 * else ignores it. Returns 1 when something was imported.
 *
 * The host does not have to work out which panel was hit - it cannot, the
 * layout is the editor's - it just forwards every drop. */
DAI_API int  dai_editor_ui_drop_files(dai_editor_ui *p, const char *paths_nl,
                                      float x, float y);""",
    'header import api')
wr('include/dai_editor_ui.h', s)

# ==================================================================== state
s = rd('src/dai_editor_ui.cpp')
s = sub1(s,
"""    dai_editor_ui_rename_fn asset_rename = nullptr;
    void       *rename_user = nullptr;""",
"""    dai_editor_ui_rename_fn asset_rename = nullptr;
    void       *rename_user = nullptr;
    // Importing from outside the project: the desktop's file manager drops
    // onto the Project window, the host does the copying.
    dai_editor_ui_rename_fn asset_import = nullptr;
    void       *import_user = nullptr;
    // A row of the Project window dragged onto a FOLDER moves it there. The
    // row that is aimed at is worked out while the folders are drawn and
    // consumed at the end of the frame, where the release is handled - the
    // two cannot be the same place, because the folder rows are drawn long
    // before anyone knows whether the button came up over one of them.
    std::string proj_drop_dir;
    int         proj_drop_ok = 0;""",
    'import state')

# ----------------------------------------------------------------- the move
s = sub1(s,
"""// One folder row of the tree, then its visible children. `ry` walks down the
// column; the clip rect cuts whatever the panel cannot show.""",
"""// Drag a row onto a folder and it moves there. On disk this is a rename with
// a different parent, which is exactly what the host's rename callback does -
// so an explorer's most basic gesture needs no new plumbing, only the two
// guards that make it safe:
//
//   - a folder cannot move into itself or into its own descendant. Left to
//     std::rename that is EINVAL on Linux and a lost subtree on some others;
//     either way it is never what was meant.
//   - dropping something into the folder it already lives in does nothing,
//     silently. It is the commonest miss-drop there is.
static void project_move(dai_editor_ui *p, const std::string &src, const std::string &dir) {
    if (!p || !p->asset_rename || src.empty()) return;
    if (parent_of(src) == dir) return;
    if (dir == src) return;
    if (dir.size() > src.size() && dir.compare(0, src.size(), src) == 0 &&
        dir[src.size()] == '/') return;
    std::string b = base_of(src);
    if (b.empty()) return;
    std::string dst = dir.empty() ? b : dir + "/" + b;
    if (dst == src) return;
    if (p->asset_rename(src.c_str(), dst.c_str(), p->rename_user)) {
        p->want_refresh = 1;
        char msg[192];
        std::snprintf(msg, sizeof(msg), "moved %s to %s", b.c_str(),
                      dir.empty() ? "Assets" : dir.c_str());
        dai_editor_ui_toast(p, msg, 1.6f);
    } else {
        dai_editor_ui_toast(p, "could not move it - is something with that name already there?", 2.5f);
    }
}

// One folder row of the tree, then its visible children. `ry` walks down the
// column; the clip rect cuts whatever the panel cannot show.""",
    'project_move')

# ------------------------------------------------- the tree is a drop target
s = sub1(s,
"""        dai_ui_text(ui, text_x, ry + 3.0f, label.c_str(), st->text);
        if (over && right_pressed && clicks_ok) {""",
"""        dai_ui_text(ui, text_x, ry + 3.0f, label.c_str(), st->text);
        // A row being dragged aims at this folder: light it up, and remember
        // it for the release. The tree is the only way to reach a folder that
        // is not in the current listing - "up one level", in other words.
        if (!p->drag_script.empty() && over && p->drag_script != dir) {
            p->proj_drop_dir = dir;
            p->proj_drop_ok = 1;
            dai_ui_rect_outline(ui, px, ry, tree_w, ROW, 1.0f, st->accent);
        }
        if (over && right_pressed && clicks_ok) {""",
    'tree drop target')

# ------------------------------------------ the folder rows of the listing
s = sub1(s,
"""                    // Right click aims Rename at THIS folder.
                    if (over_f && right_pressed && clicks_ok) p->rename_click = ffull;""",
"""                    // ...and it is where a dragged row lands.
                    if (!p->drag_script.empty() && over_f && p->drag_script != ffull) {
                        p->proj_drop_dir = ffull;
                        p->proj_drop_ok = 1;
                        dai_ui_rect_outline(ui, list_x + 2.0f, ry, list_w - 4.0f, ROW,
                                            1.0f, st->accent);
                    }
                    // Right click aims Rename at THIS folder.
                    if (over_f && right_pressed && clicks_ok) p->rename_click = ffull;""",
    'list folder drop target')

# ------------------------------------------------- every row arms the drag
s = sub1(s,
"""                    // A .js arms a drag: press, move 6 px, and it travels
                    // with the cursor until it lands on a node (or nowhere).
                    size_t fl2 = full.size();
                    int lpress = 0;
                    dai_ui_mouse(ui, nullptr, nullptr, nullptr, &lpress);
                    if (is_behaviour_file(full) && over &&
                        lpress && clicks_ok) {
                        p->drag_pending = full;
                        p->drag_px = mx; p->drag_py = my;
                    }""",
"""                    // EVERY row arms a drag now, not just a .js: press, move
                    // 6 px, and it travels with the cursor. Where it lands
                    // decides what it meant - a folder moves it there, a
                    // hierarchy node still attaches a script. A browser in
                    // which only one file type can be picked up is not a file
                    // browser, it is a script list with icons.
                    int lpress = 0;
                    dai_ui_mouse(ui, nullptr, nullptr, nullptr, &lpress);
                    if (over && lpress && clicks_ok) {
                        p->drag_pending = full;
                        p->drag_px = mx; p->drag_py = my;
                    }""",
    'arm drag on any file')

# Folders can be dragged too - that is half of what moving things around a
# project actually is.
s = sub1(s,
"""                    if (browser_row(p, list_x + 2.0f, ry, list_w - 4.0f, ROW,
                                    DAI_ICON_FOLDER, name.c_str(), 0) && clicks_ok) {""",
"""                    {
                        int fpress = 0;
                        dai_ui_mouse(ui, nullptr, nullptr, nullptr, &fpress);
                        if (over_f && fpress && clicks_ok) {
                            p->drag_pending = ffull;      // a folder drags too
                            p->drag_px = mx; p->drag_py = my;
                        }
                    }
                    if (browser_row(p, list_x + 2.0f, ry, list_w - 4.0f, ROW,
                                    DAI_ICON_FOLDER, name.c_str(), 0) && clicks_ok) {""",
    'arm drag on folders')

# --------------------------------------------------------- the drop itself
s = sub1(s,
"""        if (!ddown && !p->drag_pending.empty()) {
            if (!p->drag_script.empty()) {
                dai_node target = DAI_INVALID_NODE;
                if (p->hover_node != DAI_INVALID_NODE) target = p->hover_node;
                else if (dai_ui_root_hovered(ui, "Inspector") &&
                         dai_editor_selection_count(p->ed) > 0)
                    target = dai_editor_selected(p->ed, 0);
                if (target != DAI_INVALID_NODE) script_attach(p, target, p->drag_script);
            }
            p->drag_pending.clear();
            p->drag_script.clear();
        }""",
"""        if (!ddown && !p->drag_pending.empty()) {
            if (!p->drag_script.empty()) {
                // A folder under the pointer wins: that gesture is a MOVE and
                // it is the only meaning it can have. Only then does the old
                // "drop a script on a node" reading get a look in - and only
                // for a file that actually is a script, or dropping a .png on
                // a crate would add a behaviour called crate.png.
                if (p->proj_drop_ok) {
                    project_move(p, p->drag_script, p->proj_drop_dir);
                } else if (is_behaviour_file(p->drag_script)) {
                    dai_node target = DAI_INVALID_NODE;
                    if (p->hover_node != DAI_INVALID_NODE) target = p->hover_node;
                    else if (dai_ui_root_hovered(ui, "Inspector") &&
                             dai_editor_selection_count(p->ed) > 0)
                        target = dai_editor_selected(p->ed, 0);
                    if (target != DAI_INVALID_NODE) script_attach(p, target, p->drag_script);
                }
            }
            p->drag_pending.clear();
            p->drag_script.clear();
        }
        // Set while the folders were drawn, spent here, gone by the next
        // frame - so a stale target cannot survive the Project window being
        // closed, or hidden behind another tab.
        p->proj_drop_ok = 0;
        p->proj_drop_dir.clear();""",
    'drop handling')

# ------------------------------------------------------------- the two APIs
s = sub1(s,
"""void dai_editor_ui_prefab_host(dai_editor_ui *p, dai_editor_ui_rename_fn fn) {
    if (p) p->prefab_save = fn;
}""",
"""void dai_editor_ui_import_host(dai_editor_ui *p, dai_editor_ui_rename_fn fn, void *user) {
    if (!p) return;
    p->asset_import = fn; p->import_user = user;
}

int dai_editor_ui_drop_files(dai_editor_ui *p, const char *paths_nl, float x, float y) {
    if (!p || !paths_nl || !*paths_nl) return 0;
    // Only the Project window takes files. Asking the dock where it is - and
    // not remembering it from the draw - is the rule everywhere else in here:
    // a drop arrives between frames, and last frame's rectangle is the only
    // one that exists.
    float px = 0, py = 0, pw = 0, ph = 0;
    if (!dai_dock_panel_rect(p->dock, "Project", 0, &px, &py, &pw, &ph)) return 0;
    if (x < px || x >= px + pw || y < py || y >= py + ph) return 0;
    if (!p->asset_import) {
        dai_editor_ui_toast(p, "no import host - this build cannot copy files in", 3.0f);
        return 0;
    }
    // The Projects half of the window has no folder to import INTO; a drop
    // there means the files, so switch to them first.
    p->proj_tab = 0;
    int n = 0, seen = 0;
    std::string one;
    for (const char *c = paths_nl; ; ++c) {
        if (*c && *c != '\\n') { one += *c; continue; }
        while (!one.empty() && (one.back() == '\\r' || one.back() == ' ')) one.pop_back();
        if (!one.empty()) {
            ++seen;
            std::string b = one;
            size_t sl = b.find_last_of("/\\\\");
            if (sl != std::string::npos) b = b.substr(sl + 1);
            if (!b.empty()) {
                std::string dest = p->proj_dir.empty() ? b : p->proj_dir + "/" + b;
                if (p->asset_import(one.c_str(), dest.c_str(), p->import_user)) ++n;
            }
            one.clear();
        }
        if (!*c) break;
    }
    char msg[128];
    if (n > 0) {
        p->want_refresh = 1;
        std::snprintf(msg, sizeof(msg), n == 1 ? "imported %d item into %s"
                                               : "imported %d items into %s",
                      n, p->proj_dir.empty() ? "Assets" : p->proj_dir.c_str());
        dai_editor_ui_toast(p, msg, 2.0f);
    } else if (seen > 0) {
        dai_editor_ui_toast(p, "nothing imported - could not copy it in", 2.5f);
    }
    return n > 0;
}

void dai_editor_ui_prefab_host(dai_editor_ui *p, dai_editor_ui_rename_fn fn) {
    if (p) p->prefab_save = fn;
}""",
    'import api impl')
wr('src/dai_editor_ui.cpp', s)
print('patch42 ok')
