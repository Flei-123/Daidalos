#!/usr/bin/env python3
# patch70 - a prefab lands in the folder you are in, a right click re-opens the
# menu where you pressed, and it selects what it landed on.
import sys, os
ROOT = os.path.dirname(os.path.abspath(__file__))
def rd(p):
    with open(os.path.join(ROOT, p), 'r', encoding='utf-8') as f: return f.read()
def wr(p, s):
    full = os.path.join(ROOT, p); bak = full + '.bak_p70'
    if not os.path.exists(bak):
        with open(bak, 'w', encoding='utf-8') as f: f.write(rd(p))
    with open(full, 'w', encoding='utf-8') as f: f.write(s)
def sub1(t, old, new, what):
    if new in t: print('skip (already applied): ' + what); return t
    if old not in t: print('MISS: ' + what); sys.exit(1)
    if t.count(old) != 1: print('AMBIG %d: %s' % (t.count(old), what)); sys.exit(1)
    return t.replace(old, new)

s = rd('src/dai_editor_ui.cpp')

# ============================ 1. "Assets" is a folder like any other
# The fallback was: no folder under the pointer AND the browser is at the root
# -> write it to "prefabs". That made sense when the root was not somewhere you
# could BE; it is, and dropping an object into Assets and finding the prefab in
# a folder you never opened is the editor deciding where your files live.
s = sub1(s,
"""                // The folder the pointer was actually over wins; the folder
                // the browser happens to be showing is only the fallback.
                // Dropping an object onto "Enemies" and finding the prefab in
                // "Assets" is a filing system that files things somewhere else.
                std::string dir = p->proj_drop_ok ? p->proj_drop_dir
                                : (p->proj_dir.empty() ? std::string("prefabs")
                                                       : p->proj_dir);""",
"""                // The folder the pointer was actually over wins; otherwise
                // the folder the browser is showing - and "" IS a folder, it
                // is Assets. There is no third place to guess at: an object
                // dropped in Assets belongs in Assets.
                std::string dir = p->proj_drop_ok ? p->proj_drop_dir : p->proj_dir;""",
    'prefab goes where you are')

s = sub1(s,
"""                char rel[224];
                std::snprintf(rel, sizeof(rel), "%s/%s.daidalos", dir.c_str(),
                              pr3.name[0] ? pr3.name : "Prefab");""",
"""                char rel[224];
                if (dir.empty())
                    std::snprintf(rel, sizeof(rel), "%s.daidalos",
                                  pr3.name[0] ? pr3.name : "Prefab");
                else
                    std::snprintf(rel, sizeof(rel), "%s/%s.daidalos", dir.c_str(),
                                  pr3.name[0] ? pr3.name : "Prefab");""",
    'no leading slash at the root')

# ==================== 2. a right click moves the menu to where it was pressed
s = sub1(s,
"""    // Right click anywhere in the panel: the Create menu, the way Unity's
    // project window does it.
    if (right_pressed && dai_ui_root_hovered(ui, "Project") &&
        !p->menu_project.open && !p->menu_node.open && !p->menu_canvas.open)
        dai_ui_popup_open(&p->menu_project, mx, my);""",
"""    // Right click anywhere in the panel: the Create menu, the way Unity's
    // project window does it - and a SECOND right click somewhere else moves
    // it there. Refusing while one was already open meant the first press was
    // swallowed and you had to press twice, which reads as the menu being
    // stuck to the spot it first appeared.
    if (right_pressed && dai_ui_root_hovered(ui, "Project") &&
        !p->menu_node.open && !p->menu_canvas.open) {
        p->menu_project.open = 0;          // forget where it was
        p->menu_project.placed = 0;        // ...including the frozen position
        dai_ui_popup_open(&p->menu_project, mx, my);
    }""",
    'right click moves the menu')

# ============================ 3. it selects what it was pressed on
s = sub1(s,
"""                    // Right click aims Rename at THIS folder.
                    if (over_f && right_pressed && clicks_ok) p->rename_click = ffull;""",
"""                    // A right click SELECTS the folder it landed on, the way
                    // a left click does. Acting on the old selection while the
                    // pointer sits on another row is how the wrong folder gets
                    // deleted - and it is the one mistake with no undo.
                    if (over_f && right_pressed && clicks_ok) {
                        p->rename_click = ffull;
                        p->proj_sel_folder = ffull;
                        p->last_pick = ffull;
                        p->asset_sel = -1;
                    }""",
    'right click selects the folder')

s = sub1(s,
"""                    // A right click selects what it landed on, then the menu opens.
                    if (over && right_pressed && clicks_ok) { p->asset_sel = fi; p->rename_click = full; }""",
"""                    // A right click selects what it landed on, then the menu opens.
                    if (over && right_pressed && clicks_ok) {
                        p->asset_sel = fi;
                        p->rename_click = full;
                        p->last_pick = full;
                        p->proj_sel_folder.clear();   // a file and a folder are not both it
                    }""",
    'right click selects the file')

# The tree's right click selected by NAVIGATING, which moves the listing out
# from under the pointer before the menu has even opened.
s = sub1(s,
"""        if (over && right_pressed && clicks_ok) {
            // Right click aims Rename at THIS folder - the tree is where
            // people point at folders, and only the list rows armed it.
            p->proj_dir = dir;
            p->rename_click = dir;
        }""",
"""        if (over && right_pressed && clicks_ok) {
            // Selects, does not navigate: opening the folder moves the listing
            // out from under the pointer before the menu has even appeared,
            // and then the menu is about a folder you are now inside.
            p->rename_click = dir;
            p->proj_sel_folder = dir;
            p->last_pick = dir;
            p->asset_sel = -1;
        }""",
    'tree right click selects')
wr('src/dai_editor_ui.cpp', s)
print('patch70 ok')
