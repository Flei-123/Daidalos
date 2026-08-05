#!/usr/bin/env python3
# patch47 - prefabs that are actually linked: making one turns the object into
# an instance, the inspector says so, and one can be dragged into the scene.
import sys, os
ROOT = os.path.dirname(os.path.abspath(__file__))
def rd(p):
    with open(os.path.join(ROOT, p), 'r', encoding='utf-8') as f: return f.read()
def wr(p, s):
    full = os.path.join(ROOT, p); bak = full + '.bak_p47'
    if not os.path.exists(bak):
        with open(bak, 'w', encoding='utf-8') as f: f.write(rd(p))
    with open(full, 'w', encoding='utf-8') as f: f.write(s)
def sub1(t, old, new, what):
    if new in t: print('skip (already applied): ' + what); return t
    if old not in t: print('MISS: ' + what); sys.exit(1)
    if t.count(old) != 1: print('AMBIG %d: %s' % (t.count(old), what)); sys.exit(1)
    return t.replace(old, new)

s = rd('src/dai_editor_ui.cpp')

# ==================================== -1. one button shape for the whole editor
# The prefab bar wants the Project window's small button, and that is defined
# eight hundred lines further down. A declaration beats a second button that
# nearly matches the first.
s = sub1(s,
"""static void project_expand_to(dai_editor_ui *p, const std::string &dir);""",
"""static void project_expand_to(dai_editor_ui *p, const std::string &dir);
/* The Project window's small button, used by the inspector's prefab bar too -
 * one button shape for the whole editor beats two that nearly match. */
static int browser_button(dai_editor_ui *p, float x, float y, float w, float h,
                          const char *label);""",
    'browser_button forward decl')

# ================================================== 0. the pill's dead read
# patch45 moved the drop-target clear ABOVE the pill that reads it, so the
# pill could never say where a dragged file would land. Read first, clear
# after - the values belong to this frame either way.
s = sub1(s,
"""        // Set while the folders were drawn, spent here, gone by the next
        // frame - so a stale target cannot survive the Project window being
        // closed, or hidden behind another tab.
        p->proj_drop_ok = 0;
        p->proj_drop_dir.clear();
        if (!p->drag_script.empty()) {
            std::string lbl = base_of(p->drag_script);
            if (p->proj_drop_ok)
                lbl += "  ->  " + (p->proj_drop_dir.empty() ? std::string("Assets")
                                                            : p->proj_drop_dir);
            else if (is_behaviour_file(p->drag_script) && p->hover_node != DAI_INVALID_NODE)
                lbl += "  ->  attach";""",
"""        if (!p->drag_script.empty()) {
            std::string lbl = base_of(p->drag_script);
            if (p->proj_drop_ok)
                lbl += "  ->  " + (p->proj_drop_dir.empty() ? std::string("Assets")
                                                            : p->proj_drop_dir);
            else if (is_behaviour_file(p->drag_script) && p->hover_node != DAI_INVALID_NODE)
                lbl += "  ->  attach";
            else if (is_scene_file(p->drag_script) &&
                     (dai_ui_root_hovered(ui, "Scene") || dai_ui_root_hovered(ui, "Hierarchy")))
                lbl += "  ->  place in scene";""",
    'pill reads before clear')

s = sub1(s,
"""            dai_ui_text(ui, dmx + 21.0f, dmy + 14.0f, lbl.c_str(), st->text);
            dai_ui_layer_pop(ui);
            dai_ui_claim_mouse(ui);
        }""",
"""            dai_ui_text(ui, dmx + 21.0f, dmy + 14.0f, lbl.c_str(), st->text);
            dai_ui_layer_pop(ui);
            dai_ui_claim_mouse(ui);
        }
        // Set while the folders were drawn, spent above, gone by the next
        // frame - so a stale target cannot survive the Project window being
        // closed, or hidden behind another tab.
        p->proj_drop_ok = 0;
        p->proj_drop_dir.clear();""",
    'clear after pill')

# =========================================== 1. what a .daidalos file even is
s = sub1(s,
"""// One console filter chip: the level's icon in its OWN colours, the count in""",
"""// A scene file - which is also what a prefab is. The two are the same format
// on purpose (a prefab is a scene with one root), so the only thing that can
// tell them apart is where they live: Scenes/ holds scenes, everything else
// holding a .daidalos holds a prefab.
static bool is_scene_file(const std::string &path) {
    const std::string ext = ".daidalos";
    return path.size() > ext.size() &&
           path.compare(path.size() - ext.size(), ext.size(), ext) == 0;
}

static bool is_scene_asset(const std::string &path) {
    return is_scene_file(path) &&
           (path.compare(0, 7, "Scenes/") == 0 || path.compare(0, 7, "scenes/") == 0);
}

// One console filter chip: the level's icon in its OWN colours, the count in""",
    'is_scene_file helper')

# ============================== 2. dragging a prefab out of Project and in
s = sub1(s,
"""                if (p->proj_drop_ok) {
                    project_move(p, p->drag_script, p->proj_drop_dir);
                } else if (is_behaviour_file(p->drag_script)) {""",
"""                if (p->proj_drop_ok) {
                    project_move(p, p->drag_script, p->proj_drop_dir);
                } else if (is_scene_file(p->drag_script) && !is_scene_asset(p->drag_script) &&
                           (dai_ui_root_hovered(ui, "Scene") ||
                            dai_ui_root_hovered(ui, "Hierarchy"))) {
                    // A prefab dragged into the viewport (or onto the
                    // hierarchy) is placed. The host does the instantiating -
                    // it owns the assets root - so this hands over the same
                    // pending pick a double click produces, which already
                    // links the instance to the file it came from.
                    for (const char *a : p->assets) {
                        if (a && p->drag_script == a) { p->pending_asset = a; break; }
                    }
                    p->pending_as_tree = 0;
                } else if (is_behaviour_file(p->drag_script)) {""",
    'drag prefab into scene')

# ================================================ 3. the inspector says so
s = sub1(s,
"""    if (r.asset[0]) {
        if (dai_ui_input_text(p->ui, "Asset", p->asset_buf, sizeof(p->asset_buf)))
            std::snprintf(r.asset, sizeof(r.asset), "%s", p->asset_buf);
    }
    dai_ui_separator(p->ui);""",
"""    if (r.asset[0]) {
        if (dai_ui_input_text(p->ui, "Asset", p->asset_buf, sizeof(p->asset_buf)))
            std::snprintf(r.asset, sizeof(r.asset), "%s", p->asset_buf);
    }
    // ---- Prefab -------------------------------------------------------------
    // A node with a prefab path IS an instance: its children are not stored in
    // this scene, they are expanded from that file. Unity puts a blue bar and
    // the source name at the top of the inspector for exactly this, because
    // "why did my edit come back" has one answer and it is this line.
    if (r.prefab[0]) {
        dai_ui *ui3 = p->ui;
        const dai_ui_style *st3 = dai_ui_style_of(ui3);
        float hx3, hy3;
        dai_ui_cursor_pos(ui3, &hx3, &hy3);
        float hw3 = dai_ui_panel_width(ui3) - st3->padding * 2;
        float hh3 = dai_ui_text_height(ui3) + 10.0f;
        dai_ui_advance(ui3, 0, hh3 + 2.0f);
        dai_ui_rrect(ui3, hx3, hy3, hw3, hh3, 4.0f, rgba(0x25, 0x3A, 0x52, 255));
        dai_ui_rect(ui3, hx3, hy3, 3.0f, hh3, st3->accent);
        float tx3 = hx3 + 9.0f;
        if (dai_ui_has_icon(ui3, DAI_ICON_C_PREFAB)) {
            dai_ui_icon_at(ui3, DAI_ICON_C_PREFAB, tx3, hy3 + (hh3 - 14.0f) * 0.5f, 14.0f,
                           st3->accent);
            tx3 += 19.0f;
        }
        char pl3[160];
        std::snprintf(pl3, sizeof(pl3), "Prefab  %s", base_of(r.prefab).c_str());
        dai_ui_text(ui3, tx3, hy3 + (hh3 - dai_ui_text_height(ui3)) * 0.5f, pl3, st3->text);
        // Select shows the source in the Project window; Unpack breaks the
        // link and keeps the objects, which is Unity's "Unpack Prefab".
        float bw3 = dai_ui_text_width(ui3, "Unpack") + 16.0f;
        float sw3 = dai_ui_text_width(ui3, "Select") + 16.0f;
        float by3 = hy3 + (hh3 - 18.0f) * 0.5f;
        if (browser_button(p, hx3 + hw3 - bw3 - 4.0f, by3, bw3, 18.0f, "Unpack")) {
            r.prefab[0] = 0;
            dai_editor_ui_toast(p, "prefab link removed - the objects stay", 2.0f);
        }
        if (browser_button(p, hx3 + hw3 - bw3 - sw3 - 10.0f, by3, sw3, 18.0f, "Select")) {
            p->proj_dir = parent_of(r.prefab);
            project_expand_to(p, p->proj_dir);
            p->proj_list_scroll = 0.0f;
            for (size_t ai = 0; ai < p->assets.size(); ++ai)
                if (p->assets[ai] && r.prefab == p->assets[ai]) { p->asset_sel = (int)ai; break; }
            dai_editor_ui_panel_open(p, "Project");
            dai_dock_focus(p->dock, "Project");
        }
    }
    dai_ui_separator(p->ui);""",
    'inspector prefab block')
wr('src/dai_editor_ui.cpp', s)

# ================================== 4. making a prefab LINKS what it came from
s = rd('examples/editor_demo.cpp')
s = sub1(s,
"""    dai_result rc = dai_doc_prefab_save(g_prefab_doc, n, path);
    if (rc != DAI_OK) {
        char msg[800];
        std::snprintf(msg, sizeof(msg), "prefab save failed: %s", path);
        if (g_panels_for_log) dai_editor_ui_log(g_panels_for_log, 2, msg);
        std::printf("%s\\n", msg);
        return 0;
    }
    return 1;
}""",
"""    dai_result rc = dai_doc_prefab_save(g_prefab_doc, n, path);
    if (rc != DAI_OK) {
        char msg[800];
        std::snprintf(msg, sizeof(msg), "prefab save failed: %s", path);
        if (g_panels_for_log) dai_editor_ui_log(g_panels_for_log, 2, msg);
        std::printf("%s\\n", msg);
        return 0;
    }
    // The object it was made FROM becomes an instance of it. That is what
    // Unity does and it is the only version that is any use: without it you
    // get a file nothing points at, editing the prefab leaves the original
    // untouched, and the scene has a silent duplicate of everything. The link
    // is a path relative to the assets root - the same string the browser and
    // dai_doc_prefab_reload use, so a reload finds it.
    {
        dai_node_desc rec{};
        if (dai_doc_get(g_prefab_doc, n, &rec) == DAI_OK) {
            dai_doc_begin(g_prefab_doc, "Create prefab");
            std::snprintf(rec.prefab, sizeof(rec.prefab), "%s", rel_path);
            dai_doc_set(g_prefab_doc, n, &rec);
            dai_doc_commit(g_prefab_doc);
        }
    }
    return 1;
}""",
    'prefab_save links the source')
wr('examples/editor_demo.cpp', s)
print('patch47 ok')
