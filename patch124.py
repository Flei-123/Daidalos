import io

p = 'src/dai_editor_ui.cpp'
s = io.open(p, encoding='utf-8').read()

# ===========================================================================
# Das Transform-Menue: nicht "die Komponente", sondern WELCHER TEIL.
#
# Bisher war jedes Komponentenmenue dasselbe: Reset / Copy / Paste / Remove.
# Fuer Transform ist das zu grob - man kopiert die Position eines Objekts auf
# ein anderes und will dessen Drehung NICHT mitnehmen. Unity hat dafuer zwei
# Eintraege, Blender vier; vier ist die ehrlichere Zahl, weil "alles" auch
# eine Wahl ist und nicht der Normalfall.
# ===========================================================================
old = """        int target = p->comp_menu_target;
        static const dai_ui_menu_item COMP_MENU_X[] = {
            { DAI_ICON_RESET, "Reset", nullptr },
            { DAI_ICON_COPY, "Copy Component", nullptr },
            { DAI_ICON_SAVE, "Paste Component Values", nullptr },
            { DAI_ICON_CLOSE, "Remove Component", nullptr },
        };
        int cpick = dai_ui_popup_menu(p->ui, &p->menu_comp, COMP_MENU_X, 4);"""
new = """        int target = p->comp_menu_target;
        // Transform gets its own menu. Copying "the transform" is almost never
        // what is meant: you want this object to stand WHERE that one stands,
        // and to keep its own rotation - or the other way round. One entry for
        // all four values, three for the parts.
        if (target == 0) {
            static const dai_ui_menu_item TR_MENU[] = {
                { DAI_ICON_RESET, "Reset Transform", nullptr },
                { DAI_ICON_COPY,  "Copy All",        nullptr },
                { DAI_ICON_COPY,  "Copy Position",   nullptr },
                { DAI_ICON_COPY,  "Copy Rotation",   nullptr },
                { DAI_ICON_COPY,  "Copy Scale",      nullptr },
                { DAI_ICON_SAVE,  "Paste",           nullptr },
            };
            int tpick = dai_ui_popup_menu(p->ui, &p->menu_comp, TR_MENU, 6);
            if (tpick >= 0 && dai_editor_selection_count(p->ed) > 0) {
                dai_node tn = dai_editor_selected(p->ed, 0);
                dai_node_desc tr{};
                if (dai_doc_get(d, tn, &tr) == DAI_OK) {
                    char buf[256];
                    if (tpick == 0) {
                        dai_doc_begin(d, "Reset Transform");
                        tr.position = dai_vec3{ 0, 0, 0 };
                        tr.rotation = dai_quat{ 0, 0, 0, 1 };
                        tr.scale = dai_vec3{ 1, 1, 1 };
                        dai_doc_set(d, tn, &tr);
                        dai_doc_commit(d);
                        dai_editor_resync(p->ed);
                    } else if (tpick >= 1 && tpick <= 4) {
                        // The clipboard says WHICH parts it holds, so a paste
                        // knows what it may touch. A blob that always claims
                        // to be a whole transform is how "copy position" ends
                        // up rotating things.
                        const char *what = tpick == 1 ? "all" : tpick == 2 ? "pos"
                                         : tpick == 3 ? "rot" : "scale";
                        std::snprintf(buf, sizeof(buf),
                            "transform:%s p=%g,%g,%g r=%g,%g,%g,%g s=%g,%g,%g", what,
                            (double)tr.position.x, (double)tr.position.y, (double)tr.position.z,
                            (double)tr.rotation.x, (double)tr.rotation.y,
                            (double)tr.rotation.z, (double)tr.rotation.w,
                            (double)tr.scale.x, (double)tr.scale.y, (double)tr.scale.z);
                        dai_editor_ui_clipboard_set(p, 0, buf);
                        dai_editor_ui_toast(p,
                            tpick == 1 ? "transform copied" :
                            tpick == 2 ? "position copied" :
                            tpick == 3 ? "rotation copied" : "scale copied", 1.5f);
                    } else if (tpick == 5) {
                        const char *cb = dai_editor_ui_clipboard_get(p, nullptr);
                        if (!cb || std::strncmp(cb, "transform:", 10) != 0) {
                            dai_editor_ui_toast(p, "the clipboard holds no transform", 2.0f);
                        } else {
                            const char *what = cb + 10;
                            bool all = std::strncmp(what, "all", 3) == 0;
                            bool pos = all || std::strncmp(what, "pos", 3) == 0;
                            bool rot = all || std::strncmp(what, "rot", 3) == 0;
                            bool scl = all || std::strncmp(what, "scale", 5) == 0;
                            float v[10] = { 0 };
                            const char *pp = std::strstr(cb, "p=");
                            const char *rr = std::strstr(cb, "r=");
                            const char *ss = std::strstr(cb, "s=");
                            if (pp) std::sscanf(pp + 2, "%f,%f,%f", &v[0], &v[1], &v[2]);
                            if (rr) std::sscanf(rr + 2, "%f,%f,%f,%f", &v[3], &v[4], &v[5], &v[6]);
                            if (ss) std::sscanf(ss + 2, "%f,%f,%f", &v[7], &v[8], &v[9]);
                            dai_doc_begin(d, "Paste Transform");
                            // Onto the WHOLE selection: pasting one position
                            // onto twelve selected crates is a real thing to
                            // want, and it is the same rule the rest of the
                            // inspector follows.
                            for (uint32_t si = 0; si < dai_editor_selection_count(p->ed); ++si) {
                                dai_node on = dai_editor_selected(p->ed, si);
                                dai_node_desc orr{};
                                if (dai_doc_get(d, on, &orr) != DAI_OK) continue;
                                if (pos) orr.position = dai_vec3{ v[0], v[1], v[2] };
                                if (rot) orr.rotation = dai_quat{ v[3], v[4], v[5], v[6] };
                                if (scl) orr.scale = dai_vec3{ v[7], v[8], v[9] };
                                dai_doc_set(d, on, &orr);
                            }
                            dai_doc_commit(d);
                            dai_editor_resync(p->ed);
                            dai_editor_ui_toast(p, pos && rot && scl ? "transform pasted"
                                                : pos ? "position pasted"
                                                : rot ? "rotation pasted" : "scale pasted", 1.5f);
                        }
                    }
                }
            }
            if (!p->menu_comp.open) p->comp_menu_target = -1;
            return;
        }
        static const dai_ui_menu_item COMP_MENU_X[] = {
            { DAI_ICON_RESET, "Reset", nullptr },
            { DAI_ICON_COPY, "Copy Component", nullptr },
            { DAI_ICON_SAVE, "Paste Component Values", nullptr },
            { DAI_ICON_CLOSE, "Remove Component", nullptr },
        };
        int cpick = dai_ui_popup_menu(p->ui, &p->menu_comp, COMP_MENU_X, 4);"""
assert s.count(old) == 1, 'component menu not found'
s = s.replace(old, new)
io.open(p, 'w', encoding='utf-8').write(s)
print('transform menu: all, position, rotation, scale')
