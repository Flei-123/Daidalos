#!/usr/bin/env python3
# patch54 - the editor side: captions that are captions, folders that show
# whether they hold anything, prefab instances in blue, and Delete that deletes.
import sys, os
ROOT = os.path.dirname(os.path.abspath(__file__))
def rd(p):
    with open(os.path.join(ROOT, p), 'r', encoding='utf-8') as f: return f.read()
def wr(p, s):
    full = os.path.join(ROOT, p); bak = full + '.bak_p54'
    if not os.path.exists(bak):
        with open(bak, 'w', encoding='utf-8') as f: f.write(rd(p))
    with open(full, 'w', encoding='utf-8') as f: f.write(s)
def sub1(t, old, new, what):
    if new in t: print('skip (already applied): ' + what); return t
    if old not in t: print('MISS: ' + what); sys.exit(1)
    if t.count(old) != 1: print('AMBIG %d: %s' % (t.count(old), what)); sys.exit(1)
    return t.replace(old, new)

s = rd('src/dai_editor_ui.cpp')

# ================================================ 1. the category is a caption
s = sub1(s,
"""            if (row_kind[i] < 0) items[i] = { DAI_ICON_LAYERS, flat[i].c_str(), nullptr };""",
"""            if (row_kind[i] < 0) items[i] = { nullptr, flat[i].c_str(), nullptr, 1 };""",
    'category is a header item')

s = sub1(s,
"""                items[i] = { ic, flat[i].c_str(), nullptr };""",
"""                items[i] = { ic, flat[i].c_str(), nullptr, 0 };""",
    'component item not a header')

# =========================================== 2. a folder that looks occupied
# Both places that draw a folder row ask the same question: does anything live
# under this path. The tree already walks the folder set; the listing has the
# file list in hand.
s = sub1(s,
"""// How many rows the tree WILL draw, before it draws them - the scroll offset""",
"""// Does this folder hold anything at all - a file or another folder? The two
// folder icons differ by exactly this, and a hollow folder that turns out to
// be full is worse than no icon difference at all.
static bool folder_has_content(dai_editor_ui *p, const std::set<std::string> &folders,
                               const std::string &dir) {
    for (const auto &f : folders)
        if (parent_of(f) == dir) return true;
    for (const char *a : p->assets)
        if (a && parent_of(a) == dir) return true;
    return false;
}

// How many rows the tree WILL draw, before it draws them - the scroll offset""",
    'folder_has_content')

s = sub1(s,
"""        std::string label = dir.empty() ? "Assets" : base_of(dir);
        float text_x = px + indent + 14.0f;
        if (dai_ui_has_icon(ui, DAI_ICON_FOLDER)) {
            dai_ui_icon_at(ui, DAI_ICON_FOLDER, text_x, ry + 3.5f, 13.0f,
                           selected ? st->text : st->text_dim);
            text_x += 19.0f;
        }""",
"""        std::string label = dir.empty() ? "Assets" : base_of(dir);
        float text_x = px + indent + 14.0f;
        const char *fic = folder_has_content(p, folders, dir) ? DAI_ICON_FOLDER_FULL
                                                             : DAI_ICON_FOLDER;
        if (dai_ui_has_icon(ui, fic)) {
            dai_ui_icon_at(ui, fic, text_x, ry + 3.5f, 13.0f,
                           selected ? st->text : st->text_dim);
            text_x += 19.0f;
        }""",
    'tree folder icon')

s = sub1(s,
"""                    if (browser_row(p, list_x + 2.0f, ry, list_w - 4.0f, ROW,
                                    DAI_ICON_FOLDER, name.c_str(),
                                    ffull == p->proj_sel_folder) && clicks_ok) {""",
"""                    const char *fic2 = folder_has_content(p, folders, ffull)
                                     ? DAI_ICON_FOLDER_FULL : DAI_ICON_FOLDER;
                    if (browser_row(p, list_x + 2.0f, ry, list_w - 4.0f, ROW,
                                    fic2, name.c_str(),
                                    ffull == p->proj_sel_folder) && clicks_ok) {""",
    'list folder icon')

# ============================================= 3. prefab instances are blue
s = sub1(s,
"""        int rc = dai_ui_tree_item_icon(p->ui, node_icon(r), label, depth, kids,
                                       kids ? &open : nullptr,
                                       dai_editor_is_selected(p->ed, n));""",
"""        // Unity's one visual rule for prefabs, and it is a good one: the NAME
        // is blue. Not a badge, not a second column - the thing you are
        // already reading tells you this object came from a file, so "why did
        // my change come back" is answered before it is asked.
        if (r.prefab[0]) dai_ui_tree_label_color(p->ui, rgba(0x6C, 0xB6, 0xF5, 255));
        int rc = dai_ui_tree_item_icon(p->ui, node_icon(r), label, depth, kids,
                                       kids ? &open : nullptr,
                                       dai_editor_is_selected(p->ed, n));""",
    'prefab label blue')

# ================================================ 4. Delete, in the browser
s = sub1(s,
"""    dai_editor_ui_rename_fn asset_import = nullptr;
    void       *import_user = nullptr;""",
"""    dai_editor_ui_rename_fn asset_import = nullptr;
    void       *import_user = nullptr;
    // Deleting a file is not undoable, so it asks first - and the asking is a
    // small popup rather than a modal, because a modal in an editor stops the
    // world for a question about one file.
    dai_editor_ui_rename_fn asset_delete = nullptr;
    void       *delete_user = nullptr;
    std::string delete_ask;            // what Delete is about to remove
    dai_ui_popup menu_delete{};""",
    'delete state')

s = sub1(s,
"""void dai_editor_ui_import_host(dai_editor_ui *p, dai_editor_ui_rename_fn fn, void *user) {
    if (!p) return;
    p->asset_import = fn; p->import_user = user;
}""",
"""void dai_editor_ui_import_host(dai_editor_ui *p, dai_editor_ui_rename_fn fn, void *user) {
    if (!p) return;
    p->asset_import = fn; p->import_user = user;
}

void dai_editor_ui_delete_host(dai_editor_ui *p, dai_editor_ui_rename_fn fn, void *user) {
    if (!p) return;
    p->asset_delete = fn; p->delete_user = user;
}

int dai_editor_ui_delete_project_pick(dai_editor_ui *p) {
    if (!p) return 0;
    // Only when the Project window is the one under the pointer: Delete in the
    // scene means the selected OBJECT, and the two must never be confused.
    if (!dai_ui_root_hovered(p->ui, "Project")) return 0;
    std::string target;
    if (!p->proj_sel_folder.empty()) target = p->proj_sel_folder;
    else if (p->asset_sel >= 0 && p->asset_sel < (int)p->assets.size() &&
             p->assets[(size_t)p->asset_sel])
        target = p->assets[(size_t)p->asset_sel];
    if (target.empty()) return 0;
    if (!p->asset_delete) {
        dai_editor_ui_toast(p, "this build cannot delete files", 2.0f);
        return 1;                       // still ours: do not delete the object
    }
    p->delete_ask = target;
    float mx = 0, my = 0;
    dai_ui_mouse(p->ui, &mx, &my, nullptr, nullptr);
    dai_ui_popup_open(&p->menu_delete, mx, my);
    return 1;
}""",
    'delete api impl')

# The confirm popup runs with the other context menus, above every panel.
s = sub1(s,
"""    // The material picker's list. A material is a name today (the renderer""",
"""    // "Delete X?" - two words and two buttons, over the row it is about.
    if (p->menu_delete.open && !p->delete_ask.empty()) {
        std::string q = "Delete " + base_of(p->delete_ask) + "?";
        dai_ui_menu_item items[2] = {
            { DAI_ICON_TRASH, q.c_str(), nullptr, 1 },
            { nullptr, "Delete permanently", nullptr, 0 },
        };
        int pick = dai_ui_popup_menu(p->ui, &p->menu_delete, items, 2);
        if (pick == 1) {
            if (p->asset_delete && p->asset_delete(p->delete_ask.c_str(), nullptr,
                                                   p->delete_user)) {
                char msg[192];
                std::snprintf(msg, sizeof(msg), "deleted %s",
                              base_of(p->delete_ask).c_str());
                dai_editor_ui_toast(p, msg, 2.0f);
                p->want_refresh = 1;
                if (p->proj_sel_folder == p->delete_ask) p->proj_sel_folder.clear();
                p->asset_sel = -1;
            } else {
                dai_editor_ui_toast(p, "could not delete it", 2.5f);
            }
            p->delete_ask.clear();
        } else if (!p->menu_delete.open) {
            p->delete_ask.clear();
        }
    }

    // The material picker's list. A material is a name today (the renderer""",
    'delete confirm popup')
wr('src/dai_editor_ui.cpp', s)

# ---------------------------------------------------------------- the header
s = rd('include/dai_editor_ui.h')
s = sub1(s,
"""DAI_API void dai_editor_ui_import_host(dai_editor_ui *p, dai_editor_ui_rename_fn fn, void *user);""",
"""DAI_API void dai_editor_ui_import_host(dai_editor_ui *p, dai_editor_ui_rename_fn fn, void *user);

/* Deleting an asset. `new_path` is NULL - the same callback shape as rename
 * and import, because the host's answer to all three is "it owns the disk".
 * Return 1 when the file (or folder, recursively) is gone. */
DAI_API void dai_editor_ui_delete_host(dai_editor_ui *p, dai_editor_ui_rename_fn fn, void *user);

/* Delete pressed while the Project window is under the pointer: asks about
 * whatever is selected there and returns 1 when it took the key, so the host
 * knows not to also delete the scene selection. */
DAI_API int  dai_editor_ui_delete_project_pick(dai_editor_ui *p);""",
    'delete host decl')
wr('include/dai_editor_ui.h', s)
print('patch54 ok')
