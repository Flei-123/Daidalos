import io

p = 'src/dai_editor_ui.cpp'
s = io.open(p, encoding='utf-8').read()

# ===========================================================================
# 1) Ein Behaviour im Project-Fenster traegt dasselbe Icon wie im Inspector.
# ===========================================================================
old = """    if (e == "js" || e == "ts")                       return DAI_ICON_SCRIPT;
    if (e == "cpp" || e == "cc" || e == "cxx" || e == "h" || e == "hpp") return DAI_ICON_SCRIPT;"""
new = """    // The SAME glyph the component header uses (dai_ui_header_icon_col with
    // DAI_ICON_C_SCRIPT). A file in the Project window and the component it
    // becomes when you drop it on an object are one thing; showing them as two
    // different pictures makes the reader do a translation that has no content.
    if (e == "js" || e == "ts")                       return DAI_ICON_C_SCRIPT;
    if (e == "cpp" || e == "cc" || e == "cxx" || e == "h" || e == "hpp") return DAI_ICON_C_SCRIPT;"""
assert s.count(old) == 1, 'icon_for_asset script lines not found'
s = s.replace(old, new)

# ===========================================================================
# 2) Buttons so breit wie ihr Text - auch die, die es noch nicht waren.
# ===========================================================================
old = """    dai_ui_separator(p->ui);
    if (dai_ui_button(p->ui, "Add Component...")) {"""
new = """    dai_ui_separator(p->ui);
    if (dai_ui_button_fit(p->ui, "Add Component...")) {"""
assert s.count(old) == 1, 'Add Component button not found'
s = s.replace(old, new)

for lbl in ('"Check for updates"', '"Copy this to the clipboard"',
            '"Assign to selection"', '"Place"', '"As tree"'):
    old = 'dai_ui_button(ui, %s)' % lbl
    new = 'dai_ui_button_fit(ui, %s)' % lbl
    if s.count(old):
        s = s.replace(old, new)
    old = 'dai_ui_button(p->ui, %s)' % lbl
    new = 'dai_ui_button_fit(p->ui, %s)' % lbl
    if s.count(old):
        s = s.replace(old, new)

# ===========================================================================
# 3) Typisierte Objektfelder.
# ===========================================================================
old = """struct ParamDecl { std::string name, def, tip; int type = PARAM_NODE; };"""
new = """// `ntype` is the DECLARED TYPE of a node reference - Unity's rule, where a
// field spelled `Rigidbody target` offers only rigidbodies in its picker and
// refuses everything else on a drag. Empty means "any object", which is what
// every reference meant before types existed.
struct ParamDecl { std::string name, def, tip, ntype; int type = PARAM_NODE; };

// Which component a typed reference asks for, and what to call it on screen.
// Kept as one table so the label, the icon and the test can never drift apart.
struct NodeTypeInfo { const char *key, *label, *icon; };
static const NodeTypeInfo NODE_TYPES[] = {
    { "transform", "Transform", DAI_ICON_C_TRANSFORM },
    { "camera",    "Camera",    DAI_ICON_C_CAMERA },
    { "light",     "Light",     DAI_ICON_C_LIGHT },
    { "rigidbody", "Rigidbody", DAI_ICON_C_BODY },
    { "body",      "Rigidbody", DAI_ICON_C_BODY },
    { "collider",  "Collider",  DAI_ICON_C_COLLIDER },
    { "sprite",    "Sprite",    DAI_ICON_C_SPRITE },
    { "audio",     "Audio",     DAI_ICON_C_AUDIO },
    { "mesh",      "Mesh",      DAI_ICON_C_MESH },
};
static const NodeTypeInfo *node_type_of(const std::string &k) {
    if (k.empty()) return nullptr;
    for (const NodeTypeInfo &t : NODE_TYPES)
        if (k == t.key) return &t;
    return nullptr;
}
// Does this node carry what the field asks for? A Transform is every node -
// in Unity too, which is why `Transform` fields accept anything.
static bool node_has_type(const dai_node_desc &r, const std::string &k) {
    if (k.empty() || k == "object" || k == "node" || k == "transform") return true;
    if (k == "camera")    return r.camera != 0;
    if (k == "light")     return r.light != 0;
    if (k == "rigidbody" || k == "body") return !r.no_rigidbody && !r.no_body;
    if (k == "collider")  return !r.no_collider && !r.no_body;
    if (k == "sprite")    return r.sprite != 0;
    if (k == "audio")     return r.audio_event[0] != 0 || r.audio_autoplay || r.audio_bus;
    if (k == "mesh")      return 1;   /* every node draws something */
    return true;
}"""
assert s.count(old) == 1, 'ParamDecl not found'
s = s.replace(old, new)

old = """                    if (t == "float" || t == "number") d.type = PARAM_FLOAT;
                    else if (t == "int")               d.type = PARAM_INT;
                    else if (t == "bool")              d.type = PARAM_BOOL;
                    else if (t == "string" || t == "text") d.type = PARAM_STRING;
                    else if (t == "header")            d.type = PARAM_HEADER;
                    else                               d.type = PARAM_NODE;"""
new = """                    if (t == "float" || t == "number") d.type = PARAM_FLOAT;
                    else if (t == "int")               d.type = PARAM_INT;
                    else if (t == "bool")              d.type = PARAM_BOOL;
                    else if (t == "string" || t == "text") d.type = PARAM_STRING;
                    else if (t == "header")            d.type = PARAM_HEADER;
                    else {
                        d.type = PARAM_NODE;
                        // Anything else IS the reference's type. An unknown
                        // word behaves as "any object" rather than dropping
                        // the field: a typo must not make a slot disappear.
                        if (t != "node" && t != "object") d.ntype = t;
                    }"""
assert s.count(old) == 1, 'param type mapping not found'
s = s.replace(old, new)

# --- das Feld selbst -------------------------------------------------------
old = """                        std::string disp = (val.empty() ? std::string("None") : val)
                                         + " (Object)";
                        int orc = dai_ui_object_field(p->ui, pd.name.c_str(), disp.c_str(),
                                                      DAI_ICON_C_TRANSFORM);"""
new = """                        const NodeTypeInfo *nt = node_type_of(pd.ntype);
                        const char *tname = nt ? nt->label : "Object";
                        // Unity writes the value and its type: "Main Camera
                        // (Transform)". The type is not decoration - it is the
                        // contract the picker and the drop both answer to.
                        std::string disp = (val.empty() ? std::string("None") : val)
                                         + " (" + tname + ")";
                        // An assignment that no longer fits - the camera lost
                        // its Camera component, the file was hand edited - is
                        // said out loud instead of being quietly wrong.
                        int bad = 0;
                        if (!val.empty() && nt) {
                            dai_node vn = dai_doc_find(d, val.c_str());
                            dai_node_desc vr{};
                            if (vn == DAI_INVALID_NODE) bad = 1;
                            else if (dai_doc_get(d, vn, &vr) == DAI_OK && !node_has_type(vr, pd.ntype))
                                bad = 1;
                            if (bad) disp = val + "  (not a " + tname + ")";
                        }
                        int orc = dai_ui_object_field(p->ui, pd.name.c_str(), disp.c_str(),
                                                      nt ? nt->icon : DAI_ICON_C_TRANSFORM);"""
assert s.count(old) == 1, 'object field draw not found'
s = s.replace(old, new)

old = """                            p->obj_pick_node = n;
                            p->obj_pick_entry = (int)si;
                            std::snprintf(p->obj_pick_key, sizeof(p->obj_pick_key),
                                          "%s", pd.name.c_str());"""
new = """                            p->obj_pick_node = n;
                            p->obj_pick_entry = (int)si;
                            std::snprintf(p->obj_pick_key, sizeof(p->obj_pick_key),
                                          "%s", pd.name.c_str());
                            std::snprintf(p->obj_pick_type, sizeof(p->obj_pick_type),
                                          "%s", pd.ntype.c_str());"""
assert s.count(old) == 1, 'obj pick assignment not found'
s = s.replace(old, new)

# --- der Picker filtert ----------------------------------------------------
old = """        std::vector<std::string> names;
        names.push_back("None");
        for (uint32_t i = 0; i < na; ++i) {
            dai_node_desc od2{};
            if (dai_doc_get(od, all[i], &od2) == DAI_OK && od2.name[0])
                names.push_back(od2.name);
        }
        std::vector<dai_ui_menu_item> items(names.size());
        for (size_t i = 0; i < names.size(); ++i)
            items[i] = { i == 0 ? nullptr : DAI_ICON_C_TRANSFORM, names[i].c_str(), nullptr, 0 };"""
new = """        // Only what the field's type accepts. A list that offers a light to a
        // Camera slot is a list that has to be read twice, and the second read
        // is the one that goes wrong.
        std::string want = p->obj_pick_type;
        const NodeTypeInfo *pnt = node_type_of(want);
        std::vector<std::string> names;
        names.push_back("None");
        for (uint32_t i = 0; i < na; ++i) {
            dai_node_desc od2{};
            if (dai_doc_get(od, all[i], &od2) != DAI_OK || !od2.name[0]) continue;
            if (!node_has_type(od2, want)) continue;
            names.push_back(od2.name);
        }
        std::vector<dai_ui_menu_item> items(names.size());
        for (size_t i = 0; i < names.size(); ++i)
            items[i] = { i == 0 ? nullptr : (pnt ? pnt->icon : DAI_ICON_C_TRANSFORM),
                         names[i].c_str(), nullptr, 0 };"""
assert s.count(old) == 1, 'object picker list not found'
s = s.replace(old, new)

old = """    dai_node    obj_pick_node = DAI_INVALID_NODE;"""
if s.count(old) != 1:
    # Feldname anders - suchen
    import re
    m = re.search(r'\n(\s*)dai_node\s+obj_pick_node[^\n]*\n', s)
    assert m, 'obj_pick_node member not found'
    old = m.group(0)
    new = old + m.group(1) + 'char obj_pick_type[32] = { 0 };   // the field\'s declared type, "" = any\n'
else:
    new = old + "\n    char obj_pick_type[32] = { 0 };   // the field's declared type, \"\" = any"
s = s.replace(old, new, 1)

io.open(p, 'w', encoding='utf-8').write(s)
print('dai_editor_ui.cpp: script icon, fitted buttons, typed object fields')
