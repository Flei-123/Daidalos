import io

p = 'src/dai_editor_ui.cpp'
s = io.open(p, encoding='utf-8').read()

# ===========================================================================
# 1) Zustand: Shift-Anker fuer beide Listen, und die Datei-Mehrfachauswahl.
# ===========================================================================
old = """    int          click_ctrl = 0;        // ctrl was held: add to the selection"""
new = """    int          click_ctrl = 0;        // ctrl was held: add to the selection
    int          click_shift = 0;       // shift was held: take everything between
    // The anchor a shift-click measures from. One per list, because the two
    // lists are two orderings - and "everything between" only means anything
    // inside one of them.
    dai_node     range_anchor_node = DAI_INVALID_NODE;
    std::string  range_anchor_asset;
    // Files picked in the Project window. The folder selection stays single:
    // a range of folders is not a thing anyone does, and the browser navigates
    // by folder.
    std::vector<std::string> asset_multi;"""
assert s.count(old) == 1, 'click_ctrl not found'
s = s.replace(old, new)

# ===========================================================================
# 2) Die Hierarchie-Zeile merkt sich Shift.
# ===========================================================================
old = """            p->click_dbl = 0;
            p->click_ctrl = p->last_ctrl_held ? 1 : 0;
        }"""
new = """            p->click_dbl = 0;
            p->click_ctrl = p->last_ctrl_held ? 1 : 0;
            p->click_shift = p->last_shift_held ? 1 : 0;
        }"""
assert s.count(old) == 1, 'hierarchy click arm not found'
s = s.replace(old, new)

# ===========================================================================
# 3) Beim Loslassen: Bereich, Strg oder ersetzen - und die ANDERE Liste
#    verliert ihre Auswahl, weil es nur eine gibt.
# ===========================================================================
old = """                if (p->click_kind == 3 && p->click_node != DAI_INVALID_NODE) {
                    dai_editor_select(p->ed, p->click_node, p->click_ctrl);
                    p->inspect_asset.clear();      // an object is the selection now
                }"""
new = """                if (p->click_kind == 3 && p->click_node != DAI_INVALID_NODE) {
                    if (p->click_shift && p->range_anchor_node != DAI_INVALID_NODE) {
                        // Shift takes everything BETWEEN, in the order the
                        // rows are on screen - which is the order the tree was
                        // walked, not the order the ids were made in. Windows,
                        // Unity and every file manager work this way, and a
                        // hierarchy that only does ctrl-click makes you click
                        // forty times to select forty rows.
                        select_range_nodes(p, p->range_anchor_node, p->click_node,
                                           p->click_ctrl);
                    } else {
                        dai_editor_select(p->ed, p->click_node, p->click_ctrl);
                        p->range_anchor_node = p->click_node;
                    }
                    // ONE selection, not two. The Project window keeps its own
                    // highlight otherwise, and then F2 renames a file while
                    // you are looking at a selected object - which is exactly
                    // what it used to do.
                    p->inspect_asset.clear();
                    p->asset_sel = -1;
                    p->asset_multi.clear();
                    p->proj_sel_folder.clear();
                    p->last_pick.clear();
                }"""
assert s.count(old) == 1, 'hierarchy click release not found'
s = s.replace(old, new)

# ===========================================================================
# 4) Der Bereichs-Helfer. Er braucht die SICHTBARE Reihenfolge der Zeilen.
# ===========================================================================
old = """static void reparent_node(dai_editor_ui *p, dai_node child, dai_node parent) {"""
new = """// Every node in the order the hierarchy DRAWS them: depth first, children
// under their parent, folded subtrees skipped. A shift-range means "the rows
// between these two", and rows are what is on screen - selecting by id order
// would grab objects that are nowhere near each other.
static void hierarchy_order(const dai_editor_ui *p, dai_doc *d, dai_node parent,
                            std::vector<dai_node> &out) {
    std::vector<dai_node> all((size_t)dai_doc_count(d));
    uint32_t n = all.empty() ? 0 : dai_doc_nodes(d, all.data(), (uint32_t)all.size());
    for (uint32_t i = 0; i < n; ++i) {
        dai_node_desc r{};
        if (dai_doc_get(d, all[i], &r) != DAI_OK) continue;
        if (r.parent != parent) continue;
        out.push_back(all[i]);
        if (p->folded.count(all[i])) continue;      // collapsed: its rows are not on screen
        hierarchy_order(p, d, all[i], out);
    }
}

static void select_range_nodes(dai_editor_ui *p, dai_node a, dai_node b, int keep) {
    dai_doc *d = dai_editor_doc(p->ed);
    std::vector<dai_node> rows;
    hierarchy_order(p, d, DAI_INVALID_NODE, rows);
    int ia = -1, ib = -1;
    for (size_t i = 0; i < rows.size(); ++i) {
        if (rows[i] == a) ia = (int)i;
        if (rows[i] == b) ib = (int)i;
    }
    if (ia < 0 || ib < 0) { dai_editor_select(p->ed, b, keep); return; }
    if (ia > ib) { int t = ia; ia = ib; ib = t; }
    // Shift alone REPLACES the selection with the range - shift+ctrl adds to
    // it. That is the Windows rule, and the one muscle memory expects.
    if (!keep) dai_editor_deselect_all(p->ed);
    for (int i = ia; i <= ib; ++i) dai_editor_select(p->ed, rows[(size_t)i], 1);
}

static void reparent_node(dai_editor_ui *p, dai_node child, dai_node parent) {"""
assert s.count(old) == 1, 'reparent_node anchor not found'
s = s.replace(old, new)

# ===========================================================================
# 5) Datei-Klick: Shift-Bereich, Strg-Zusatz, und die Hierarchie raeumt auf.
# ===========================================================================
old = """                        p->click_kind = 2;"""
new = """                        p->click_kind = 2;
                        p->click_ctrl = p->last_ctrl_held ? 1 : 0;
                        p->click_shift = p->last_shift_held ? 1 : 0;"""
assert s.count(old) == 1, 'file click arm not found'
s = s.replace(old, new)

io.open(p, 'w', encoding='utf-8').write(s)
print('selection: one list, shift ranges, anchors')
