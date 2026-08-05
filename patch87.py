import io

def rd(p): return io.open(p, encoding='utf-8').read()
def wr(p, s): io.open(p, 'w', encoding='utf-8').write(s)
def rep(s, old, new, n=1, tag=''):
    c = s.count(old)
    assert c == n, 'count %d != %d for %s :: %r' % (c, n, tag, old[:70])
    return s.replace(old, new)

E = 'src/dai_editor_ui.cpp'
e = rd(E)

# ---------------------------------------------------------------- state
e = rep(e, """    int         param_hover_entry = -1;
    char        param_hover_key[64] = { 0 };""",
"""    int         param_hover_entry = -1;
    char        param_hover_key[64] = { 0 };

    // A press is not a click yet. Selecting on the press is what made DRAGGING
    // a row repaint the inspector: you grab an object to move it and the panel
    // behind you has already switched to it. The pick waits here and is
    // committed when the button comes up without a drag having started.
    int          click_kind = 0;        // 0 none, 1 folder, 2 file, 3 node
    std::string  click_path;            // the folder or file it landed on
    int          click_dbl = 0;         // it was the second click of a pair
    dai_node     click_node = DAI_INVALID_NODE;
    int          click_ctrl = 0;        // ctrl was held: add to the selection
    dai_node     ping_node = DAI_INVALID_NODE;  // an object field was clicked

    // The object picker behind a reference field's target button.
    dai_ui_searchlist obj_list{};
    dai_node    obj_pick_node = DAI_INVALID_NODE;
    int         obj_pick_entry = -1;
    char        obj_pick_key[64] = { 0 };""", tag='state')

# ---------------------------------------------------------------- params
e = rep(e, """enum ParamType { PARAM_NODE = 0, PARAM_FLOAT, PARAM_INT, PARAM_BOOL, PARAM_STRING };
struct ParamDecl { std::string name, def; int type = PARAM_NODE; };""",
"""enum ParamType { PARAM_NODE = 0, PARAM_FLOAT, PARAM_INT, PARAM_BOOL, PARAM_STRING,
                 PARAM_HEADER };
// `tip` is the description shown while the pointer rests on the row - Unity's
// [Tooltip]. A header is not a field: it is the [Header("...")] line above one,
// and it carries its text in `name`.
struct ParamDecl { std::string name, def, tip; int type = PARAM_NODE; };""", tag='ParamDecl')

e = rep(e, """                if (colon != std::string::npos) {
                    std::string t = rest.substr(0, colon);
                    rest = rest.substr(colon + 1);
                    if (t == "float" || t == "number") d.type = PARAM_FLOAT;
                    else if (t == "int")               d.type = PARAM_INT;
                    else if (t == "bool")              d.type = PARAM_BOOL;
                    else if (t == "string" || t == "text") d.type = PARAM_STRING;
                    else                               d.type = PARAM_NODE;
                }
                size_t eq = rest.find('=');""",
"""                if (colon != std::string::npos) {
                    std::string t = rest.substr(0, colon);
                    rest = rest.substr(colon + 1);
                    if (t == "float" || t == "number") d.type = PARAM_FLOAT;
                    else if (t == "int")               d.type = PARAM_INT;
                    else if (t == "bool")              d.type = PARAM_BOOL;
                    else if (t == "string" || t == "text") d.type = PARAM_STRING;
                    else if (t == "header")            d.type = PARAM_HEADER;
                    else                               d.type = PARAM_NODE;
                }
                // "|" ends the declaration and begins its description.
                size_t bar = rest.find('|');
                if (bar != std::string::npos) {
                    d.tip = rest.substr(bar + 1);
                    rest = rest.substr(0, bar);
                }
                size_t eq = rest.find('=');""", tag='parse types')

# ---------------------------------------------------------------- hierarchy
e = rep(e, """        int rc = dai_ui_tree_item_icon(p->ui, node_icon(r), label, depth, kids,
                                       kids ? &open : nullptr,
                                       dai_editor_is_selected(p->ed, n));""",
"""        int rc = dai_ui_tree_item_icon(p->ui, node_icon(r), label, depth, kids,
                                       kids ? &open : nullptr,
                                       dai_editor_is_selected(p->ed, n) ||
                                       (p->click_kind == 3 && p->click_node == n));""",
       tag='tree highlight')

e = rep(e, """        if (rc & 1) {
            // Ctrl-click ADDS to the selection, the way Unity multi-selects in
            // the hierarchy; a plain click replaces.
            dai_editor_select(p->ed, n, p->last_ctrl_held);
            p->inspect_asset.clear();       // an object is the selection now
        }""",
"""        if (rc & 1) {
            // Armed, not done: the selection changes when the button comes up
            // and nothing was dragged. Ctrl still ADDS, the way Unity
            // multi-selects; a plain click replaces.
            p->click_kind = 3;
            p->click_node = n;
            p->click_path.clear();
            p->click_dbl = 0;
            p->click_ctrl = p->last_ctrl_held ? 1 : 0;
        }""", tag='tree click defer')

# ---------------------------------------------------------------- folder row
e = rep(e, """                    if (browser_row(p, list_x + 2.0f, ry, list_w - 4.0f, ROW,
                                    fic2, name.c_str(),
                                    ffull == p->proj_sel_folder) && clicks_ok) {
                        // One click SELECTS it. Two go in. Anything else and
                        // a folder can never be the thing you are pointing at
                        // - which is the state F2, drag and the strip at the
                        // bottom all need it to be able to reach.
                        p->last_pick = ffull;      // F2 renames THIS folder
                        p->proj_sel_folder = ffull;
                        p->asset_sel = -1;         // a folder and a file cannot both be it
                        p->inspect_asset = ffull;
                        dai_editor_deselect_all(p->ed);
                        if (dai_ui_double_click(ui)) {
                            p->proj_dir = p->proj_dir.empty() ? name : p->proj_dir + "/" + name;
                            project_expand_to(p, p->proj_dir);
                            p->proj_list_scroll = 0.0f;
                            p->proj_sel_folder.clear();
                        }
                    }""",
"""                    if (browser_row(p, list_x + 2.0f, ry, list_w - 4.0f, ROW,
                                    fic2, name.c_str(),
                                    ffull == p->proj_sel_folder ||
                                    (p->click_kind == 1 && p->click_path == ffull)) && clicks_ok) {
                        // One click SELECTS it. Two go in. Both wait for the
                        // button to come up: a press that dragged the folder
                        // somewhere else was never a click on it.
                        p->click_kind = 1;
                        p->click_path = ffull;
                        p->click_dbl = dai_ui_double_click(ui) ? 1 : 0;
                    }""", tag='folder row defer')

# ---------------------------------------------------------------- file row
e = rep(e, """                    if (browser_row(p, list_x + 2.0f, ry, list_w - 4.0f, ROW,
                                    icon_for_asset(full), label.c_str(), selected) && clicks_ok) {""",
"""                    if (browser_row(p, list_x + 2.0f, ry, list_w - 4.0f, ROW,
                                    icon_for_asset(full), label.c_str(),
                                    selected || (p->click_kind == 2 && p->click_path == full))
                            && clicks_ok) {""", tag='file row hdr')

e = rep(e, """                        // Double click is what everyone tries first: a script
                        // opens in the external editor, a prefab drops into
                        // the scene, a model is placed. A browser where the
                        // only way in is a button at the bottom is a browser
                        // people call broken.
                        if (dai_ui_double_click(ui)) {
                            if (is_behaviour_file(full) || is_text_file(full)) {
                                // Here or out there, whichever Settings says.
                                if (!p->script_external && p->file_read)
                                    dai_editor_ui_script_open(p, full.c_str());
                                else if (p->open_asset)
                                    p->open_asset(nullptr, full.c_str(), p->open_asset_user);
                            } else if (is_scene_file(full) && !is_scene_asset(full)) {
                                // A prefab OPENS. Dropping a copy into the
                                // scene is what dragging it does; a double
                                // click that silently added an object was
                                // never anybody's intention.
                                p->prefab_open_want = full;
                            } else {
                                // The PENDING pick is handed to the host after
                                // the frame, so it must be the host's own
                                // pointer and not a temporary - asset_at
                                // returns "" for a bad index, which the host
                                // then ignores.
                                p->pending_asset = fi >= 0 && fi < (int)p->assets.size()
                                                 ? p->assets[(size_t)fi] : nullptr;
                                p->pending_as_tree = 0;
                                p->pending_at_valid = 0;   // no drop point: it was a click
                            }
                        }
                        p->asset_sel = fi;
                        p->last_pick = full;
                        p->proj_sel_folder.clear();
                        p->inspect_asset = full;      // the inspector follows
                        dai_editor_deselect_all(p->ed);
                    }""",
"""                        // Armed only. What it means - select, open, place -
                        // is decided when the button comes up, because until
                        // then it may still turn out to have been a drag.
                        p->click_kind = 2;
                        p->click_path = full;
                        p->click_dbl = dai_ui_double_click(ui) ? 1 : 0;
                    }""", tag='file row defer')

# ------------------------------------------------- commit the pending click
e = rep(e, """        float dmx = 0, dmy = 0; int ddown = 0;
        dai_ui_mouse(ui, &dmx, &dmy, &ddown, nullptr);
        if (!p->drag_pending.empty() && p->drag_script.empty() && ddown) {""",
"""        float dmx = 0, dmy = 0; int ddown = 0;
        dai_ui_mouse(ui, &dmx, &dmy, &ddown, nullptr);

        // ---- a press became a click: nothing was dragged, so it counts -----
        // This runs BEFORE the drag state below is cleared, because "was this
        // a drag" is exactly the question being asked.
        if (!ddown && p->click_kind) {
            bool dragged = !p->drag_script.empty() || p->drag_node != DAI_INVALID_NODE;
            if (!dragged) {
                if (p->click_kind == 3 && p->click_node != DAI_INVALID_NODE) {
                    dai_editor_select(p->ed, p->click_node, p->click_ctrl);
                    p->inspect_asset.clear();      // an object is the selection now
                } else if (p->click_kind == 1) {
                    p->last_pick = p->click_path;  // F2 renames THIS folder
                    p->proj_sel_folder = p->click_path;
                    p->asset_sel = -1;             // a folder and a file cannot both be it
                    p->inspect_asset = p->click_path;
                    dai_editor_deselect_all(p->ed);
                    if (p->click_dbl) {
                        p->proj_dir = p->click_path;
                        project_expand_to(p, p->proj_dir);
                        p->proj_list_scroll = 0.0f;
                        p->proj_sel_folder.clear();
                    }
                } else if (p->click_kind == 2) {
                    // The row index is looked up again: the listing is rebuilt
                    // on every disk change, and one can happen between the
                    // press and the release.
                    int fi2 = -1;
                    for (size_t ai = 0; ai < p->assets.size(); ++ai)
                        if (p->assets[ai] && p->click_path == p->assets[ai]) { fi2 = (int)ai; break; }
                    const std::string &full2 = p->click_path;
                    if (p->click_dbl) {
                        if (is_behaviour_file(full2) || is_text_file(full2)) {
                            if (!p->script_external && p->file_read)
                                dai_editor_ui_script_open(p, full2.c_str());
                            else if (p->open_asset)
                                p->open_asset(nullptr, full2.c_str(), p->open_asset_user);
                        } else if (is_scene_file(full2) && !is_scene_asset(full2)) {
                            p->prefab_open_want = full2;
                        } else if (fi2 >= 0) {
                            p->pending_asset = p->assets[(size_t)fi2];
                            p->pending_as_tree = 0;
                            p->pending_at_valid = 0;   // no drop point: it was a click
                        }
                    }
                    p->asset_sel = fi2;
                    p->last_pick = full2;
                    p->proj_sel_folder.clear();
                    p->inspect_asset = full2;      // the inspector follows
                    dai_editor_deselect_all(p->ed);
                }
            }
            p->click_kind = 0;
            p->click_node = DAI_INVALID_NODE;
            p->click_dbl = 0;
        }
        // An object field was clicked: it points AT something, so show it.
        if (p->ping_node != DAI_INVALID_NODE) {
            if (dai_doc_valid(dai_editor_doc(p->ed), p->ping_node)) {
                dai_editor_select(p->ed, p->ping_node, 0);
                p->inspect_asset.clear();
                p->reveal_row_wanted = 1;
            }
            p->ping_node = DAI_INVALID_NODE;
        }

        if (!p->drag_pending.empty() && p->drag_script.empty() && ddown) {""",
       tag='commit click')

# ------------------------------------------------- asset inspector buttons
for lbl in ['p->inspect_dirty ? "Save material *" : "Save material"',
            '"Edit"', '"Open scene"', '"Open prefab"', '"Open externally"']:
    e = rep(e, 'dai_ui_button(p->ui, %s)' % lbl,
            'dai_ui_button_fit(p->ui, %s)' % lbl, tag='fit ' + lbl)

# ------------------------------------------------- bigger param buffers
e = e.replace('char keys[1024] = { 0 };', 'char keys[4096] = { 0 };')

# ------------------------------------------------- the node field, Unity shape
e = rep(e, """                for (const ParamDecl &pd : parse_params(keys)) {
                    std::string val = entry_param(slist[si], pd.name);
                    if (val.empty()) val = pd.def;      // the file's own default
                    char fid[96];""",
"""                for (const ParamDecl &pd : parse_params(keys)) {
                    if (pd.type == PARAM_HEADER) {
                        // Unity's [Header("...")]: air, then a bright line. It
                        // is not a field and has no value - it is the reason
                        // the eight fields under it belong together.
                        dai_ui_advance(p->ui, 0, 6.0f);
                        dai_ui_label(p->ui, pd.name.c_str());
                        continue;
                    }
                    std::string val = entry_param(slist[si], pd.name);
                    if (val.empty()) val = pd.def;      // the file's own default
                    char fid[96];""", tag='header row')

e = rep(e, """                    } else {
                        // A node reference: still a drop target, because the
                        // only sane way to name an object is to point at it.
                        char pl[384];
                        std::snprintf(pl, sizeof(pl), "%s: %s", pd.name.c_str(),
                                      val.empty() ? "none (drag an object here)" : val.c_str());
                        if (dai_ui_button(p->ui, pl) && !val.empty())
                            entry_set_param(slist[si], pd.name, "");   // click clears it
                        const char *hot2 = dai_ui_hot_label(p->ui);
                        if (hot2 && std::strcmp(hot2, pl) == 0) {
                            p->param_hover_entry = (int)si;
                            std::snprintf(p->param_hover_key, sizeof(p->param_hover_key),
                                          "%s", pd.name.c_str());
                        }
                    }
                }""",
"""                    } else {
                        // A node reference, drawn the way Unity draws one: the
                        // value with its type in brackets, and a target button
                        // that opens a searchable list of every object in the
                        // scene. Dragging one in still works - that gesture is
                        // faster when you can see both windows, and the picker
                        // is what you reach for when you cannot.
                        std::string disp = (val.empty() ? std::string("None") : val)
                                         + " (Object)";
                        int orc = dai_ui_object_field(p->ui, pd.name.c_str(), disp.c_str(),
                                                      DAI_ICON_C_TRANSFORM);
                        if (!pd.tip.empty()) dai_ui_help(p->ui, pd.tip.c_str());
                        if (orc == 2) {
                            float mx3 = 0, my3 = 0;
                            dai_ui_mouse(p->ui, &mx3, &my3, nullptr, nullptr);
                            dai_ui_searchlist_open(&p->obj_list, mx3 - 210.0f, my3);
                            p->obj_list.wants_focus = 1;
                            std::snprintf(p->obj_list.hint, sizeof(p->obj_list.hint),
                                          "Search objects...");
                            p->obj_pick_node = n;
                            p->obj_pick_entry = (int)si;
                            std::snprintf(p->obj_pick_key, sizeof(p->obj_pick_key),
                                          "%s", pd.name.c_str());
                        } else if (orc == 1 && !val.empty()) {
                            // Clicking the field shows what it points at.
                            p->ping_node = dai_doc_find(d, val.c_str());
                        }
                        const char *hot2 = dai_ui_hot_label(p->ui);
                        if (hot2 && std::strcmp(hot2, pd.name.c_str()) == 0) {
                            p->param_hover_entry = (int)si;
                            std::snprintf(p->param_hover_key, sizeof(p->param_hover_key),
                                          "%s", pd.name.c_str());
                        }
                    }
                    if (!pd.tip.empty() && pd.type != PARAM_NODE)
                        dai_ui_help(p->ui, pd.tip.c_str());
                }""", tag='object field')

# ------------------------------------------------- the file inspector's list
e = rep(e, """            dai_ui_label_fmt(p->ui, "Serialized fields: %u", (unsigned)decls.size());
            for (const ParamDecl &pd : decls) {
                static const char *TN[] = { "node", "float", "int", "bool", "string" };
                dai_ui_label_fmt(p->ui, "   %s %s%s%s", TN[pd.type], pd.name.c_str(),
                                 pd.def.empty() ? "" : " = ", pd.def.c_str());
            }""",
"""            unsigned nfields = 0;
            for (const ParamDecl &pd : decls) if (pd.type != PARAM_HEADER) ++nfields;
            dai_ui_label_fmt(p->ui, "Serialized fields: %u", nfields);
            for (const ParamDecl &pd : decls) {
                static const char *TN[] = { "node", "float", "int", "bool", "string" };
                if (pd.type == PARAM_HEADER) {
                    dai_ui_advance(p->ui, 0, 4.0f);
                    dai_ui_label(p->ui, pd.name.c_str());
                    continue;
                }
                dai_ui_label_fmt(p->ui, "   %s %s%s%s", TN[pd.type], pd.name.c_str(),
                                 pd.def.empty() ? "" : " = ", pd.def.c_str());
                if (!pd.tip.empty()) dai_ui_help(p->ui, pd.tip.c_str());
            }""", tag='file inspector fields')

# ------------------------------------------------- the object picker itself
e = rep(e, """    // The Add Component list. Entries flip between add and remove so one menu""",
"""    // The object picker: every node in the scene, filtered by the search box,
    // plus "None" at the top. It is the target button's half of the gesture -
    // drag and drop is the other half, and a field that only accepts a drag is
    // a field you cannot fill while the hierarchy is scrolled somewhere else.
    if (p->obj_list.open && p->obj_pick_node != DAI_INVALID_NODE) {
        dai_doc *od = dai_editor_doc(p->ed);
        std::vector<dai_node> all(dai_doc_count(od));
        uint32_t na = all.empty() ? 0 : dai_doc_nodes(od, all.data(), (uint32_t)all.size());
        std::vector<std::string> names;
        names.push_back("None");
        for (uint32_t i = 0; i < na; ++i) {
            dai_node_desc od2{};
            if (dai_doc_get(od, all[i], &od2) == DAI_OK && od2.name[0])
                names.push_back(od2.name);
        }
        std::vector<dai_ui_menu_item> items(names.size());
        for (size_t i = 0; i < names.size(); ++i)
            items[i] = { i == 0 ? nullptr : DAI_ICON_C_TRANSFORM, names[i].c_str(), nullptr, 0 };
        int pick = dai_ui_searchlist_draw(p->ui, &p->obj_list, items.data(),
                                          (uint32_t)items.size());
        if (pick >= 0 && pick < (int)names.size()) {
            dai_node_desc orr{};
            if (dai_doc_get(od, p->obj_pick_node, &orr) == DAI_OK) {
                std::vector<std::string> olist = script_list(orr.script);
                if (p->obj_pick_entry >= 0 && p->obj_pick_entry < (int)olist.size()) {
                    entry_set_param(olist[(size_t)p->obj_pick_entry], p->obj_pick_key,
                                    pick == 0 ? std::string() : names[(size_t)pick]);
                    dai_doc_begin(od, "Reference");
                    script_join(orr.script, sizeof(orr.script), olist);
                    dai_doc_set(od, p->obj_pick_node, &orr);
                    dai_doc_commit(od);
                    dai_editor_resync(p->ed);
                }
            }
            p->obj_pick_node = DAI_INVALID_NODE;
        }
    }

    // The Add Component list. Entries flip between add and remove so one menu""",
       tag='object picker')

# the two other search lists say what they search
e = rep(e, """        dai_ui_searchlist_open(&p->mat_list, mx2 - 150.0f, my2);
                        p->mat_list.wants_focus = 1;""",
"""        dai_ui_searchlist_open(&p->mat_list, mx2 - 150.0f, my2);
                        p->mat_list.wants_focus = 1;
                        std::snprintf(p->mat_list.hint, sizeof(p->mat_list.hint),
                                      "Search materials...");""", tag='mat hint')

e = rep(e, """        dai_ui_searchlist_open(&p->addcomp_list, mx2, my2);
        p->addcomp_list.wants_focus = 1;""",
"""        dai_ui_searchlist_open(&p->addcomp_list, mx2, my2);
        p->addcomp_list.wants_focus = 1;
        std::snprintf(p->addcomp_list.hint, sizeof(p->addcomp_list.hint),
                      "Search components...");""", tag='comp hint')

wr(E, e)
print('editor_ui ok')
