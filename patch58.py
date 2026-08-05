#!/usr/bin/env python3
# patch58 - the host end of the script editor: read/write, Ctrl+S, and the
# preference that decides which editor a double click opens.
import sys, os
ROOT = os.path.dirname(os.path.abspath(__file__))
def rd(p):
    with open(os.path.join(ROOT, p), 'r', encoding='utf-8') as f: return f.read()
def wr(p, s):
    full = os.path.join(ROOT, p); bak = full + '.bak_p58'
    if not os.path.exists(bak):
        with open(bak, 'w', encoding='utf-8') as f: f.write(rd(p))
    with open(full, 'w', encoding='utf-8') as f: f.write(s)
def sub1(t, old, new, what):
    if new in t: print('skip (already applied): ' + what); return t
    if old not in t: print('MISS: ' + what); sys.exit(1)
    if t.count(old) != 1: print('AMBIG %d: %s' % (t.count(old), what)); sys.exit(1)
    return t.replace(old, new)

# ============================================ 1. one more preference to keep
s = rd('include/dai_project.h')
s = sub1(s,
"""    int   language;          /* dai_lang, 0 = English                         */""",
"""    int   language;          /* dai_lang, 0 = English                         */
    int   script_editor;     /* 0 = the built-in editor, 1 = the external one */""",
    'prefs field')
wr('include/dai_project.h', s)

s = rd('src/dai_project.cpp')
s = sub1(s,
"""        else if (key == "language")      parse_int(after, &out->language);""",
"""        else if (key == "language")      parse_int(after, &out->language);
        else if (key == "script-editor") parse_int(after, &out->script_editor);""",
    'prefs parse')
s = sub1(s,
"""    if (pr->language != d.language)                 put(t, "language %d\\n", pr->language);""",
"""    if (pr->language != d.language)                 put(t, "language %d\\n", pr->language);
    if (pr->script_editor != d.script_editor)       put(t, "script-editor %d\\n", pr->script_editor);""",
    'prefs write')
wr('src/dai_project.cpp', s)

# ============================================ 2. reading and writing a file
s = rd('examples/editor_demo.cpp')
s = sub1(s,
"""static int asset_delete(const char *rel, const char *, void *) {""",
"""// The built-in script editor's two file operations. Same rules as everything
// else that touches the assets folder: relative, never upwards, never absolute.
static uint32_t asset_read_text(const char *rel, char *out, uint32_t max, void *) {
    if (!rel || !out || max < 2 || !g_assets_dir[0]) return 0;
    out[0] = 0;
    if (std::strstr(rel, "..") || rel[0] == '/' || rel[0] == '\\\\') return 0;
    char full[700];
    std::snprintf(full, sizeof(full), "%s/%s", g_assets_dir, rel);
    FILE *f = std::fopen(full, "rb");
    if (!f) return 0;
    size_t n = std::fread(out, 1, max - 1, f);
    std::fclose(f);
    // Editors work in '\\n'. A file written on Windows arrives with '\\r\\n' and
    // every one of those carriage returns would otherwise be a visible glyph
    // at the end of every line.
    size_t w = 0;
    for (size_t i = 0; i < n; ++i) if (out[i] != '\\r') out[w++] = out[i];
    out[w] = 0;
    return (uint32_t)w;
}

static int asset_write_text(const char *rel, const char *text, void *) {
    if (!rel || !text || !g_assets_dir[0]) return 0;
    if (std::strstr(rel, "..") || rel[0] == '/' || rel[0] == '\\\\') return 0;
    char full[700];
    std::snprintf(full, sizeof(full), "%s/%s", g_assets_dir, rel);
    make_parent_dirs(full);
    FILE *f = std::fopen(full, "wb");
    if (!f) return 0;
    size_t n = std::strlen(text);
    size_t got = std::fwrite(text, 1, n, f);
    int ok = (std::fclose(f) == 0) && got == n;
    return ok ? 1 : 0;
}

static int asset_delete(const char *rel, const char *, void *) {""",
    'file io host')

s = sub1(s,
"""    dai_editor_ui_delete_host(panels, asset_delete, nullptr);""",
"""    dai_editor_ui_delete_host(panels, asset_delete, nullptr);
    dai_editor_ui_file_host(panels, asset_read_text, asset_write_text, nullptr);
    dai_editor_ui_script_editor_pref(panels, prefs.script_editor);""",
    'wire file host')

# ============================================ 3. Ctrl+S belongs to the editor
s = sub1(s,
"""        if (ctrl && pressed(8) && !typing) {
            const char *sp = scene_path ? scene_path : (g_scene_path[0] ? g_scene_path : nullptr);""",
"""        // Ctrl+S while a script is being edited saves the SCRIPT. Anything
        // else and it is the scene, as it always was. `typing` is false for
        // the code editor on purpose - Ctrl+S is not a character.
        if (ctrl && pressed(8) && dai_editor_ui_script_save(panels)) {
            // handled by the script panel
        } else if (ctrl && pressed(8) && !typing) {
            const char *sp = scene_path ? scene_path : (g_scene_path[0] ? g_scene_path : nullptr);""",
    'ctrl-s routing')

# ================================= 4. the preference, where preferences live
s = sub1(s,
"""    dai_editor_ui_scale_host(panels, apply_ui_scale, g_dpi_pref, nullptr);""",
"""    dai_editor_ui_scale_host(panels, apply_ui_scale, g_dpi_pref, nullptr);
    g_panels_for_prefs = panels;""",
    'panels for prefs')

s = sub1(s,
"""static dai_prefs    *g_prefs = nullptr;
static void save_prefs_now() {
    if (!g_prefs) return;
    g_prefs->ui_scale = g_dpi_pref;
    g_prefs->language = dai_tr_lang_get();
    dai_prefs_save(g_prefs);
}""",
"""static dai_prefs    *g_prefs = nullptr;
static dai_editor_ui *g_panels_for_prefs = nullptr;
static void save_prefs_now() {
    if (!g_prefs) return;
    g_prefs->ui_scale = g_dpi_pref;
    g_prefs->language = dai_tr_lang_get();
    if (g_panels_for_prefs)
        g_prefs->script_editor = dai_editor_ui_script_editor_pref_get(g_panels_for_prefs);
    dai_prefs_save(g_prefs);
}""",
    'save script editor pref')
wr('examples/editor_demo.cpp', s)

# ==================== 5. the Settings row that switches it, in the editor UI
s = rd('src/dai_editor_ui.cpp')
s = sub1(s,
"""    if (dai_ui_option(ui, "Theme", &p->settings_theme, THEMES, 3)) {""",
"""    {
        // Which editor a double click on a script opens. Both are real
        // answers: the built-in one is here and instant, the external one has
        // your extensions - and the project carries a .d.ts so it understands
        // the engine either way.
        static const char *const SCRIPT_ED[] = { "Built-in editor", "External (VS Code)" };
        if (dai_ui_option(ui, "Scripts open in", &p->script_external, SCRIPT_ED, 2))
            dai_editor_ui_toast(p, p->script_external ? "scripts open externally"
                                                      : "scripts open in the Script tab", 2.0f);
    }
    if (dai_ui_option(ui, "Theme", &p->settings_theme, THEMES, 3)) {""",
    'settings row')
wr('src/dai_editor_ui.cpp', s)
print('patch58 ok')
