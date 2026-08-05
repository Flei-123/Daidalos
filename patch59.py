#!/usr/bin/env python3
# patch59 - the interface size that would not stay, the panel a saved layout
# never heard of, a confirm box with two answers, and a prefab that arrived
# wrapped in a box.
import sys, os
ROOT = os.path.dirname(os.path.abspath(__file__))
def rd(p):
    with open(os.path.join(ROOT, p), 'r', encoding='utf-8') as f: return f.read()
def wr(p, s):
    full = os.path.join(ROOT, p); bak = full + '.bak_p59'
    if not os.path.exists(bak):
        with open(bak, 'w', encoding='utf-8') as f: f.write(rd(p))
    with open(full, 'w', encoding='utf-8') as f: f.write(s)
def sub1(t, old, new, what):
    if new in t: print('skip (already applied): ' + what); return t
    if old not in t: print('MISS: ' + what); sys.exit(1)
    if t.count(old) != 1: print('AMBIG %d: %s' % (t.count(old), what)); sys.exit(1)
    return t.replace(old, new)

# ============================== 1. two settings were sharing one number
# prefs.ui_scale was written by TWO things that mean different things:
#
#   apply_font()      -> "the interface font is 16 px"      (ui_scale = 16/13)
#   save_prefs_now()  -> "the display override is Auto"     (ui_scale = 0)
#
# Whichever ran last won, and the second one runs on every project open and
# every exit - so the interface size was reliably thrown away, and ui_scale
# was left at 0, which reads back as "auto" and is not even a size. They are
# two settings. They get two fields.
s = rd('include/dai_project.h')
s = sub1(s,
"""    int   script_editor;     /* 0 = the built-in editor, 1 = the external one */""",
"""    int   script_editor;     /* 0 = the built-in editor, 1 = the external one */
    /* The DISPLAY scale override: 0 = follow the monitor, 1.5 = force 150%.
     * Not the same thing as ui_scale, which is how big the interface font is.
     * They shared one field once; the interface size was lost every restart. */
    float dpi_scale;""",
    'prefs dpi_scale field')
wr('include/dai_project.h', s)

s = rd('src/dai_project.cpp')
s = sub1(s,
"""        else if (key == "script-editor") parse_int(after, &out->script_editor);""",
"""        else if (key == "script-editor") parse_int(after, &out->script_editor);
        else if (key == "dpi-scale")     parse_floats(after, &out->dpi_scale, 1);""",
    'prefs dpi parse')
s = sub1(s,
"""    if (pr->script_editor != d.script_editor)       put(t, "script-editor %d\\n", pr->script_editor);""",
"""    if (pr->script_editor != d.script_editor)       put(t, "script-editor %d\\n", pr->script_editor);
    if (pr->dpi_scale != d.dpi_scale)               put(t, "dpi-scale %s\\n", fstr(pr->dpi_scale).c_str());""",
    'prefs dpi write')
wr('src/dai_project.cpp', s)

s = rd('examples/editor_demo.cpp')
s = sub1(s,
"""static void save_prefs_now() {
    if (!g_prefs) return;
    g_prefs->ui_scale = g_dpi_pref;
    g_prefs->language = dai_tr_lang_get();""",
"""static void save_prefs_now() {
    if (!g_prefs) return;
    // NOT ui_scale: that one belongs to apply_font, which writes it through
    // g_ui_scale_out the moment the size changes. Writing the display
    // override into it here is what used to erase the interface size.
    g_prefs->dpi_scale = g_dpi_pref;
    g_prefs->language = dai_tr_lang_get();""",
    'save_prefs_now keeps ui_scale')

s = sub1(s,
"""    if (g_ui_scale_out) *g_ui_scale_out = px / 13.0f;""",
"""    if (g_ui_scale_out) *g_ui_scale_out = px / 13.0f;
    save_prefs_now();      // the size settles now, not at some clean exit""",
    'apply_font persists')

s = sub1(s,
"""        float dpi = dai_window_dpi_scale(win);
        if (dpi > 1.02f && prefs.ui_scale > 0.98f && prefs.ui_scale < 1.02f) {
            prefs.ui_scale = dpi;""",
"""        float dpi = dai_window_dpi_scale(win);
        if (dpi > 1.02f && prefs.dpi_scale < 0.05f) {
            prefs.dpi_scale = dpi;""",
    'first run dpi guess')

s = sub1(s,
"""    g_dpi_pref = prefs.ui_scale;""",
"""    g_dpi_pref = prefs.dpi_scale;""",
    'read dpi pref')
wr('examples/editor_demo.cpp', s)

# ======================== 2. a saved layout never hears about a new panel
# dai_dock_add_tab is idempotent and returns on the first line when the title
# is already registered - so registering every frame costs a string compare
# and means a panel added in a later version APPEARS for someone who has a
# layout.txt from an earlier one. Registering only inside layout_reset made
# every new panel invisible to every existing install.
s = rd('src/dai_editor_ui.cpp')
s = sub1(s,
"""    dai_dock_begin(p->dock, ui, 0.0f, TOP, vw, vh - TOP - BOTTOM);""",
"""    // Panels this build knows about, every frame. Idempotent by design (see
    // dai_dock_add_tab): the first call registers, the rest return at once.
    // A closed panel stays closed - dai_dock_begin skips closed registrations.
    dai_dock_add_tab(p->dock, "Console", "Project");
    dai_dock_add_tab(p->dock, "Audio", "Project");
    dai_dock_add_tab(p->dock, "Script", "Scene");

    dai_dock_begin(p->dock, ui, 0.0f, TOP, vw, vh - TOP - BOTTOM);""",
    'register panels every frame')

# ======================================== 3. a question is not an option
s = sub1(s,
"""        dai_ui_menu_item items[2] = {
            { DAI_ICON_TRASH, q.c_str(), nullptr, 1 },
            { nullptr, "Delete permanently", nullptr, 0 },
        };""",
"""        dai_ui_menu_item items[2] = {
            { DAI_ICON_TRASH, q.c_str(), nullptr, 1 },   // the question, not a choice
            { nullptr, "Delete permanently", nullptr, 0 },
        };""",
    'delete popup comment')
wr('src/dai_editor_ui.cpp', s)

s = rd('src/dai_ui.cpp')
s = sub1(s,
"""    int hovered = -1;
    if (over && result == -2)
        hovered = (int)((my - y - pad) / row_h);

    for (uint32_t i = 0; i < count; ++i) {
        float ry = y + pad + row_h * (float)i;
        if ((int)i == hovered)
            dai_ui_rect(ui, x + 1.0f, ry, w - 2.0f, row_h, ui->style.button_hover);
        float tx = x + 8.0f;
        if (items[i].icon && dai_ui_has_icon(ui, items[i].icon)) {
            float isz = row_h - 8.0f;
            dai_ui_icon_at(ui, items[i].icon, tx, ry + 4.0f, isz, ui->style.text_dim);
            tx += isz + 6.0f;
        }
        dai_ui_text(ui, tx, ry + 4.0f, items[i].label, ui->style.text);""",
"""    int hovered = -1;
    if (over && result == -2)
        hovered = (int)((my - y - pad) / row_h);
    // A caption row is not a choice: it must not highlight and it must not be
    // returned. A confirm box built from "the question" plus "the action"
    // otherwise reads as two answers to a yes/no question, which is one
    // answer too many.
    if (hovered >= 0 && hovered < (int)count && items[hovered].header) hovered = -1;

    for (uint32_t i = 0; i < count; ++i) {
        float ry = y + pad + row_h * (float)i;
        bool head = items[i].header != 0;
        if ((int)i == hovered)
            dai_ui_rect(ui, x + 1.0f, ry, w - 2.0f, row_h, ui->style.button_hover);
        float tx = x + 8.0f;
        if (items[i].icon && dai_ui_has_icon(ui, items[i].icon)) {
            float isz = row_h - 8.0f;
            dai_ui_icon_at(ui, items[i].icon, tx, ry + 4.0f, isz, ui->style.text_dim);
            tx += isz + 6.0f;
        }
        dai_ui_text(ui, tx, ry + 4.0f, items[i].label,
                    head ? ui->style.text_dim : ui->style.text);
        if (head) {
            float ry2 = ry + row_h - 1.0f;
            dai_ui_rect(ui, x + 6.0f, ry2, w - 12.0f, 1.0f, ui->style.panel_border);
        }""",
    'popup header rows')
wr('src/dai_ui.cpp', s)

# ============================ 4. one object in, one object out
s = rd('src/dai_doc_text.cpp')
s = sub1(s,
"""    // The instance root is a transform node that points at the file. It gets
    // the prefab root's own transform so the instance lands where the original
    // was authored.
    dai_node_desc root = dai_node_desc_default();
    std::vector<dai_node> sids((size_t)dai_doc_count(sub));
    if (!sids.empty()) {
        dai_doc_nodes(sub, sids.data(), (uint32_t)sids.size());
        dai_doc_get(sub, sids[0], &root);
    }
    root.parent = parent;
    root.no_body = 1;                 // the pieces carry the physics
    root.mesh = 0xFFFFFFFFu;
    root.asset[0] = 0;
    snprintf(root.prefab, sizeof(root.prefab), "%s", path);

    dai_doc_begin(d, "Instantiate prefab");
    dai_node made = dai_doc_add(d, &root);
    if (made) graft(d, sub, made);
    dai_doc_commit(d);
    dai_doc_destroy(sub);
    return made;""",
"""    std::vector<dai_node> sids((size_t)dai_doc_count(sub));
    if (!sids.empty()) dai_doc_nodes(sub, sids.data(), (uint32_t)sids.size());

    // A prefab of ONE object instantiates as ONE object.
    //
    // It used to always build a wrapper: an empty transform pointing at the
    // file, with the prefab's contents grafted underneath. For a crate made of
    // twelve pieces that is right - they need something to hang off. For a
    // prefab of a single cube it is a box inside a box, and the thing you
    // select, move and look at in the inspector is the empty one, which has no
    // mesh, no collider and nothing to edit. Unity gives you the cube.
    //
    // So: when the file holds exactly one node, THAT node is the instance and
    // carries the prefab link itself.
    if (sids.size() == 1) {
        dai_node_desc only = dai_node_desc_default();
        dai_doc_get(sub, sids[0], &only);
        only.parent = parent;
        snprintf(only.prefab, sizeof(only.prefab), "%s", path);
        dai_doc_begin(d, "Instantiate prefab");
        dai_node made1 = dai_doc_add(d, &only);
        dai_doc_commit(d);
        dai_doc_destroy(sub);
        return made1;
    }

    // The instance root is a transform node that points at the file. It gets
    // the prefab root's own transform so the instance lands where the original
    // was authored.
    dai_node_desc root = dai_node_desc_default();
    if (!sids.empty()) dai_doc_get(sub, sids[0], &root);
    root.parent = parent;
    root.no_body = 1;                 // the pieces carry the physics
    root.mesh = 0xFFFFFFFFu;
    root.asset[0] = 0;
    snprintf(root.prefab, sizeof(root.prefab), "%s", path);

    dai_doc_begin(d, "Instantiate prefab");
    dai_node made = dai_doc_add(d, &root);
    if (made) graft(d, sub, made);
    dai_doc_commit(d);
    dai_doc_destroy(sub);
    return made;""",
    'single node prefab')
wr('src/dai_doc_text.cpp', s)
print('patch59 ok')
