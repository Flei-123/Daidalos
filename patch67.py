#!/usr/bin/env python3
# patch67 - the context menu only offers what the click landed on, and a new
# folder is created, not entered.
import sys, os
ROOT = os.path.dirname(os.path.abspath(__file__))
def rd(p):
    with open(os.path.join(ROOT, p), 'r', encoding='utf-8') as f: return f.read()
def wr(p, s):
    full = os.path.join(ROOT, p); bak = full + '.bak_p67'
    if not os.path.exists(bak):
        with open(bak, 'w', encoding='utf-8') as f: f.write(rd(p))
    with open(full, 'w', encoding='utf-8') as f: f.write(s)
def sub1(t, old, new, what):
    if new in t: print('skip (already applied): ' + what); return t
    if old not in t: print('MISS: ' + what); sys.exit(1)
    if t.count(old) != 1: print('AMBIG %d: %s' % (t.count(old), what)); sys.exit(1)
    return t.replace(old, new)

s = rd('src/dai_editor_ui.cpp')

# ============================ 1. a right click in empty space hit nothing
# rename_click is set by whichever ROW the right button came down on. Nothing
# ever cleared it, so a right click on the empty part of the browser still
# carried the last row's path - and the menu offered to rename and delete a
# file the pointer was nowhere near. Cleared at the top of the frame's browser
# pass, before any row has had the chance to claim it.
s = sub1(s,
"""    // The divider between the columns drags, like every other split.""",
"""    // Whatever the last right click hit is a fact about THAT click. Cleared
    // before the rows redraw, so a press on empty space leaves it empty and
    // the menu below can tell the difference between "this file" and "here".
    if (right_pressed) p->rename_click.clear();

    // The divider between the columns drags, like every other split.""",
    'clear rename_click on right press')

# ============================== 2. the menu is built from what was hit
s = sub1(s,
"""    static const dai_ui_menu_item PROJ_ITEMS[] = {
        { DAI_ICON_SCRIPT, "Create: JS Script", nullptr },
        { DAI_ICON_FOLDER, "Create: Folder", nullptr },
        { DAI_ICON_PLUS, "Rename", "F2" },
        { DAI_ICON_SAVE, "Save scene", "Ctrl+S" },
        { DAI_ICON_SEARCH, "Refresh", nullptr },
        { DAI_ICON_SCRIPT, "Create: C++ Behaviour", nullptr },
        { DAI_ICON_TRASH, "Delete", "Del" },
    };
    int ppick = dai_ui_popup_menu(p->ui, &p->menu_project, PROJ_ITEMS, 7);""",
"""    // Rename and Delete only exist when the click landed ON something. A menu
    // that offers to delete when you right clicked the background is a menu
    // that will eventually delete the wrong thing.
    const bool on_row = !p->rename_click.empty();
    static const dai_ui_menu_item PROJ_ITEMS[] = {
        { DAI_ICON_SCRIPT, "Create: JS Script", nullptr },
        { DAI_ICON_FOLDER, "Create: Folder", nullptr },
        { DAI_ICON_PLUS, "Rename", "F2" },
        { DAI_ICON_SAVE, "Save scene", "Ctrl+S" },
        { DAI_ICON_SEARCH, "Refresh", nullptr },
        { DAI_ICON_SCRIPT, "Create: C++ Behaviour", nullptr },
        { DAI_ICON_TRASH, "Delete", "Del" },
    };
    // The two row-only entries sit at 2 and 6; without a row the menu is the
    // other five, and the indices below are mapped back so nothing else moves.
    static const int WITH_ROW[7]    = { 0, 1, 2, 3, 4, 5, 6 };
    static const int WITHOUT_ROW[5] = { 0, 1, 3, 4, 5 };
    dai_ui_menu_item shown_items[7];
    const int *map = on_row ? WITH_ROW : WITHOUT_ROW;
    uint32_t shown_n = on_row ? 7u : 5u;
    for (uint32_t i = 0; i < shown_n; ++i) shown_items[i] = PROJ_ITEMS[map[i]];
    int raw = dai_ui_popup_menu(p->ui, &p->menu_project, shown_items, shown_n);
    int ppick = (raw >= 0 && raw < (int)shown_n) ? map[raw] : raw;""",
    'menu built from what was hit')

# ================================= 3. a new folder is made, not walked into
s = sub1(s,
"""            if (p->folder_create(name, p->script_user)) {
                p->proj_dir = name;
                project_expand_to(p, p->proj_dir);
                p->proj_list_scroll = 0.0f;
                p->want_refresh = 1;
                break;
            }""",
"""            if (p->folder_create(name, p->script_user)) {
                // Selected, not entered. Walking into a folder you have just
                // made hides the folder you were working in, and the next
                // thing anybody does is click Back - Unity selects it and
                // starts the rename right there instead.
                p->proj_sel_folder = name;
                p->last_pick = name;
                p->asset_sel = -1;
                p->want_refresh = 1;
                break;
            }""",
    'new folder is selected not entered')
wr('src/dai_editor_ui.cpp', s)
print('patch67 ok')
