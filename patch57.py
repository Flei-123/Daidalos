#!/usr/bin/env python3
# patch57 - the Script panel: files open in a tab inside the editor, Ctrl+S
# saves, and a preference decides whether a double click opens here or outside.
import sys, os
ROOT = os.path.dirname(os.path.abspath(__file__))
def rd(p):
    with open(os.path.join(ROOT, p), 'r', encoding='utf-8') as f: return f.read()
def wr(p, s):
    full = os.path.join(ROOT, p); bak = full + '.bak_p57'
    if not os.path.exists(bak):
        with open(bak, 'w', encoding='utf-8') as f: f.write(rd(p))
    with open(full, 'w', encoding='utf-8') as f: f.write(s)
def sub1(t, old, new, what):
    if new in t: print('skip (already applied): ' + what); return t
    if old not in t: print('MISS: ' + what); sys.exit(1)
    if t.count(old) != 1: print('AMBIG %d: %s' % (t.count(old), what)); sys.exit(1)
    return t.replace(old, new)

# =================================================================== header
s = rd('include/dai_editor_ui.h')
s = sub1(s,
"""DAI_API int  dai_editor_ui_delete_project_pick(dai_editor_ui *p);""",
"""DAI_API int  dai_editor_ui_delete_project_pick(dai_editor_ui *p);

/* ---- the built-in script editor -----------------------------------------
 *
 * Reading and writing a text asset. The editor owns the tabs, the caret and
 * the undo; the host owns the disk, as it does for every other file operation
 * in this header.
 *   read  - fills `out` with at most `max` bytes, returns how many (0 = could
 *           not read). Must NUL terminate.
 *   write - the whole file, NUL terminated text. 1 on success. */
typedef uint32_t (*dai_editor_ui_read_fn)(const char *rel, char *out, uint32_t max, void *user);
typedef int      (*dai_editor_ui_write_fn)(const char *rel, const char *text, void *user);
DAI_API void dai_editor_ui_file_host(dai_editor_ui *p, dai_editor_ui_read_fn rd,
                                     dai_editor_ui_write_fn wr, void *user);

/* Opens an asset in the Script panel (and shows the panel). 1 when it did. */
DAI_API int  dai_editor_ui_script_open(dai_editor_ui *p, const char *rel_path);
/* Saves whatever the Script panel is showing. What Ctrl+S means while the
 * script editor has the keyboard - the host asks this FIRST and only saves the
 * scene when the answer is 0. */
DAI_API int  dai_editor_ui_script_save(dai_editor_ui *p);
/* 0 = open scripts in the built-in editor, 1 = hand them to the external one.
 * The host stores it with the other preferences. */
DAI_API void dai_editor_ui_script_editor_pref(dai_editor_ui *p, int external);
DAI_API int  dai_editor_ui_script_editor_pref_get(const dai_editor_ui *p);""",
    'script panel api decl')
wr('include/dai_editor_ui.h', s)

# ==================================================================== state
s = rd('src/dai_editor_ui.cpp')
s = sub1(s,
"""    std::string delete_ask;            // what Delete is about to remove
    dai_ui_popup menu_delete{};""",
"""    std::string delete_ask;            // what Delete is about to remove
    dai_ui_popup menu_delete{};

    // ---- the built-in script editor ------------------------------------
    // One entry per open file. The buffer is the file plus room to type in:
    // a code editor that stops accepting characters at the file's original
    // length is a code editor you use once.
    struct OpenScript {
        std::string       path;
        std::vector<char> buf;
        dai_ui_code_state st{};
        int               dirty = 0;
    };
    std::vector<OpenScript> scripts_open;
    int  script_tab = 0;
    int  script_external = 0;          // 0 = edit here, 1 = hand to VS Code
    dai_editor_ui_read_fn  file_read = nullptr;
    dai_editor_ui_write_fn file_write = nullptr;
    void *file_user = nullptr;""",
    'script editor state')

# =================================================================== the body
s = sub1(s,
"""// The audio mixer: the four busses every game grows anyway.""",
"""// The Script panel: the files you are editing, as tabs, with the code editor
// underneath and one line of status at the bottom.
//
// Why build one at all when VS Code exists: because the round trip matters.
// Changing a number in a behaviour and pressing Play should be two keys, not
// alt-tab, edit, save, alt-tab, play - and an editor that cannot show you the
// script you just attached is an editor you are always leaving. This one is
// deliberately small: no completion, no language server, no extensions. When
// you want those, the preference sends the file to the real thing and the
// project already carries the .d.ts that makes it understand the engine.
static void script_body(dai_editor_ui *p, float px, float py, float pw, float ph) {
    dai_ui *ui = p->ui;
    const dai_ui_style *st = dai_ui_style_of(ui);
    const float BAR = dai_ui_text_height(ui) + 12.0f;

    if (p->scripts_open.empty()) {
        dai_ui_text(ui, px + 10.0f, py + 10.0f,
                    "no file open - double click a script in the Project window",
                    st->text_dim);
        return;
    }
    if (p->script_tab >= (int)p->scripts_open.size()) p->script_tab = 0;
    if (p->script_tab < 0) p->script_tab = 0;

    // ---- the tab strip -----------------------------------------------------
    float tx = px + 2.0f;
    int close_at = -1;
    float mx = 0, my = 0;
    int pressed = 0;
    dai_ui_mouse(ui, &mx, &my, nullptr, &pressed);
    for (size_t i = 0; i < p->scripts_open.size(); ++i) {
        std::string lbl = base_of(p->scripts_open[i].path);
        if (p->scripts_open[i].dirty) lbl += " *";
        float tw = dai_ui_text_width(ui, lbl.c_str()) + 34.0f;
        if (tx + tw > px + pw - 4.0f) break;
        bool on = (int)i == p->script_tab;
        bool over = mx >= tx && mx < tx + tw && my >= py + 2.0f && my < py + BAR - 2.0f;
        dai_ui_rrect(ui, tx, py + 2.0f, tw, BAR - 4.0f, 4.0f,
                     on ? st->button : (over ? st->button_hover : st->titlebar));
        if (on) dai_ui_rect(ui, tx + 3.0f, py + BAR - 5.0f, tw - 6.0f, 2.0f, st->accent);
        dai_ui_text(ui, tx + 9.0f, py + 2.0f + (BAR - 4.0f - dai_ui_text_height(ui)) * 0.5f,
                    lbl.c_str(), on ? st->text : st->text_dim);
        // the close cross
        float cx = tx + tw - 15.0f, cy = py + BAR * 0.5f;
        bool over_x = mx >= cx - 6.0f && mx < cx + 6.0f && my >= cy - 6.0f && my < cy + 6.0f;
        uint32_t xc = over_x ? st->text : st->text_dim;
        dai_ui_line(ui, cx - 3.5f, cy - 3.5f, cx + 3.5f, cy + 3.5f, 1.4f, xc);
        dai_ui_line(ui, cx + 3.5f, cy - 3.5f, cx - 3.5f, cy + 3.5f, 1.4f, xc);
        if (pressed && over && !dai_ui_popup_active(ui)) {
            if (over_x) close_at = (int)i;
            else        p->script_tab = (int)i;
        }
        tx += tw + 3.0f;
    }
    dai_ui_rect(ui, px, py + BAR, pw, 1.0f, st->panel_border);

    // ---- the editor --------------------------------------------------------
    auto &cur = p->scripts_open[(size_t)p->script_tab];
    const float STATUS = dai_ui_text_height(ui) + 10.0f;
    float ey = py + BAR + 1.0f;
    float eh = ph - BAR - 1.0f - STATUS;
    if (eh > 20.0f) {
        int lang = DAI_CODE_LANG_JS;
        if (is_cpp_file(cur.path)) lang = DAI_CODE_LANG_CPP;
        char cid[64];
        std::snprintf(cid, sizeof(cid), "code%d", p->script_tab);
        if (dai_ui_code_edit(ui, cid, px + 2.0f, ey, pw - 4.0f, eh,
                             cur.buf.data(), cur.buf.size(), &cur.st, lang))
            cur.dirty = 1;
    }

    // ---- the status line ---------------------------------------------------
    float sy = py + ph - STATUS;
    dai_ui_rect(ui, px, sy, pw, 1.0f, st->panel_border);
    int line = 1, col = 1;
    dai_ui_code_caret_pos(cur.buf.data(), cur.st.caret, &line, &col);
    char info[256];
    std::snprintf(info, sizeof(info), "%s   Ln %d, Col %d%s",
                  cur.path.c_str(), line, col, cur.dirty ? "   (unsaved)" : "");
    dai_ui_text(ui, px + 8.0f, sy + 5.0f, info, st->text_dim);
    float bw = dai_ui_text_width(ui, "Save") + 18.0f;
    float ow = dai_ui_text_width(ui, "Open externally") + 18.0f;
    if (browser_button(p, px + pw - bw - 6.0f, sy + 3.0f, bw, STATUS - 6.0f, "Save"))
        dai_editor_ui_script_save(p);
    if (browser_button(p, px + pw - bw - ow - 12.0f, sy + 3.0f, ow, STATUS - 6.0f,
                       "Open externally") && p->open_asset)
        p->open_asset(nullptr, cur.path.c_str(), p->open_asset_user);

    if (close_at >= 0) {
        // A tab with unsaved work says so rather than dropping it.
        if (p->scripts_open[(size_t)close_at].dirty) {
            dai_editor_ui_toast(p, "unsaved - press Save first, then close", 2.5f);
        } else {
            p->scripts_open.erase(p->scripts_open.begin() + close_at);
            if (p->script_tab >= (int)p->scripts_open.size())
                p->script_tab = (int)p->scripts_open.size() - 1;
            if (p->script_tab < 0) p->script_tab = 0;
        }
    }
}

// The audio mixer: the four busses every game grows anyway.""",
    'script_body')

# ============================================================ the panel slot
s = sub1(s,
"""    for (int inst = 0; inst < DAI_MAX_PANEL_INSTANCES &&
                       dai_dock_panel(p->dock, "Audio", &px, &py, &pw, &ph); ++inst) {
        dai_ui_panel_begin(ui, px, py, pw, ph, nullptr);
        audio_body(p, px, py, pw, ph);""",
"""    for (int inst = 0; inst < DAI_MAX_PANEL_INSTANCES &&
                       dai_dock_panel(p->dock, "Script", &px, &py, &pw, &ph); ++inst) {
        dai_ui_panel_begin(ui, px, py, pw, ph, nullptr);
        script_body(p, px, py, pw, ph);
        dai_ui_panel_end(ui);
        dai_dock_panel_end(p->dock);
    }
    for (int inst = 0; inst < DAI_MAX_PANEL_INSTANCES &&
                       dai_dock_panel(p->dock, "Audio", &px, &py, &pw, &ph); ++inst) {
        dai_ui_panel_begin(ui, px, py, pw, ph, nullptr);
        audio_body(p, px, py, pw, ph);""",
    'script panel slot')

s = sub1(s,
"""    dai_dock_add_tab(p->dock, "Console", "Project");""",
"""    dai_dock_add_tab(p->dock, "Console", "Project");
    // The script editor starts beside the scene, where a code window belongs -
    // as a TAB of it, so it costs no space until something is opened in it.
    dai_dock_add_tab(p->dock, "Script", "Scene");""",
    'register script panel')

# ================================================================ the API
s = sub1(s,
"""void dai_editor_ui_delete_host(dai_editor_ui *p, dai_editor_ui_rename_fn fn, void *user) {""",
"""void dai_editor_ui_file_host(dai_editor_ui *p, dai_editor_ui_read_fn r,
                             dai_editor_ui_write_fn w, void *user) {
    if (!p) return;
    p->file_read = r; p->file_write = w; p->file_user = user;
}

void dai_editor_ui_script_editor_pref(dai_editor_ui *p, int external) {
    if (p) p->script_external = external ? 1 : 0;
}
int dai_editor_ui_script_editor_pref_get(const dai_editor_ui *p) {
    return p ? p->script_external : 0;
}

int dai_editor_ui_script_open(dai_editor_ui *p, const char *rel_path) {
    if (!p || !rel_path || !*rel_path) return 0;
    // Already open? Then this is "show me that one", which is what clicking a
    // file a second time means in every editor there is.
    for (size_t i = 0; i < p->scripts_open.size(); ++i)
        if (p->scripts_open[i].path == rel_path) {
            p->script_tab = (int)i;
            dai_editor_ui_panel_open(p, "Script");
            dai_dock_focus(p->dock, "Script");
            return 1;
        }
    if (!p->file_read) {
        dai_editor_ui_toast(p, "this build cannot read files", 2.0f);
        return 0;
    }
    dai_editor_ui::OpenScript o;
    o.path = rel_path;
    // Room to type. A file opened at exactly its own size is read-only in
    // practice, and nothing says so.
    o.buf.assign(96 * 1024, 0);
    uint32_t got = p->file_read(rel_path, o.buf.data(), (uint32_t)o.buf.size() - 1,
                                p->file_user);
    if (!got && o.buf[0] == 0) {
        // An empty file is fine; an unreadable one is not, and the difference
        // is whether the host said anything.
        o.buf[0] = 0;
    }
    p->scripts_open.push_back(std::move(o));
    p->script_tab = (int)p->scripts_open.size() - 1;
    p->scripts_open.back().st.want_focus = 1;
    dai_editor_ui_panel_open(p, "Script");
    dai_dock_focus(p->dock, "Script");
    return 1;
}

int dai_editor_ui_script_save(dai_editor_ui *p) {
    if (!p || p->scripts_open.empty()) return 0;
    if (p->script_tab < 0 || p->script_tab >= (int)p->scripts_open.size()) return 0;
    auto &cur = p->scripts_open[(size_t)p->script_tab];
    if (!p->file_write) {
        dai_editor_ui_toast(p, "this build cannot write files", 2.0f);
        return 0;
    }
    if (p->file_write(cur.path.c_str(), cur.buf.data(), p->file_user)) {
        cur.dirty = 0;
        char msg[192];
        std::snprintf(msg, sizeof(msg), "saved %s", base_of(cur.path).c_str());
        dai_editor_ui_toast(p, msg, 1.5f);
        p->want_refresh = 1;
        return 1;
    }
    dai_editor_ui_toast(p, "could not write that file", 2.5f);
    return 0;
}

void dai_editor_ui_delete_host(dai_editor_ui *p, dai_editor_ui_rename_fn fn, void *user) {""",
    'script panel api impl')

# ======================================= double click routes by the preference
s = sub1(s,
"""                        if (dai_ui_double_click(ui)) {
                            if (is_behaviour_file(full) || is_text_file(full)) {
                                if (p->open_asset)
                                    p->open_asset(nullptr, full.c_str(), p->open_asset_user);
                            } else {""",
"""                        if (dai_ui_double_click(ui)) {
                            if (is_behaviour_file(full) || is_text_file(full)) {
                                // Here or out there, whichever Settings says.
                                if (!p->script_external && p->file_read)
                                    dai_editor_ui_script_open(p, full.c_str());
                                else if (p->open_asset)
                                    p->open_asset(nullptr, full.c_str(), p->open_asset_user);
                            } else {""",
    'double click routing')
wr('src/dai_editor_ui.cpp', s)
print('patch57 ok')
