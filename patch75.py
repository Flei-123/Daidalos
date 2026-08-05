#!/usr/bin/env python3
# patch75 - materials are files now: build them, create them, assign them,
# and apply them to everything that points at one.
import sys, os
ROOT = os.path.dirname(os.path.abspath(__file__))
def rd(p):
    with open(os.path.join(ROOT, p), 'r', encoding='utf-8') as f: return f.read()
def wr(p, s):
    full = os.path.join(ROOT, p); bak = full + '.bak_p75'
    if not os.path.exists(bak):
        with open(bak, 'w', encoding='utf-8') as f: f.write(rd(p))
    with open(full, 'w', encoding='utf-8') as f: f.write(s)
def sub1(t, old, new, what):
    if new in t: print('skip (already applied): ' + what); return t
    if old not in t: print('MISS: ' + what); sys.exit(1)
    if t.count(old) != 1: print('AMBIG %d: %s' % (t.count(old), what)); sys.exit(1)
    return t.replace(old, new)

# ==================================================================== build
s = rd('build.sh')
s = sub1(s,
"""g++ $FLAGS $ARCH -Iinclude -Isrc -c src/dai_project.cpp -o build/dai_project.o""",
"""g++ $FLAGS $ARCH -Iinclude -Isrc -c src/dai_project.cpp -o build/dai_project.o
g++ $FLAGS $ARCH -Iinclude -Isrc -c src/dai_material.cpp -o build/dai_material.o""",
    'build material')
s = sub1(s,
"""ar rcs build/libdaidalos.a build/dai_engine.o build/physics_null.o build/physics_jolt.o ${TALOS_OBJ} \\""",
"""ar rcs build/libdaidalos.a build/dai_engine.o build/physics_null.o build/physics_jolt.o ${TALOS_OBJ} \\
       build/dai_material.o \\""",
    'archive material')
wr('build.sh', s)

s = rd('build_win.sh')
s = sub1(s,
"""      dai_audio dai_native dai_tr physics_null\"""",
"""      dai_audio dai_native dai_tr dai_material physics_null\"""",
    'win core material')
wr('build_win.sh', s)

# ============================================== the browser knows the type
s = rd('src/dai_editor_ui.cpp')
s = sub1(s,
"""    if (e == "daidalos" || e == "prefab")             return DAI_ICON_C_PREFAB;""",
"""    if (e == "daidalos" || e == "prefab")             return DAI_ICON_C_PREFAB;
    if (e == "daimat")                                return DAI_ICON_MATERIAL;""",
    'material icon')

# ================================= the picker lists the project's materials
s = sub1(s,
"""    if (p->mat_list.open && p->mat_menu_node != DAI_INVALID_NODE) {
        static const char *MATS[] = { "Default", "Plastic", "Metal", "Glass", "Rubber", "Emissive" };
        dai_ui_menu_item items[6];
        for (int i = 0; i < 6; ++i) items[i] = { DAI_ICON_MATERIAL, MATS[i], nullptr };
        int pick = dai_ui_searchlist_draw(p->ui, &p->mat_list, items, 6);
        if (pick >= 0 && pick < 6) {
            dai_doc *md = dai_editor_doc(p->ed);
            dai_node_desc mr{};
            if (dai_doc_get(md, p->mat_menu_node, &mr) == DAI_OK) {
                std::vector<std::string> mats = script_list(mr.materials);
                while ((int)mats.size() <= p->mat_menu_slot) mats.push_back("Default");
                mats[(size_t)p->mat_menu_slot] = MATS[pick];
                dai_doc_begin(md, "Material");
                script_join(mr.materials, sizeof(mr.materials), mats);
                dai_doc_set(md, p->mat_menu_node, &mr);
                dai_doc_commit(md);
                dai_editor_resync(p->ed);
            }
            p->mat_menu_node = DAI_INVALID_NODE;
        }
    }""",
"""    if (p->mat_list.open && p->mat_menu_node != DAI_INVALID_NODE) {
        // Every .daimat in the project, and "Default" for none. The six
        // hard-coded names that used to be here were not materials - nothing
        // read them, and two objects called "Metal" shared a word, not a
        // surface.
        std::vector<std::string> paths;
        paths.push_back("Default");
        for (const char *a : p->assets)
            if (a && is_material_file(a)) paths.push_back(a);
        std::vector<std::string> labels;
        labels.reserve(paths.size());
        for (const std::string &pp : paths)
            labels.push_back(pp == "Default" ? pp : base_of(pp));
        std::vector<dai_ui_menu_item> items(paths.size());
        for (size_t i = 0; i < paths.size(); ++i)
            items[i] = { DAI_ICON_MATERIAL, labels[i].c_str(), nullptr, 0 };
        int pick = dai_ui_searchlist_draw(p->ui, &p->mat_list, items.data(),
                                          (uint32_t)items.size());
        if (pick >= 0 && pick < (int)paths.size()) {
            dai_doc *md = dai_editor_doc(p->ed);
            dai_node_desc mr{};
            if (dai_doc_get(md, p->mat_menu_node, &mr) == DAI_OK) {
                std::vector<std::string> mats = script_list(mr.materials);
                while ((int)mats.size() <= p->mat_menu_slot) mats.push_back("Default");
                mats[(size_t)p->mat_menu_slot] = paths[(size_t)pick];
                dai_doc_begin(md, "Material");
                script_join(mr.materials, sizeof(mr.materials), mats);
                dai_doc_set(md, p->mat_menu_node, &mr);
                dai_doc_commit(md);
                dai_editor_resync(p->ed);
                p->want_material_apply = 1;
            }
            p->mat_menu_node = DAI_INVALID_NODE;
        }
    }""",
    'material picker lists files')

s = sub1(s,
"""static bool is_scene_file(const std::string &path) {""",
"""// A .daimat. Asked here rather than through dai_material_is_file so this file
// keeps compiling without the material header - the editor UI has no business
// knowing what is IN a material, only which files are ones.
static bool is_material_file(const std::string &path) {
    const std::string ext = ".daimat";
    if (path.size() <= ext.size()) return false;
    std::string tail = path.substr(path.size() - ext.size());
    for (char &c : tail) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    return tail == ext;
}

static bool is_scene_file(const std::string &path) {""",
    'is_material_file')

# ============================ dragging a .daimat onto an object assigns it
s = sub1(s,
"""                } else if (is_behaviour_file(p->drag_script)) {""",
"""                } else if (is_material_file(p->drag_script) &&
                           (p->hover_node != DAI_INVALID_NODE ||
                            dai_ui_root_hovered(ui, "Inspector"))) {
                    // Dropped on an object: that object wears it. This is the
                    // gesture people try first, and it is the only one that
                    // does not require finding the slot in the inspector.
                    dai_node target = p->hover_node != DAI_INVALID_NODE
                                    ? p->hover_node
                                    : (dai_editor_selection_count(p->ed) > 0
                                       ? dai_editor_selected(p->ed, 0) : DAI_INVALID_NODE);
                    dai_doc *md2 = dai_editor_doc(p->ed);
                    dai_node_desc mr2{};
                    if (target != DAI_INVALID_NODE &&
                        dai_doc_get(md2, target, &mr2) == DAI_OK) {
                        std::vector<std::string> mats = script_list(mr2.materials);
                        if (mats.empty()) mats.push_back("Default");
                        mats[0] = p->drag_script;
                        dai_doc_begin(md2, "Material");
                        script_join(mr2.materials, sizeof(mr2.materials), mats);
                        dai_doc_set(md2, target, &mr2);
                        dai_doc_commit(md2);
                        dai_editor_resync(p->ed);
                        p->want_material_apply = 1;
                        char mm[160];
                        std::snprintf(mm, sizeof(mm), "%s applied",
                                      base_of(p->drag_script).c_str());
                        dai_editor_ui_toast(p, mm, 1.5f);
                    }
                } else if (is_behaviour_file(p->drag_script)) {""",
    'drop material on object')

# (the drag pill's material line is applied directly in the source)

# ================================== the host is told to re-read the files
s = sub1(s,
"""    int    want_save = 0, want_refresh = 0;""",
"""    int    want_save = 0, want_refresh = 0;
    // Set when a material was assigned or the browser was refreshed: the host
    // re-reads the .daimat files and pushes their numbers onto the nodes that
    // point at them. Read and cleared like the other one-shots.
    int want_material_apply = 0;""",
    'want_material_apply')

s = sub1(s,
"""int dai_editor_ui_take_refresh(dai_editor_ui *p) {""",
"""int dai_editor_ui_take_material_apply(dai_editor_ui *p) {
    if (!p || !p->want_material_apply) return 0;
    p->want_material_apply = 0;
    return 1;
}

int dai_editor_ui_take_refresh(dai_editor_ui *p) {""",
    'take_material_apply')
wr('src/dai_editor_ui.cpp', s)

s = rd('include/dai_editor_ui.h')
s = sub1(s,
"""DAI_API int  dai_editor_ui_take_refresh(dai_editor_ui *p);""",
"""DAI_API int  dai_editor_ui_take_refresh(dai_editor_ui *p);
/* 1 once when a material was assigned (or the assets were refreshed): the host
 * should re-read every .daimat a node points at and copy its numbers onto that
 * node. The editor does not open files; the host does. */
DAI_API int  dai_editor_ui_take_material_apply(dai_editor_ui *p);""",
    'take_material_apply decl')
wr('include/dai_editor_ui.h', s)
print('patch75 ok')
