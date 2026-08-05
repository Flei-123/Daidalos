#!/usr/bin/env python3
# patch65 - the hover that never cleared, a prefab that lands where you drop
# it, Delete in the browser's own menu, and a scene that says when it is dirty.
import sys, os
ROOT = os.path.dirname(os.path.abspath(__file__))
def rd(p):
    with open(os.path.join(ROOT, p), 'r', encoding='utf-8') as f: return f.read()
def wr(p, s):
    full = os.path.join(ROOT, p); bak = full + '.bak_p65'
    if not os.path.exists(bak):
        with open(bak, 'w', encoding='utf-8') as f: f.write(rd(p))
    with open(full, 'w', encoding='utf-8') as f: f.write(s)
def sub1(t, old, new, what):
    if new in t: print('skip (already applied): ' + what); return t
    if old not in t: print('MISS: ' + what); sys.exit(1)
    if t.count(old) != 1: print('AMBIG %d: %s' % (t.count(old), what)); sys.exit(1)
    return t.replace(old, new)

s = rd('src/dai_editor_ui.cpp')

# ==================================== 1. the row under the pointer, THIS frame
# hover_node was set whenever a row reported hover and never cleared. Off the
# rows entirely - the empty space under the last object - it therefore still
# named whatever was hovered last, which is why dropping a child "into
# nothing" made it a child of the bottom row instead of freeing it. One line,
# in the same place param_hover_entry already had one.
s = sub1(s,
"""    p->visible_rows = 0;
    p->param_hover_entry = -1;          // same, for reference assign fields""",
"""    p->visible_rows = 0;
    p->param_hover_entry = -1;          // same, for reference assign fields
    // Which row the pointer is over is a fact about THIS frame. Left standing
    // from the last one, the empty space below the tree quietly still means
    // "the last row you touched" - and every drop into nothing re-parented
    // onto the bottom object.
    p->hover_node = DAI_INVALID_NODE;""",
    'hover_node reset')

# ============================ 2. a prefab is created where it was dropped
s = sub1(s,
"""                std::string dir = p->proj_dir.empty() ? std::string("prefabs")
                                                      : p->proj_dir;""",
"""                // The folder the pointer was actually over wins; the folder
                // the browser happens to be showing is only the fallback.
                // Dropping an object onto "Enemies" and finding the prefab in
                // "Assets" is a filing system that files things somewhere else.
                std::string dir = p->proj_drop_ok ? p->proj_drop_dir
                                : (p->proj_dir.empty() ? std::string("prefabs")
                                                       : p->proj_dir);""",
    'prefab into the dropped folder')

# ===================================== 3. Delete, in the menu it belongs to
s = sub1(s,
"""    static const dai_ui_menu_item PROJ_ITEMS[] = {
        { DAI_ICON_SCRIPT, "Create: JS Script", nullptr },
        { DAI_ICON_FOLDER, "Create: Folder", nullptr },
        { DAI_ICON_PLUS, "Rename", "F2" },
        { DAI_ICON_SAVE, "Save scene", "Ctrl+S" },
        { DAI_ICON_SEARCH, "Refresh", nullptr },
        { DAI_ICON_SCRIPT, "Create: C++ Behaviour", nullptr },
    };
    int ppick = dai_ui_popup_menu(p->ui, &p->menu_project, PROJ_ITEMS, 6);""",
"""    static const dai_ui_menu_item PROJ_ITEMS[] = {
        { DAI_ICON_SCRIPT, "Create: JS Script", nullptr },
        { DAI_ICON_FOLDER, "Create: Folder", nullptr },
        { DAI_ICON_PLUS, "Rename", "F2" },
        { DAI_ICON_SAVE, "Save scene", "Ctrl+S" },
        { DAI_ICON_SEARCH, "Refresh", nullptr },
        { DAI_ICON_SCRIPT, "Create: C++ Behaviour", nullptr },
        { DAI_ICON_TRASH, "Delete", "Del" },
    };
    int ppick = dai_ui_popup_menu(p->ui, &p->menu_project, PROJ_ITEMS, 7);
    if (ppick == 6) {
        // Whatever the right click landed on, or failing that the selection.
        std::string target = !p->rename_click.empty() ? p->rename_click
                           : !p->proj_sel_folder.empty() ? p->proj_sel_folder
                           : std::string(asset_at(p, p->asset_sel));
        p->rename_click.clear();
        if (target.empty()) {
            dai_editor_ui_toast(p, "nothing selected to delete", 2.0f);
        } else if (!p->asset_delete) {
            dai_editor_ui_toast(p, "this build cannot delete files", 2.0f);
        } else {
            p->delete_ask = target;
            float dmx2 = 0, dmy2 = 0;
            dai_ui_mouse(p->ui, &dmx2, &dmy2, nullptr, nullptr);
            dai_ui_popup_open(&p->menu_delete, dmx2, dmy2);
        }
    }""",
    'delete in project menu')

# The Delete KEY had the same blind spot as the menu: it only looked at the
# folder selection and the file selection, and a right click had set neither.
s = sub1(s,
"""    std::string target;
    if (!p->proj_sel_folder.empty()) target = p->proj_sel_folder;
    else if (p->asset_sel >= 0 && p->asset_sel < (int)p->assets.size() &&
             p->assets[(size_t)p->asset_sel])
        target = p->assets[(size_t)p->asset_sel];
    if (target.empty()) return 0;""",
"""    std::string target;
    if (!p->proj_sel_folder.empty()) target = p->proj_sel_folder;
    else if (*asset_at(p, p->asset_sel)) target = asset_at(p, p->asset_sel);
    else if (!p->last_pick.empty())      target = p->last_pick;   // the tree, or a right click
    if (target.empty()) {
        dai_editor_ui_toast(p, "select a file or folder first", 1.8f);
        return 1;      // still ours: Delete over the browser is never the scene's
    }""",
    'delete key falls back to last pick')

# ======================================= 4. the scene says when it is dirty
s = sub1(s,
"""    int have_root = p->scene_label[0] != 0;
    int root_open = !p->scene_root_folded;
    if (have_root) {
        char label[160];
        std::snprintf(label, sizeof(label), "%s", p->scene_label);""",
"""    int have_root = p->scene_label[0] != 0;
    int root_open = !p->scene_root_folded;
    if (have_root) {
        // The asterisk every editor uses, in the one row that names the file.
        // Without it "did I save that" has no answer except pressing Ctrl+S
        // again and hoping.
        char label[172];
        std::snprintf(label, sizeof(label), "%s%s", p->scene_label,
                      p->scene_dirty ? " *" : "");""",
    'scene star')

s = sub1(s,
"""    char scene_label[128] = { 0 };    // the active scene, shown as the hierarchy root""",
"""    char scene_label[128] = { 0 };    // the active scene, shown as the hierarchy root
    int  scene_dirty = 0;             // the host owns the file; it says when""",
    'scene_dirty field')
wr('src/dai_editor_ui.cpp', s)

s = rd('include/dai_editor_ui.h')
s = sub1(s,
"""DAI_API void dai_editor_ui_scene_label(dai_editor_ui *p, const char *name);""",
"""DAI_API void dai_editor_ui_scene_label(dai_editor_ui *p, const char *name);
/* Whether the open scene differs from the file on disk. The host compares the
 * document's revision against the one it last wrote - only it knows when a
 * save happened - and the hierarchy puts an asterisk on the scene row. */
DAI_API void dai_editor_ui_scene_dirty(dai_editor_ui *p, int dirty);
DAI_API int  dai_editor_ui_scene_dirty_get(const dai_editor_ui *p);""",
    'scene dirty decl')
wr('include/dai_editor_ui.h', s)

s = rd('src/dai_editor_ui.cpp')
s = sub1(s,
"""void dai_editor_ui_scene_label(dai_editor_ui *p, const char *name) {""",
"""void dai_editor_ui_scene_dirty(dai_editor_ui *p, int dirty) {
    if (p) p->scene_dirty = dirty ? 1 : 0;
}
int dai_editor_ui_scene_dirty_get(const dai_editor_ui *p) {
    return p ? p->scene_dirty : 0;
}

void dai_editor_ui_scene_label(dai_editor_ui *p, const char *name) {""",
    'scene dirty impl')
wr('src/dai_editor_ui.cpp', s)
print('patch65 ok')
