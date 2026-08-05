import io

p = 'examples/editor_demo.cpp'
s = io.open(p, encoding='utf-8').read()

# ---------------------------------------------------------------------------
# 1) Das HUD im Game-View. Nach der Editor-UI, vor dai_ui_end - es gehoert in
#    dieselbe Zeichenliste, sonst waere es hinter den Panels.
# ---------------------------------------------------------------------------
old = """        diag_step("ui end");
        dai_ui_end(ui);"""
new = """        // ---- the game's own UI ------------------------------------------
        // Drawn into the Game view, clipped to it, in the same draw list as
        // everything else - a HUD in a second pass would sit on top of the
        // panels the Game view is docked next to.
        //
        // Shown while EDITING too, not only during play. A label you cannot
        // see until you press play is a label you place by trial and error.
        {
            float hx, hy, hw, hh;
            if (dai_editor_ui_game_view_rect(panels, &hx, &hy, &hw, &hh))
                dai_hud_draw(ui, doc, hx, hy, hw, hh, 1.0f, hud_resolve, nullptr);
        }
        diag_step("ui end");
        dai_ui_end(ui);"""
assert s.count(old) == 1, 'ui end anchor not found'
s = s.replace(old, new)

# ---------------------------------------------------------------------------
# 2) Sprachtabellen laden, wenn ein Projekt aufgeht.
# ---------------------------------------------------------------------------
old = """    std::snprintf(g_assets_dir, sizeof(g_assets_dir), "%s", dai_project_asset_dir(g_project));"""
new = """    std::snprintf(g_assets_dir, sizeof(g_assets_dir), "%s", dai_project_asset_dir(g_project));
    // Which languages this project has, and which one to preview in. Done
    // here because it is the one moment the answer can change.
    strings_scan();
    {
        dai_project_settings ps = dai_project_settings_default();
        dai_project_settings_load(g_project, &ps);
        const char *want = ps.language[0] ? ps.language
                         : (g_langs.empty() ? "" : g_langs[0].c_str());
        strings_use(want);
    }"""
assert s.count(old) == 1, 'assets dir anchor not found'
s = s.replace(old, new)

# ---------------------------------------------------------------------------
# 3) node.setText - ohne das ist ein Punktestand eine Konstante.
# ---------------------------------------------------------------------------
old = """static dai_script_node_host g_node_host = { sh_find, sh_get_pos, sh_set_pos, sh_get_rot, sh_set_rot, nullptr };"""
new = """// The label on a node, from a script. THE reason a HUD exists: a score that
// cannot change is a decoration.
//
// It writes the DOCUMENT, not a live copy, because a Text component has no
// body and nothing simulates it - and dai_hud_draw reads the document. Play
// still restores it, because Stop restores the whole document snapshot.
static void sh_set_text(double id, const char *str, void *) {
    if (!g_scene_doc) return;
    dai_node_desc r{};
    if (dai_doc_get(g_scene_doc, (dai_node)(uint32_t)id, &r) != DAI_OK) return;
    std::snprintf(r.text, sizeof(r.text), "%s", str ? str : "");
    if (!r.text_on) r.text_on = 1;      // setting text on a node means show it
    // No dai_doc_begin/commit: a score changing sixty times a second must not
    // put sixty entries on the undo stack. dai_doc_set without a transaction
    // is the "this is not an edit" path.
    dai_doc_set(g_scene_doc, (dai_node)(uint32_t)id, &r);
}

static dai_script_node_host g_node_host = { sh_find, sh_get_pos, sh_set_pos, sh_get_rot, sh_set_rot,
                                            sh_set_text, nullptr };"""
assert s.count(old) == 1, 'node host not found'
s = s.replace(old, new)

io.open(p, 'w', encoding='utf-8').write(s)
print('editor_demo: HUD drawn, strings loaded per project, node.setText')

# ---------------------------------------------------------------------------
# 4) Der Script-Host bekommt set_text.
# ---------------------------------------------------------------------------
p = 'include/dai_script.h'
s = io.open(p, encoding='utf-8').read()
old = """typedef struct dai_script_node_host {"""
new = """/* `set_text` writes a node's Text component - the whole point of having one,
 * since a score that cannot change is a decoration. */
typedef struct dai_script_node_host {"""
assert s.count(old) == 1, 'node host struct comment anchor not found'
s = s.replace(old, new)
io.open(p, 'w', encoding='utf-8').write(s)
print('dai_script.h: comment')
