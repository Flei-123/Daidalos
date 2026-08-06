import io

p = 'src/dai_editor_ui.cpp'
s = io.open(p, encoding='utf-8').read()

# ===========================================================================
# 1) Datei-Auswahl: Strg legt dazu, Shift nimmt den ganzen Bereich.
# ===========================================================================
old = """                    p->asset_sel = fi2;
                    p->last_pick = full2;
                    p->proj_sel_folder.clear();
                    p->inspect_asset = full2;      // the inspector follows
                    dai_editor_deselect_all(p->ed);
                }"""
new = """                    // Ctrl adds one, shift takes the run between the anchor
                    // and here, a plain click replaces - the file manager
                    // rules, because this IS a file manager.
                    if (p->click_shift && !p->range_anchor_asset.empty()) {
                        std::vector<std::string> rows = project_visible_files(p);
                        int ia = -1, ib = -1;
                        for (size_t i = 0; i < rows.size(); ++i) {
                            if (rows[i] == p->range_anchor_asset) ia = (int)i;
                            if (rows[i] == full2) ib = (int)i;
                        }
                        if (ia >= 0 && ib >= 0) {
                            if (ia > ib) { int t = ia; ia = ib; ib = t; }
                            if (!p->click_ctrl) p->asset_multi.clear();
                            for (int i = ia; i <= ib; ++i) {
                                bool have = false;
                                for (const std::string &e : p->asset_multi)
                                    if (e == rows[(size_t)i]) { have = true; break; }
                                if (!have) p->asset_multi.push_back(rows[(size_t)i]);
                            }
                        }
                    } else if (p->click_ctrl) {
                        bool had = false;
                        for (size_t i = 0; i < p->asset_multi.size(); ++i)
                            if (p->asset_multi[i] == full2) {
                                p->asset_multi.erase(p->asset_multi.begin() + (long)i);
                                had = true; break;
                            }
                        if (!had) p->asset_multi.push_back(full2);
                        p->range_anchor_asset = full2;
                    } else {
                        p->asset_multi.assign(1, full2);
                        p->range_anchor_asset = full2;
                    }
                    p->asset_sel = fi2;
                    p->last_pick = full2;
                    p->proj_sel_folder.clear();
                    p->inspect_asset = full2;      // the inspector follows
                    dai_editor_deselect_all(p->ed);
                }"""
assert s.count(old) == 1, 'file click release not found'
s = s.replace(old, new)

# Ordner-Klick raeumt die Datei-Mehrfachauswahl mit ab.
old = """                    p->asset_sel = -1;             // a folder and a file cannot both be it
                    p->inspect_asset = p->click_path;"""
new = """                    p->asset_sel = -1;             // a folder and a file cannot both be it
                    p->asset_multi.clear();
                    p->range_anchor_asset.clear();
                    p->inspect_asset = p->click_path;"""
assert s.count(old) == 1, 'folder click not found'
s = s.replace(old, new)

# ===========================================================================
# 2) Die sichtbare Dateireihenfolge - dieselbe, die die Liste zeichnet.
# ===========================================================================
old = """static void reparent_node(dai_editor_ui *p, dai_node child, dai_node parent) {"""
new = """// The files of the open folder, in the order the browser lists them. A shift
// range is "the rows between these two", and rows means what is on screen.
static std::vector<std::string> project_visible_files(const dai_editor_ui *p) {
    std::vector<std::string> out;
    for (const char *a : p->assets) {
        if (!a) continue;
        std::string full = a;
        if (parent_of(full) != p->proj_dir) continue;
        out.push_back(full);
    }
    std::sort(out.begin(), out.end());
    return out;
}

static void reparent_node(dai_editor_ui *p, dai_node child, dai_node parent) {"""
assert s.count(old) == 1, 'reparent anchor not found'
s = s.replace(old, new)

# ===========================================================================
# 3) Eine gezogene Mehrfachauswahl haengt ALLE um.
# ===========================================================================
old = """            else if (p->drag_node != DAI_INVALID_NODE && p->hover_node != DAI_INVALID_NODE &&
                p->hover_node != p->drag_node) {
                reparent_node(p, p->drag_node,
                              p->hover_node == DAI_SCENE_ROOT_NODE ? DAI_INVALID_NODE
                                                                   : p->hover_node);
            } else if (p->drag_node != DAI_INVALID_NODE &&
                       p->hover_node == DAI_INVALID_NODE &&
                       dai_ui_root_hovered(ui, "Hierarchy")) {"""
new = """            else if (p->drag_node != DAI_INVALID_NODE && p->hover_node != DAI_INVALID_NODE &&
                p->hover_node != p->drag_node) {
                reparent_dragged(p, p->hover_node == DAI_SCENE_ROOT_NODE
                                        ? DAI_INVALID_NODE : p->hover_node);
            } else if (p->drag_node != DAI_INVALID_NODE &&
                       p->hover_node == DAI_INVALID_NODE &&
                       dai_ui_root_hovered(ui, "Hierarchy")) {"""
assert s.count(old) == 1, 'reparent drop not found'
s = s.replace(old, new)

old = """                reparent_node(p, p->drag_node, DAI_INVALID_NODE);
            } else if (p->drag_node != DAI_INVALID_NODE && p->param_hover_entry >= 0 &&"""
new = """                reparent_dragged(p, DAI_INVALID_NODE);
            } else if (p->drag_node != DAI_INVALID_NODE && p->param_hover_entry >= 0 &&"""
assert s.count(old) == 1, 'reparent to root not found'
s = s.replace(old, new)

old = """// The files of the open folder, in the order the browser lists them."""
new = """// Dropping a dragged node re-parents THE WHOLE SELECTION when the dragged one
// is part of it. Selecting eight crates and dropping them into a group has to
// move eight crates; moving one and leaving seven behind is not a gesture
// anybody meant to make.
//
// A node whose ancestor is also selected is skipped: the ancestor carries it
// already, and re-parenting both would pull the child out of its parent and
// then move it twice.
static void reparent_dragged(dai_editor_ui *p, dai_node parent);

// The files of the open folder, in the order the browser lists them."""
assert s.count(old) == 1, 'file order anchor not found'
s = s.replace(old, new)

old = """static void reparent_node(dai_editor_ui *p, dai_node child, dai_node parent) {"""
new = """static void reparent_dragged(dai_editor_ui *p, dai_node parent) {
    dai_doc *d = dai_editor_doc(p->ed);
    if (!dai_editor_is_selected(p->ed, p->drag_node) ||
        dai_editor_selection_count(p->ed) <= 1) {
        reparent_node(p, p->drag_node, parent);
        return;
    }
    std::vector<dai_node> sel;
    for (uint32_t i = 0; i < dai_editor_selection_count(p->ed); ++i)
        sel.push_back(dai_editor_selected(p->ed, i));
    for (dai_node n : sel) {
        bool ancestor_in_set = false;
        for (dai_node other : sel)
            if (other != n && is_descendant(d, n, other)) { ancestor_in_set = true; break; }
        if (ancestor_in_set) continue;
        reparent_node(p, n, parent);
    }
}

static void reparent_node(dai_editor_ui *p, dai_node child, dai_node parent) {"""
assert s.count(old) == 1, 'reparent_node def not found'
s = s.replace(old, new)

io.open(p, 'w', encoding='utf-8').write(s)
print('project ranges, multi reparent')
