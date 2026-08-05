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

s = sub1(s,
"""    dai_ui_popup menu_addcomp{};      // the Add Component button's list""",
"""    dai_ui_popup menu_addcomp{};      // (superseded by addcomp_list below)
    dai_ui_searchlist addcomp_list{}; // Add Component, Unity style: search +
                                      // filtered list, scripts included""", "addcomp state")

# ---- the button opens the searchable list now ------------------------------
s = sub1(s,
"""    if (dai_ui_button(p->ui, "Add Component")) {""",
"""    if (dai_ui_button(p->ui, "Add Component...")) {""", "button label")
s = sub1(s,
"""        dai_ui_popup_open(&p->menu_addcomp, mx2, my2);
    }""",
"""        dai_ui_searchlist_open(&p->addcomp_list, mx2, my2);
        p->addcomp_list.wants_focus = 1;
        p->addcomp_node = dai_editor_selected(p->ed, 0);
    }""", "open searchlist")

# ---- the searchable list at frame level ------------------------------------
s = sub1(s,
"""    // The Add Component list. Entries flip between add and remove so one menu
    // covers both directions - a component that is already there offers to go.
    if (p->menu_addcomp.open && dai_editor_selection_count(p->ed) > 0) {""",
"""    // Add Component, the Unity shape: a search field over everything you
    // could add - the built-in components AND every behaviour file in the
    // project. Flat lists stop working the day a project has forty scripts,
    // and forty scripts is a Tuesday.
    if (p->addcomp_list.open && p->addcomp_node != DAI_INVALID_NODE) {
        struct CompEntry { std::string label; std::string cat; int kind; std::string path; };
        // kind: 0 add rigidbody, 1 add collider, 2 add camera, 3 add light,
        //       4 add sprite, 5 add audio, 6 attach the behaviour in `path`.
        dai_node_desc ar2{};
        int have_node = dai_doc_get(d, p->addcomp_node, &ar2) == DAI_OK;
        std::vector<CompEntry> entries;
        if (have_node) {
            if (ar2.no_rigidbody) entries.push_back({ "Rigidbody", "Physics", 0, "" });
            if (ar2.no_collider)  entries.push_back({ "Collider", "Physics", 1, "" });
            if (!ar2.camera)      entries.push_back({ "Camera", "Rendering", 2, "" });
            if (!ar2.light)       entries.push_back({ "Light", "Rendering", 3, "" });
            if (!ar2.sprite)      entries.push_back({ "Sprite (2D)", "Rendering", 4, "" });
            if (!ar2.audio_event[0]) entries.push_back({ "Audio Source", "Audio", 5, "" });
            // Attachments flip to "Remove" entries, the way Unity greys out
            // what is already there - but removal keeps the same list shape.
            if (!ar2.no_rigidbody) entries.push_back({ "Remove Rigidbody", "Physics", 10, "" });
            if (!ar2.no_collider)  entries.push_back({ "Remove Collider", "Physics", 11, "" });
            if (ar2.camera)        entries.push_back({ "Remove Camera", "Rendering", 12, "" });
            if (ar2.light)         entries.push_back({ "Remove Light", "Rendering", 13, "" });
            if (ar2.sprite)        entries.push_back({ "Remove Sprite (2D)", "Rendering", 14, "" });
            if (ar2.audio_event[0]) entries.push_back({ "Remove Audio Source", "Audio", 15, "" });
        }
        // Every script in the project is a component. Listed with its folder,
        // because two scripts called "player" in different folders are not
        // the same thing.
        for (const char *a : p->assets) {
            if (!a || !is_behaviour_file(a)) continue;
            bool on_node = false;
            if (have_node && ar2.script[0]) {
                std::string cur;
                for (const char *c = ar2.script; ; ++c) {
                    if (*c == ';' || !*c) {
                        if (cur == a) on_node = true;
                        cur.clear();
                        if (!*c) break;
                    } else cur += *c;
                }
            }
            if (on_node) continue;
            entries.push_back({ base_of(a), "Scripts", 6, a });
        }

        // Category headers, Unity's grey separators: they are items too, so
        // the keyboard can walk past them without treating them as picks.
        std::vector<std::string> flat;
        std::vector<int>         row_kind;   // -1 = header, else index into entries
        std::string last_cat;
        for (const CompEntry &e : entries) {
            if (e.cat != last_cat) { last_cat = e.cat; flat.push_back(e.cat); row_kind.push_back(-1); }
            flat.push_back(e.label); row_kind.push_back(1);
        }
        std::vector<dai_ui_menu_item> items(flat.size());
        for (size_t i = 0; i < flat.size(); ++i) {
            if (row_kind[i] < 0) items[i] = { DAI_ICON_LAYERS, flat[i].c_str(), nullptr };
            else {
                const char *ic = DAI_ICON_SCRIPT;
                for (const CompEntry &e : entries)
                    if (e.label == flat[i]) {
                        if (e.cat == "Physics") ic = DAI_ICON_SETTINGS;
                        else if (e.cat == "Audio") ic = DAI_ICON_AUDIO;
                        else if (e.cat == "Rendering")
                            ic = e.kind == 2 || e.kind == 12 ? DAI_ICON_CAMERA
                               : e.kind == 3 || e.kind == 13 ? DAI_ICON_LIGHT : DAI_ICON_SPRITE;
                        break;
                    }
                items[i] = { ic, flat[i].c_str(), nullptr };
            }
        }
        int pick = dai_ui_searchlist_draw(p->ui, &p->addcomp_list, items.data(), (uint32_t)items.size());
        if (pick >= 0 && pick < (int)flat.size() && row_kind[pick] >= 0 && have_node) {
            const std::string &lbl = flat[pick];
            const CompEntry *sel = nullptr;
            for (const CompEntry &e : entries) if (e.label == lbl) { sel = &e; break; }
            if (sel) {
                dai_doc_begin(d, sel->kind >= 10 ? "Remove component" : "Add component");
                switch (sel->kind) {
                case 0: ar2.no_rigidbody = 0; ar2.no_body = 0;
                        ar2.friction = p->def_friction; ar2.restitution = p->def_restitution; break;
                case 1: ar2.no_collider = 0; ar2.no_body = 0; break;
                case 2: ar2.camera = 1; break;
                case 3: ar2.light = 1; break;
                case 4: ar2.sprite = 1; break;
                case 5: std::snprintf(ar2.audio_event, sizeof(ar2.audio_event), "click"); break;
                case 6: {
                    std::vector<std::string> list = script_list(ar2.script);
                    list.push_back(sel->path);
                    script_join(ar2.script, sizeof(ar2.script), list);
                    break;
                }
                case 10: ar2.no_rigidbody = 1; if (ar2.no_collider) ar2.no_body = 1; break;
                case 11: ar2.no_collider = 1; if (ar2.no_rigidbody) ar2.no_body = 1; break;
                case 12: ar2.camera = 0; break;
                case 13: ar2.light = 0; break;
                case 14: ar2.sprite = 0; break;
                case 15: ar2.audio_event[0] = 0; break;
                }
                dai_doc_set(d, p->addcomp_node, &ar2);
                dai_doc_commit(d);
                dai_editor_resync(p->ed);
            }
        }
        if (!p->addcomp_list.open) p->addcomp_node = DAI_INVALID_NODE;
    }

    // The Add Component list. Entries flip between add and remove so one menu
    // covers both directions - a component that is already there offers to go.
    if (0 && p->menu_addcomp.open && dai_editor_selection_count(p->ed) > 0) {""",
"addcomp searchlist")

s = sub1(s,
"""    dai_node    mesh_menu_node = DAI_INVALID_NODE;""",
"""    dai_node    mesh_menu_node = DAI_INVALID_NODE;
    dai_node    addcomp_node = DAI_INVALID_NODE;   // who Add Component was opened for""", "addcomp node state")

wr(P, s)
print("patch16 done")
