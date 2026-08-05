#!/usr/bin/env python3
# patch46 - the Project window: a folder is selected by one click and entered
# by two, and the list stops fighting the wheel.
import sys, os
ROOT = os.path.dirname(os.path.abspath(__file__))
def rd(p):
    with open(os.path.join(ROOT, p), 'r', encoding='utf-8') as f: return f.read()
def wr(p, s):
    full = os.path.join(ROOT, p); bak = full + '.bak_p46'
    if not os.path.exists(bak):
        with open(bak, 'w', encoding='utf-8') as f: f.write(rd(p))
    with open(full, 'w', encoding='utf-8') as f: f.write(s)
def sub1(t, old, new, what):
    if new in t: print('skip (already applied): ' + what); return t
    if old not in t: print('MISS: ' + what); sys.exit(1)
    if t.count(old) != 1: print('AMBIG %d: %s' % (t.count(old), what)); sys.exit(1)
    return t.replace(old, new)

s = rd('src/dai_editor_ui.cpp')

# ===================================================== 1. the folder selection
s = sub1(s,
"""    std::string proj_drop_dir;
    int         proj_drop_ok = 0;""",
"""    std::string proj_drop_dir;
    int         proj_drop_ok = 0;
    // Which folder ROW of the listing is selected. Unity's rule, and every
    // file manager's: one click picks a folder up (rename it, drag it, see
    // what it is), two clicks go inside. Entering on the first click makes
    // the folder unselectable - there is no gesture left that means "this
    // one" - which is why renaming a folder needed the tree or a right click.
    std::string proj_sel_folder;""",
    'proj_sel_folder field')

# ==================================================== 2. the scroll, clamped
# BEFORE the rows are drawn, not after. The wheel moved the offset, the rows
# were drawn at the new offset, and only then was the offset clamped back -
# so a list that fits scrolled one frame and snapped back the next, for ever,
# which is the "it shakes" report. Nothing here is a rounding problem: the
# frame that is on screen is drawn from a value the code has already decided
# is wrong.
s = sub1(s,
"""// One folder row of the tree, then its visible children. `ry` walks down the
// column; the clip rect cuts whatever the panel cannot show.""",
"""// How many rows the tree WILL draw, before it draws them - the scroll offset
// has to be clamped against the real height, and the real height cannot be a
// number left over from last frame. Mirrors project_tree_rows exactly: this
// row, plus the children of an open folder.
static int project_tree_count(dai_editor_ui *p, const std::set<std::string> &folders,
                              const std::string &dir) {
    int n = 1;
    if (p->proj_folds.count(dir))
        for (const auto &f : folders)
            if (parent_of(f) == dir) n += project_tree_count(p, folders, f);
    return n;
}

// One folder row of the tree, then its visible children. `ry` walks down the
// column; the clip rect cuts whatever the panel cannot show.""",
    'tree row counter')

s = sub1(s,
"""        dai_ui_clip_begin(ui, px, cols_y, tree_w, cols_h);
        if (mx >= px && mx < px + tree_w && my >= cols_y && my < cols_y + cols_h)
            p->proj_tree_scroll -= wheel * 32.0f;
        if (p->proj_tree_scroll < 0.0f) p->proj_tree_scroll = 0.0f;
        float ry = cols_y + 2.0f - p->proj_tree_scroll;
        int rows = 0;
        project_tree_rows(p, folders, std::string(), 0, px, tree_w, cols_y, cols_h,
                          ry, rows);
        float max_off = (float)rows * 20.0f + 4.0f - cols_h;
        if (max_off < 0.0f) max_off = 0.0f;
        if (p->proj_tree_scroll > max_off) p->proj_tree_scroll = max_off;
        dai_ui_clip_end(ui);""",
"""        dai_ui_clip_begin(ui, px, cols_y, tree_w, cols_h);
        float tmax = (float)project_tree_count(p, folders, std::string()) * 20.0f
                   + 4.0f - cols_h;
        if (tmax < 0.0f) tmax = 0.0f;
        if (mx >= px && mx < px + tree_w && my >= cols_y && my < cols_y + cols_h)
            p->proj_tree_scroll -= wheel * 32.0f;
        if (p->proj_tree_scroll > tmax) p->proj_tree_scroll = tmax;
        if (p->proj_tree_scroll < 0.0f) p->proj_tree_scroll = 0.0f;
        float ry = cols_y + 2.0f - p->proj_tree_scroll;
        int rows = 0;
        project_tree_rows(p, folders, std::string(), 0, px, tree_w, cols_y, cols_h,
                          ry, rows);
        dai_ui_clip_end(ui);""",
    'tree scroll clamp')

s = sub1(s,
"""        dai_ui_clip_begin(ui, list_x, cols_y, list_w, list_h);
        if (mx >= list_x && mx < list_x + list_w && my >= cols_y && my < cols_y + list_h)
            p->proj_list_scroll -= wheel * 32.0f;
        if (p->proj_list_scroll < 0.0f) p->proj_list_scroll = 0.0f;
        float ry = cols_y + 2.0f - p->proj_list_scroll;
        int rows = 0;""",
"""        dai_ui_clip_begin(ui, list_x, cols_y, list_w, list_h);
        // The listing knows its own length before it draws a thing: the
        // folders and the files it already collected, or the one line that
        // says there are none.
        int want_rows = (int)subfolders.size() + (int)files.size();
        if (want_rows == 0) want_rows = 1;
        float lmax = (float)want_rows * ROW + 4.0f - list_h;
        if (lmax < 0.0f) lmax = 0.0f;
        if (mx >= list_x && mx < list_x + list_w && my >= cols_y && my < cols_y + list_h)
            p->proj_list_scroll -= wheel * 32.0f;
        if (p->proj_list_scroll > lmax) p->proj_list_scroll = lmax;
        if (p->proj_list_scroll < 0.0f) p->proj_list_scroll = 0.0f;
        float ry = cols_y + 2.0f - p->proj_list_scroll;
        int rows = 0;""",
    'list scroll clamp')

s = sub1(s,
"""            ry += ROW;
        }
        float max_off = (float)rows * ROW + 4.0f - list_h;
        if (max_off < 0.0f) max_off = 0.0f;
        if (p->proj_list_scroll > max_off) p->proj_list_scroll = max_off;
        dai_ui_clip_end(ui);
    }""",
"""            ry += ROW;
        }
        (void)rows;   // the clamp happened before the draw, where it belongs
        dai_ui_clip_end(ui);
    }""",
    'drop post-draw list clamp')

# ============================================== 3. one click picks, two enter
s = sub1(s,
"""                    if (browser_row(p, list_x + 2.0f, ry, list_w - 4.0f, ROW,
                                    DAI_ICON_FOLDER, name.c_str(), 0) && clicks_ok) {
                        p->last_pick = ffull;      // F2 renames THIS folder
                        p->proj_dir = p->proj_dir.empty() ? name : p->proj_dir + "/" + name;
                        project_expand_to(p, p->proj_dir);
                        p->proj_list_scroll = 0.0f;
                    }""",
"""                    if (browser_row(p, list_x + 2.0f, ry, list_w - 4.0f, ROW,
                                    DAI_ICON_FOLDER, name.c_str(),
                                    ffull == p->proj_sel_folder) && clicks_ok) {
                        // One click SELECTS it. Two go in. Anything else and
                        // a folder can never be the thing you are pointing at
                        // - which is the state F2, drag and the strip at the
                        // bottom all need it to be able to reach.
                        p->last_pick = ffull;      // F2 renames THIS folder
                        p->proj_sel_folder = ffull;
                        p->asset_sel = -1;         // a folder and a file cannot both be it
                        if (dai_ui_double_click(ui)) {
                            p->proj_dir = p->proj_dir.empty() ? name : p->proj_dir + "/" + name;
                            project_expand_to(p, p->proj_dir);
                            p->proj_list_scroll = 0.0f;
                            p->proj_sel_folder.clear();
                        }
                    }""",
    'folder single/double click')

# Picking a FILE drops the folder selection, and so does walking into another
# folder - two highlighted rows that mean different things is worse than none.
s = sub1(s,
"""                        p->asset_sel = fi;
                        p->last_pick = full;
                    }""",
"""                        p->asset_sel = fi;
                        p->last_pick = full;
                        p->proj_sel_folder.clear();
                    }""",
    'file click clears folder selection')

# The bottom strip names whatever is picked - a folder now counts.
s = sub1(s,
"""    int sel_valid = p->asset_sel >= 0 && p->asset_sel < (int)p->assets.size();
    float strip_h = (sel_valid || p->script_focus) ? 28.0f : 0.0f;""",
"""    int sel_valid = p->asset_sel >= 0 && p->asset_sel < (int)p->assets.size();
    int folder_sel = !p->proj_sel_folder.empty();
    float strip_h = (sel_valid || folder_sel || p->script_focus) ? 28.0f : 0.0f;""",
    'strip height with folder')

s = sub1(s,
"""        } else if (sel_valid) {
            const char *pick = p->assets[(size_t)p->asset_sel];""",
"""        } else if (folder_sel && !sel_valid) {
            std::string b = base_of(p->proj_sel_folder);
            dai_ui_text(ui, list_x + 4.0f, sy + 7.0f, b.c_str(), st->text);
            float bw2 = dai_ui_text_width(ui, "Open") + 20.0f;
            if (browser_button(p, list_x + list_w - bw2 - 6.0f, sy + 3.0f, bw2, 22.0f, "Open")) {
                p->proj_dir = p->proj_sel_folder;
                project_expand_to(p, p->proj_dir);
                p->proj_list_scroll = 0.0f;
                p->proj_sel_folder.clear();
            }
        } else if (sel_valid) {
            const char *pick = p->assets[(size_t)p->asset_sel];""",
    'strip shows folder')
wr('src/dai_editor_ui.cpp', s)
print('patch46 ok')
