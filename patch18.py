# -*- coding: utf-8 -*-
import sys, io

def rw(p): return io.open(p, encoding='utf-8').read()
def wr(p, s): io.open(p, 'w', encoding='utf-8').write(s)
def sub1(s, old, new, tag):
    if old not in s: print("MISS", tag); sys.exit(1)
    if s.count(old) != 1: print("AMBIG", tag, s.count(old)); sys.exit(1)
    print("ok", tag); return s.replace(old, new)

# 1. the field
P = 'include/dai_doc.h'
s = rw(P)
if 'materials[' not in s:
    s = sub1(s,
"""    /* graphics */
    uint32_t mesh;              /* 0xFFFFFFFF -> derive from shape              */""",
"""    /* graphics */
    uint32_t mesh;              /* 0xFFFFFFFF -> derive from shape              */
    /* The material STACK, Unity's "Materials" array on a MeshRenderer: one
     * row per slot, first slot is the object's base colour + texture. Stored
     * ';'-separated like the scripts. */
    char     materials[256];""", "materials field")
wr(P, s)

# 2. text (de)serialisation, same rule as everything else: only written when set
P = 'src/dai_doc_text.cpp'
s = rw(P)
s = sub1(s,
"""        if (r.asset[0])                     put(s, "  asset %s\\n", r.asset);""",
"""        if (r.asset[0])                     put(s, "  asset %s\\n", r.asset);
        if (r.materials[0])                 put(s, "  materials %s\\n", r.materials);""", "write materials")
s = sub1(s,
"""        else if (key == "prefab") { std::string v = rest_of_line(after);
                                    snprintf(rec.prefab, sizeof(rec.prefab), "%s", v.c_str()); }""",
"""        else if (key == "prefab") { std::string v = rest_of_line(after);
                                    snprintf(rec.prefab, sizeof(rec.prefab), "%s", v.c_str()); }
        else if (key == "materials") { std::string v = rest_of_line(after);
                                    snprintf(rec.materials, sizeof(rec.materials), "%s", v.c_str()); }""",
"read materials")
wr(P, s)

# 3. the inspector: a Unity-shaped array. Size first, then Element rows.
P = 'src/dai_editor_ui.cpp'
s = rw(P)
s = sub1(s,
"""        dai_ui_num_field(p->ui, "Rough", &r.roughness, 0.005f, 0.02f, 1.0f, "rough");
        dai_ui_num_field(p->ui, "Emissive", &r.emissive, 0.01f, 0.0f, 100.0f, "emissive");""",
"""        dai_ui_num_field(p->ui, "Rough", &r.roughness, 0.005f, 0.02f, 1.0f, "rough");
        dai_ui_num_field(p->ui, "Emissive", &r.emissive, 0.01f, 0.0f, 100.0f, "emissive");

        // ---- Materials, Unity's array -------------------------------------
        // Element 0 is what the object is actually drawn with today; extra
        // slots are ready for the renderer's per-mesh split. The array has a
        // size, the size has +/- , and every row is an object field - that is
        // the shape, and a custom one would be worse for no reason.
        {
            std::vector<std::string> mats = script_list(r.materials);
            if (mats.empty()) mats.push_back("Default");
            dai_ui_label_fmt(p->ui, "Materials (%d)", (int)mats.size());
            dai_ui_row(p->ui, 20.0f);
            if (dai_ui_button(p->ui, "+") && mats.size() < 7) {
                mats.push_back("Default");
                script_join(r.materials, sizeof(r.materials), mats);
            }
            if (dai_ui_button(p->ui, "-") && mats.size() > 1) {
                mats.pop_back();
                script_join(r.materials, sizeof(r.materials), mats);
            }
            dai_ui_row_end(p->ui);
            for (size_t mi = 0; mi < mats.size(); ++mi) {
                char mlbl[32];
                std::snprintf(mlbl, sizeof(mlbl), "Element %d", (int)mi);
                if (dai_ui_object_field(p->ui, mlbl, mats[mi].c_str(), DAI_ICON_MATERIAL)) {
                    float mx2 = 0, my2 = 0;
                    dai_ui_mouse(p->ui, &mx2, &my2, nullptr, nullptr);
                    dai_ui_searchlist_open(&p->mat_list, mx2 - 150.0f, my2);
                    p->mat_list.wants_focus = 1;
                    p->mat_menu_node = n;
                    p->mat_menu_slot = (int)mi;
                }
            }
        }""", "materials array")

s = sub1(s,
"""    dai_node    addcomp_node = DAI_INVALID_NODE;   // who Add Component was opened for""",
"""    dai_node    addcomp_node = DAI_INVALID_NODE;   // who Add Component was opened for
    dai_ui_searchlist mat_list{};     // the material row's picker
    dai_node    mat_menu_node = DAI_INVALID_NODE;
    int         mat_menu_slot = 0;""", "mat list state")

# the material picker, next to the mesh picker
s = sub1(s,
"""    // The Add Component list. Entries flip between add and remove so one menu""",
"""    // The material picker's list. A material is a name today (the renderer
    // has no material files yet), so the list offers the ones the palette
    // already knows; typing in the search field filters them.
    if (p->mat_list.open && p->mat_menu_node != DAI_INVALID_NODE) {
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
    }

    // The Add Component list. Entries flip between add and remove so one menu""",
"mat picker")

wr(P, s)
print("patch18 done")
