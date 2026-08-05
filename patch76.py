#!/usr/bin/env python3
# patch76 - the host end of materials: create one, and push what the file says
# onto everything that points at it.
import sys, os
ROOT = os.path.dirname(os.path.abspath(__file__))
def rd(p):
    with open(os.path.join(ROOT, p), 'r', encoding='utf-8') as f: return f.read()
def wr(p, s):
    full = os.path.join(ROOT, p); bak = full + '.bak_p76'
    if not os.path.exists(bak):
        with open(bak, 'w', encoding='utf-8') as f: f.write(rd(p))
    with open(full, 'w', encoding='utf-8') as f: f.write(s)
def sub1(t, old, new, what):
    if new in t: print('skip (already applied): ' + what); return t
    if old not in t: print('MISS: ' + what); sys.exit(1)
    if t.count(old) != 1: print('AMBIG %d: %s' % (t.count(old), what)); sys.exit(1)
    return t.replace(old, new)

# =========================================== 1. Create: Material in the menu
s = rd('include/dai_editor_ui.h')
s = sub1(s,
"""DAI_API void dai_editor_ui_folder_host(dai_editor_ui *p,
                                       int (*create)(const char *name, void *user),
                                       void *user);""",
"""DAI_API void dai_editor_ui_folder_host(dai_editor_ui *p,
                                       int (*create)(const char *name, void *user),
                                       void *user);
/* "Create: Material" - the host writes a default .daimat at that
 * asset-relative path. Same split as every other create: the editor knows
 * which folder is open, the host owns the disk and the format. */
DAI_API void dai_editor_ui_material_host(dai_editor_ui *p,
                                         int (*create)(const char *name, void *user),
                                         void *user);""",
    'material host decl')
wr('include/dai_editor_ui.h', s)

s = rd('src/dai_editor_ui.cpp')
s = sub1(s,
"""    int (*folder_create)(const char *name, void *user) = nullptr;""",
"""    int (*folder_create)(const char *name, void *user) = nullptr;
    int (*material_create)(const char *name, void *user) = nullptr;
    void *material_user = nullptr;""",
    'material create field')

s = sub1(s,
"""    p->folder_create = create;""",
"""    p->folder_create = create;""",
    'folder host marker (unchanged)')

s = sub1(s,
"""void dai_editor_ui_prefab_host(dai_editor_ui *p, dai_editor_ui_rename_fn fn) {""",
"""void dai_editor_ui_material_host(dai_editor_ui *p,
                                 int (*create)(const char *name, void *user),
                                 void *user) {
    if (!p) return;
    p->material_create = create;
    p->material_user = user;
}

void dai_editor_ui_prefab_host(dai_editor_ui *p, dai_editor_ui_rename_fn fn) {""",
    'material host impl')

s = sub1(s,
"""        { DAI_ICON_TRASH, "Delete", "Del" },
    };""",
"""        { DAI_ICON_TRASH, "Delete", "Del" },
        { DAI_ICON_MATERIAL, "Create: Material", nullptr },
    };""",
    'material menu entry')

s = sub1(s,
"""    static const int WITH_ROW[7]    = { 0, 1, 2, 3, 4, 5, 6 };
    static const int WITHOUT_ROW[5] = { 0, 1, 3, 4, 5 };
    dai_ui_menu_item shown_items[7];
    const int *map = on_row ? WITH_ROW : WITHOUT_ROW;
    uint32_t shown_n = on_row ? 7u : 5u;""",
"""    static const int WITH_ROW[8]    = { 0, 1, 7, 2, 3, 4, 5, 6 };
    static const int WITHOUT_ROW[6] = { 0, 1, 7, 3, 4, 5 };
    dai_ui_menu_item shown_items[8];
    const int *map = on_row ? WITH_ROW : WITHOUT_ROW;
    uint32_t shown_n = on_row ? 8u : 6u;""",
    'material in the menu map')

s = sub1(s,
"""    else if (ppick == 3) p->want_save = 1;
    else if (ppick == 4) p->want_refresh = 1;""",
"""    else if (ppick == 7 && p->material_create) {
        // NewMaterial, NewMaterial 2, ... in the folder that is open - the
        // same walk "Create: Folder" does, and for the same reason.
        std::string base7 = p->proj_dir.empty() ? std::string() : p->proj_dir + "/";
        for (int i = 0; i < 30; ++i) {
            char rel7[224];
            if (i == 0) std::snprintf(rel7, sizeof(rel7), "%sNewMaterial.daimat", base7.c_str());
            else        std::snprintf(rel7, sizeof(rel7), "%sNewMaterial %d.daimat", base7.c_str(), i);
            if (p->material_create(rel7, p->material_user)) {
                p->proj_tab = 0;
                p->want_refresh = 1;
                p->rename_asset = rel7;          // straight into the rename
                std::string b7 = base_of(rel7);
                if (b7.size() > 7) b7.resize(b7.size() - 7);   // drop ".daimat"
                std::snprintf(p->rename_asset_buf, sizeof(p->rename_asset_buf), "%s", b7.c_str());
                p->rename_seen_active = 0;
                break;
            }
        }
    }
    else if (ppick == 3) p->want_save = 1;
    else if (ppick == 4) p->want_refresh = 1;""",
    'material create action')
wr('src/dai_editor_ui.cpp', s)

# ================================ 2. the host: write one, and apply them all
s = rd('examples/editor_demo.cpp')
s = sub1(s,
"""#include "dai_project.h\"""",
"""#include "dai_material.h"
#include "dai_project.h\"""",
    'include material')

s = sub1(s,
"""static int script_create(const char *name, void *) {""",
"""// A new material file, with the defaults in it - not an empty file. Something
// you can immediately drag onto an object and then edit.
static int material_create(const char *rel, void *) {
    if (!rel || !*rel || !g_assets_dir[0]) return 0;
    if (std::strstr(rel, "..")) return 0;
    char path[700];
    std::snprintf(path, sizeof(path), "%s/%s", g_assets_dir, rel);
    if (path_exists(path)) return 0;              // the caller walks the number up
    make_parent_dirs(path);
    dai_material m = dai_material_default();
    return dai_material_save(&m, path) == DAI_OK ? 1 : 0;
}

// Every node that points at a .daimat gets that file's numbers. Called when a
// material is assigned and whenever the browser refreshes, which is also what
// happens after the built-in editor saves one - so editing the file and
// pressing Ctrl+S recolours every object using it.
//
// The cache is by PATH: a scene with two hundred crates on one material reads
// the file once. It is dropped wholesale on each pass rather than watched,
// because the pass only runs when something already told us the disk moved.
static void apply_materials(dai_doc *doc) {
    if (!doc || !g_assets_dir[0]) return;
    std::vector<dai_node> ids(dai_doc_count(doc));
    if (ids.empty()) return;
    dai_doc_nodes(doc, ids.data(), (uint32_t)ids.size());

    std::vector<std::pair<std::string, dai_material>> cache;
    auto material_of = [&](const std::string &rel, dai_material *out) {
        for (const auto &kv : cache)
            if (kv.first == rel) { *out = kv.second; return true; }
        char full[700];
        std::snprintf(full, sizeof(full), "%s/%s", g_assets_dir, rel.c_str());
        dai_material m = dai_material_default();
        char merr[256] = { 0 };
        if (dai_material_load(&m, full, merr, sizeof(merr)) != DAI_OK) {
            if (g_panels_for_log && merr[0]) dai_editor_ui_log(g_panels_for_log, 1, merr);
            return false;
        }
        cache.push_back({ rel, m });
        *out = m;
        return true;
    };

    int changed = 0;
    for (dai_node n : ids) {
        dai_node_desc r{};
        if (dai_doc_get(doc, n, &r) != DAI_OK || !r.materials[0]) continue;
        // Slot 0 is the surface of the whole object; the array is there for
        // meshes with several, which the renderer does not split yet.
        std::string first = r.materials;
        size_t semi = first.find(';');
        if (semi != std::string::npos) first = first.substr(0, semi);
        if (!dai_material_is_file(first.c_str())) continue;
        dai_material m{};
        if (!material_of(first, &m)) continue;
        dai_node_desc before = r;
        r.color = m.color;
        r.roughness = m.roughness;
        r.emissive = m.emissive;
        if (std::memcmp(&before, &r, sizeof(r)) != 0) {
            dai_doc_set(doc, n, &r);
            ++changed;
        }
    }
    if (changed && g_panels_for_log) {
        char msg[96];
        std::snprintf(msg, sizeof(msg), "materials applied to %d object(s)", changed);
        dai_editor_ui_log(g_panels_for_log, 0, msg);
    }
}

static int script_create(const char *name, void *) {""",
    'material_create and apply')

s = sub1(s,
"""    dai_editor_ui_folder_host(panels, folder_create, nullptr);""",
"""    dai_editor_ui_folder_host(panels, folder_create, nullptr);
    dai_editor_ui_material_host(panels, material_create, nullptr);""",
    'wire material host')

s = sub1(s,
"""        if (dai_editor_ui_take_refresh(panels) && assets) { dai_assets_poll(assets); fed_rev = 0xFFFFFFFFu; }""",
"""        if (dai_editor_ui_take_refresh(panels) && assets) {
            dai_assets_poll(assets);
            fed_rev = 0xFFFFFFFFu;
            apply_materials(doc);       // a refresh is also "the files moved"
            dai_doc_sync_apply(sync);
        }
        if (dai_editor_ui_take_material_apply(panels)) {
            apply_materials(doc);
            dai_doc_sync_apply(sync);
        }""",
    'apply on refresh and assign')
wr('examples/editor_demo.cpp', s)
print('patch76 ok')
