# -*- coding: utf-8 -*-
import sys, io

def rw(p): return io.open(p, encoding='utf-8').read()
def wr(p, s): io.open(p, 'w', encoding='utf-8').write(s)
def sub1(s, old, new, tag):
    if old not in s: print("MISS", tag); sys.exit(1)
    if s.count(old) != 1: print("AMBIG", tag, s.count(old)); sys.exit(1)
    print("ok", tag); return s.replace(old, new)

P = 'src/dai_editor_ui.cpp'
s = rw(P)

# --------------------------------------------------------- #33 + #34
# Materials as Unity's array, and the rough/emissive fields leave the Mesh
# Renderer - with materials there, a surface property floating next to it is
# two places that can disagree.
s = sub1(s,
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
        }""",
"""        // ---- Materials, Unity's array -------------------------------------
        // The roughness and emissive fields that used to sit here moved IN:
        // a surface property next to the material that owns it is two places
        // the same fact can disagree.
        //
        // The layout is Unity's, element by element: the header line carries
        // the size, the rows carry a handle and the field, and the +/- lives
        // under the list on the right.
        {
            std::vector<std::string> mats = script_list(r.materials);
            if (mats.empty()) mats.push_back("Default");
            // Header: "Materials" left, size field right.
            dai_ui_row(p->ui, 20.0f);
            float hdr_w = dai_ui_panel_width(p->ui) - dai_ui_style_of(p->ui)->padding * 2 - 46.0f;
            dai_ui_spacing(p->ui, hdr_w - dai_ui_text_width(p->ui, "Materials"));
            dai_ui_label(p->ui, "Materials");
            float mcount = (float)mats.size();
            if (dai_ui_num_field(p->ui, "", &mcount, 1.0f, 1.0f, 7.0f, "matsize")) {
                size_t want = (size_t)(mcount + 0.5f);
                while (mats.size() < want) mats.push_back("Default");
                while (mats.size() > want) mats.pop_back();
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
            // + / - on the right, under the list.
            dai_ui_row(p->ui, 20.0f);
            float btn_w = 26.0f;
            float pad = dai_ui_panel_width(p->ui) - dai_ui_style_of(p->ui)->padding * 2
                      - btn_w * 2 - dai_ui_style_of(p->ui)->spacing;
            if (pad > 0) dai_ui_spacing(p->ui, pad);
            if (dai_ui_button(p->ui, "+") && mats.size() < 7) {
                mats.push_back("Default");
                script_join(r.materials, sizeof(r.materials), mats);
            }
            if (dai_ui_button(p->ui, "-") && mats.size() > 1) {
                mats.pop_back();
                script_join(r.materials, sizeof(r.materials), mats);
            }
            dai_ui_row_end(p->ui);
        }""", "materials unity array")

wr(P, s)
print("patch20 done")
