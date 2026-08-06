import io

p = 'src/dai_editor_ui.cpp'
s = io.open(p, encoding='utf-8').read()

# ===========================================================================
# Ein eigenes Fenster fuer die Uebersetzungen.
#
# Warum ueberhaupt: die Tabellen sind Textdateien, und eine Textdatei je
# Sprache heisst, dass man beim Nachtragen eines Keys zwei Dateien offen hat
# und in beiden an dieselbe Stelle tippt. Genau daran stirbt Lokalisierung.
# Eine Zeile je Key, eine Spalte je Sprache, und die fehlenden Felder sind
# sichtbar leer - das ist die ganze Idee.
# ===========================================================================
old = """    dai_ui_searchlist obj_list{};"""
new = """    // ---- localisation ------------------------------------------------------
    // Held as a table in memory and written back per language on Save. The
    // host owns the files (it knows the assets folder); this owns the grid.
    struct LocEntry { std::string key; std::vector<std::string> vals; };
    std::vector<std::string> loc_langs;      // codes, in file order
    std::vector<std::string> loc_names;      // what each calls itself
    std::vector<LocEntry>    loc_rows;
    float loc_scroll = 0.0f;
    int   loc_dirty = 0;
    int   loc_loaded = 0;
    char  loc_newkey[96] = { 0 };
    char  loc_newlang[16] = { 0 };
    int   loc_edit_row = -1, loc_edit_col = -1;
    char  loc_edit_buf[256] = { 0 };
    // The host fills the table and writes it back - it is the one that knows
    // where Assets/Strings is.
    int  (*loc_load)(void *user) = nullptr;
    int  (*loc_save)(void *user) = nullptr;
    void *loc_user = nullptr;

    dai_ui_searchlist obj_list{};"""
assert s.count(old) == 1, 'obj_list anchor not found'
s = s.replace(old, new)

# --- das Panel -------------------------------------------------------------
old = """// The Script panel: the files you are editing, as tabs, with the code editor"""
new = """// The Localisation window: one row per key, one column per language.
//
// The alternative is what it replaces - two text files open side by side, and
// a new key typed into both at the same place. That is where localisation
// stops being done: not because translating is hard, but because ADDING a
// string is three steps in three files, and three steps get skipped.
//
// Missing entries are drawn as empty cells rather than as the key, on purpose.
// Everywhere else a missing key shows the key, because a player must never
// see a blank - but this is the one place whose job is to show what is NOT
// translated yet, and here a blank is the point.
static void loc_body(dai_editor_ui *p, float px, float py, float pw, float ph) {
    dai_ui *ui = p->ui;
    const dai_ui_style *st = dai_ui_style_of(ui);
    const float TH = dai_ui_text_height(ui);
    const float ROW = TH + 10.0f;
    const float BAR = ROW + 8.0f;

    if (!p->loc_loaded && p->loc_load) { p->loc_load(p->loc_user); p->loc_loaded = 1; }

    // ---- toolbar -----------------------------------------------------------
    float bx = px + 4.0f;
    if (browser_button(p, bx, py + 4.0f, 60.0f, ROW, "Reload")) {
        if (p->loc_load) p->loc_load(p->loc_user);
        p->loc_dirty = 0;
    }
    bx += 64.0f;
    {
        char lbl[32];
        std::snprintf(lbl, sizeof(lbl), p->loc_dirty ? "Save *" : "Save");
        if (browser_button(p, bx, py + 4.0f, 60.0f, ROW, lbl) && p->loc_save) {
            if (p->loc_save(p->loc_user)) {
                p->loc_dirty = 0;
                dai_editor_ui_toast(p, "string tables written", 1.5f);
            } else {
                dai_editor_ui_toast(p, "could not write the string tables", 2.5f);
            }
        }
    }
    bx += 68.0f;
    // New key. The field is where it is used - a dialog for one text box is a
    // dialog nobody opens twice.
    dai_ui_text(ui, bx, py + 4.0f + 5.0f, "New key", st->text_dim);
    bx += dai_ui_text_width(ui, "New key") + 6.0f;
    dai_ui_text_field(ui, "lockey", bx, py + 4.0f, 160.0f, ROW, p->loc_newkey,
                      sizeof(p->loc_newkey), nullptr);
    bx += 164.0f;
    if (browser_button(p, bx, py + 4.0f, 30.0f, ROW, "+") && p->loc_newkey[0]) {
        bool have = false;
        for (const auto &e : p->loc_rows) if (e.key == p->loc_newkey) have = true;
        if (have) dai_editor_ui_toast(p, "that key is already in the table", 2.0f);
        else {
            dai_editor_ui::LocEntry e;
            e.key = p->loc_newkey;
            e.vals.assign(p->loc_langs.size(), std::string());
            p->loc_rows.push_back(e);
            std::sort(p->loc_rows.begin(), p->loc_rows.end(),
                      [](const dai_editor_ui::LocEntry &a, const dai_editor_ui::LocEntry &b) {
                          return a.key < b.key;
                      });
            p->loc_newkey[0] = 0;
            p->loc_dirty = 1;
        }
    }
    bx += 36.0f;
    dai_ui_text(ui, bx, py + 4.0f + 5.0f, "New language", st->text_dim);
    bx += dai_ui_text_width(ui, "New language") + 6.0f;
    dai_ui_text_field(ui, "loclang", bx, py + 4.0f, 60.0f, ROW, p->loc_newlang,
                      sizeof(p->loc_newlang), nullptr);
    bx += 64.0f;
    if (browser_button(p, bx, py + 4.0f, 30.0f, ROW, "+") && p->loc_newlang[0]) {
        bool have = false;
        for (const auto &l : p->loc_langs) if (l == p->loc_newlang) have = true;
        if (!have) {
            p->loc_langs.push_back(p->loc_newlang);
            p->loc_names.push_back(p->loc_newlang);
            for (auto &e : p->loc_rows) e.vals.push_back(std::string());
            p->loc_dirty = 1;
        }
        p->loc_newlang[0] = 0;
    }
    dai_ui_rect(ui, px, py + BAR, pw, 1.0f, st->panel_border);

    if (p->loc_langs.empty()) {
        dai_ui_text(ui, px + 10.0f, py + BAR + 10.0f,
                    "no languages yet - type a code (de, fr, ja) and press +", st->text_dim);
        return;
    }

    // ---- the grid ----------------------------------------------------------
    const float KEYW = 220.0f;
    float colw = (pw - KEYW - 12.0f) / (float)p->loc_langs.size();
    if (colw < 90.0f) colw = 90.0f;

    // header
    float hy = py + BAR + 2.0f;
    dai_ui_rect(ui, px, hy, pw, ROW, st->titlebar);
    dai_ui_text(ui, px + 6.0f, hy + 4.0f, "Key", st->text);
    for (size_t c = 0; c < p->loc_langs.size(); ++c) {
        float cx = px + KEYW + colw * (float)c;
        char lbl[64];
        std::snprintf(lbl, sizeof(lbl), "%s  (%s)", p->loc_names[c].c_str(), p->loc_langs[c].c_str());
        dai_ui_clip_begin(ui, cx, hy, colw - 4.0f, ROW);
        dai_ui_text(ui, cx + 4.0f, hy + 4.0f, lbl, st->text);
        dai_ui_clip_end(ui);
    }

    float listy = hy + ROW + 1.0f, listh = ph - (listy - py);
    float mx = 0, my = 0;
    int pressed = 0;
    dai_ui_mouse(ui, &mx, &my, nullptr, &pressed);
    bool inside = mx >= px && mx < px + pw && my >= listy && my < listy + listh;
    if (inside) p->loc_scroll -= dai_ui_wheel(ui) * 40.0f;
    float total = ROW * (float)p->loc_rows.size();
    float maxs = total - listh + 4.0f;
    if (maxs < 0.0f) maxs = 0.0f;
    if (p->loc_scroll > maxs) p->loc_scroll = maxs;
    if (p->loc_scroll < 0.0f) p->loc_scroll = 0.0f;

    dai_ui_clip_begin(ui, px, listy, pw, listh);
    float ry = listy - p->loc_scroll;
    int delete_row = -1;
    for (size_t i = 0; i < p->loc_rows.size(); ++i) {
        auto &e = p->loc_rows[i];
        if (ry + ROW > listy && ry < listy + listh) {
            if (i & 1) dai_ui_rect(ui, px, ry, pw, ROW, st->track);
            dai_ui_clip_begin(ui, px + 4.0f, ry, KEYW - 30.0f, ROW);
            dai_ui_text(ui, px + 6.0f, ry + 4.0f, e.key.c_str(), st->text);
            dai_ui_clip_end(ui);
            // A key with an empty cell somewhere is the thing this window
            // exists to show, so it says so on the row rather than making you
            // scan the columns.
            bool gap = false;
            for (const std::string &v : e.vals) if (v.empty()) gap = true;
            if (gap) dai_ui_rect(ui, px, ry, 3.0f, ROW, rgba(230, 190, 90, 255));
            float dx2 = px + KEYW - 22.0f;
            if (browser_button(p, dx2, ry + 2.0f, 18.0f, ROW - 4.0f, "x")) delete_row = (int)i;

            for (size_t c = 0; c < p->loc_langs.size() && c < e.vals.size(); ++c) {
                float cx = px + KEYW + colw * (float)c;
                char fid[64];
                std::snprintf(fid, sizeof(fid), "loc%zu_%zu", i, c);
                char buf[256];
                std::snprintf(buf, sizeof(buf), "%s", e.vals[c].c_str());
                if (dai_ui_text_field(ui, fid, cx, ry + 2.0f, colw - 6.0f, ROW - 4.0f,
                                      buf, sizeof(buf), nullptr)) {
                    if (e.vals[c] != buf) { e.vals[c] = buf; p->loc_dirty = 1; }
                }
            }
        }
        ry += ROW;
    }
    dai_ui_clip_end(ui);
    if (delete_row >= 0) {
        p->loc_rows.erase(p->loc_rows.begin() + delete_row);
        p->loc_dirty = 1;
    }
}

// The Script panel: the files you are editing, as tabs, with the code editor"""
assert s.count(old) == 1, 'script panel anchor not found'
s = s.replace(old, new)

# --- als Dock-Tab anmelden -------------------------------------------------
old = """    dai_dock_add_tab(p->dock, "Console", "Project");"""
new = """    dai_dock_add_tab(p->dock, "Console", "Project");
    dai_dock_add_tab(p->dock, "Localisation", "Project");"""
assert s.count(old) == 2, 'console tab registration count unexpected'
s = s.replace(old, new)

old = """                       "Project", "Console", "Audio", "Settings" };"""
new = """                       "Project", "Console", "Localisation", "Audio", "Settings" };"""
assert s.count(old) == 1, 'panel name list not found'
s = s.replace(old, new)

# das Panel zeichnen, direkt vor der Console
old = """    for (int inst = 0; inst < DAI_MAX_PANEL_INSTANCES &&
                       dai_dock_panel(p->dock, "Console", &px, &py, &pw, &ph); ++inst) {"""
new = """    for (int inst = 0; inst < DAI_MAX_PANEL_INSTANCES &&
                       dai_dock_panel(p->dock, "Localisation", &px, &py, &pw, &ph); ++inst) {
        dai_ui_panel_begin(ui, px, py, pw, ph, nullptr);
        loc_body(p, px, py, pw, ph);
        play_dim(p, px, py, pw, ph);
        dai_ui_panel_end(ui);
        dai_dock_panel_end(p->dock);
    }
    for (int inst = 0; inst < DAI_MAX_PANEL_INSTANCES &&
                       dai_dock_panel(p->dock, "Console", &px, &py, &pw, &ph); ++inst) {"""
assert s.count(old) == 1, 'console panel loop not found'
s = s.replace(old, new)

io.open(p, 'w', encoding='utf-8').write(s)
print('Localisation window')
