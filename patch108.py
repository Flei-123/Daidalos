import io

p = 'examples/editor_demo.cpp'
s = io.open(p, encoding='utf-8').read()

# ---------------------------------------------------------------------------
# Sprachtabellen des PROJEKTS. Nicht die des Editors - dai_tr ist die andere.
# ---------------------------------------------------------------------------
old = """static dai_project_settings *g_psettings = nullptr;"""
new = """// ---- the project's own string tables ------------------------------------
//
// Assets/Strings/*.daistr, one file per language. The editor previews in the
// one picked here, so a German label can be checked without exporting - and
// a missing entry shows up as the key, in the Game view, at the size it will
// really be.
#include "dai_strings.h"
static dai_strings *g_strings = nullptr;
static char g_lang[16] = { 0 };                 // "de", "" = no table loaded
static std::vector<std::string> g_langs;        // codes found in the project
static std::vector<std::string> g_lang_names;   // what each calls itself

// Reads Assets/Strings and remembers which languages exist. Cheap, and only
// run when a project opens or the folder is refreshed.
static void strings_scan() {
    g_langs.clear();
    g_lang_names.clear();
    if (!g_assets_dir[0]) return;
    char dir[700];
    std::snprintf(dir, sizeof(dir), "%s/Strings", g_assets_dir);
    std::vector<std::string> files;
    list_dir_files(dir, files);            // defined next to the other helpers
    for (const std::string &f : files) {
        if (f.size() < 8 || f.compare(f.size() - 7, 7, ".daistr") != 0) continue;
        char full[900];
        std::snprintf(full, sizeof(full), "%s/%s", dir, f.c_str());
        dai_strings *t = dai_strings_create();
        char err[256] = { 0 };
        if (dai_strings_load(t, full, err, sizeof(err)) == DAI_OK) {
            const char *code = dai_strings_lang(t);
            const char *nm = dai_strings_name(t);
            std::string c = code && code[0] ? code : f.substr(0, f.size() - 7);
            g_langs.push_back(c);
            g_lang_names.push_back(nm && nm[0] ? nm : c);
        } else if (g_panels_for_log) {
            char line[400];
            std::snprintf(line, sizeof(line), "Strings/%s: %s", f.c_str(), err);
            dai_editor_ui_log(g_panels_for_log, 1, line);
        }
        dai_strings_destroy(t);
    }
}

static void strings_use(const char *code) {
    if (!g_strings) g_strings = dai_strings_create();
    std::snprintf(g_lang, sizeof(g_lang), "%s", code ? code : "");
    if (!g_lang[0] || !g_assets_dir[0]) return;
    char full[900];
    std::snprintf(full, sizeof(full), "%s/Strings/%s.daistr", g_assets_dir, g_lang);
    char err[256] = { 0 };
    if (dai_strings_load(g_strings, full, err, sizeof(err)) != DAI_OK && g_panels_for_log) {
        char line[400];
        std::snprintf(line, sizeof(line), "language %s: %s", g_lang, err);
        dai_editor_ui_log(g_panels_for_log, 1, line);
    }
}

// What a Text component's contents mean to a player. Static buffer: the
// caller uses it immediately, and this is called once per label per frame.
static const char *hud_resolve(const char *text, void *) {
    static char buf[256];
    return dai_strings_resolve(g_strings, text, buf, sizeof(buf));
}

static dai_project_settings *g_psettings = nullptr;"""
assert s.count(old) == 1, 'psettings anchor not found'
s = s.replace(old, new)

# --- Sprachauswahl in den Projekteinstellungen ------------------------------
old = """    dai_ui_separator(ui);
    dai_ui_input_text(ui, "App name", ps.app_name, sizeof(ps.app_name));"""
new = """    dai_ui_separator(ui);
    // Language. The list is what the project HAS, not a list of languages the
    // world contains: an option that resolves to no file is an option that
    // makes every label fall back to its key.
    {
        dai_ui_label(ui, "Language (Assets/Strings/*.daistr)");
        if (g_langs.empty()) {
            dai_ui_label(ui, "no tables yet - a label shows its own words until there are");
        } else {
            std::vector<const char *> names;
            int sel = 0;
            for (size_t i = 0; i < g_langs.size(); ++i) {
                names.push_back(g_lang_names[i].c_str());
                if (g_langs[i] == g_lang) sel = (int)i;
            }
            if (dai_ui_option(ui, "Preview", &sel, names.data(), (int)names.size()) &&
                sel >= 0 && sel < (int)g_langs.size()) {
                strings_use(g_langs[(size_t)sel].c_str());
                std::snprintf(ps.language, sizeof(ps.language), "%s", g_lang);
            }
            dai_ui_help(ui, "What the Game view shows, and what the exported game starts in. "
                            "A key with no entry shows the key.");
        }
        char rescan[32] = "Rescan";
        if (dai_ui_button_fit(ui, rescan)) { strings_scan(); strings_use(g_lang); }
    }
    dai_ui_separator(ui);
    dai_ui_input_text(ui, "App name", ps.app_name, sizeof(ps.app_name));"""
assert s.count(old) == 1, 'app name anchor not found'
s = s.replace(old, new)

io.open(p, 'w', encoding='utf-8').write(s)
print('editor_demo: string tables, language picker')
