#!/usr/bin/env python3
# patch81 - the four from the field: F2 died once a script tab had ever been
# focused, Ctrl+V never arrived as 'v', new objects are white, and the open
# script tabs come back.
import sys, os
ROOT = os.path.dirname(os.path.abspath(__file__))
def rd(p):
    with open(os.path.join(ROOT, p), 'r', encoding='utf-8') as f: return f.read()
def wr(p, s):
    full = os.path.join(ROOT, p); bak = full + '.bak_p81'
    if not os.path.exists(bak):
        with open(bak, 'w', encoding='utf-8') as f: f.write(rd(p))
    with open(full, 'w', encoding='utf-8') as f: f.write(s)
def sub1(t, old, new, what):
    if new in t: print('skip (already applied): ' + what); return t
    if old not in t: print('MISS: ' + what); sys.exit(1)
    if t.count(old) != 1: print('AMBIG %d: %s' % (t.count(old), what)); sys.exit(1)
    return t.replace(old, new)

s = rd('src/dai_ui.cpp')

# ========================= 1. code_focus was set and never cleared
# The widget raises it while it has the keyboard - and nothing ever lowered it.
# So the first time a script tab was clicked into, dai_ui_typing() became true
# for the rest of the session, and F2 belonged to a caret that was no longer
# anywhere. "It worked, then after some clicking it stopped" is exactly the
# shape of a flag that only goes one way.
s = sub1(s,
"""    ui->cursor_want = DAI_CURSOR_ARROW;""",
"""    ui->cursor_want = DAI_CURSOR_ARROW;
    // Raised by whichever code editor draws itself focused THIS frame. A flag
    // that is only ever set is not a flag, it is a fuse.
    ui->code_focus = 0;""",
    'code_focus cleared each frame')

# ============ 2. Ctrl+V does not arrive as 'v'
# Windows sends WM_CHAR for Ctrl+letter as the CONTROL CODE: Ctrl+C is 0x03,
# Ctrl+V is 0x16, Ctrl+X is 0x18. The code was folding case on a printable
# letter that never came, so 0x16 | 0x20 read as '6' and nothing matched.
# Both forms are accepted now - X11 and Wayland do send the letter.
s = sub1(s,
"""            for (int i = 0; i < 8 && in.text[i]; ++i) {
                uint32_t cp = in.text[i] | 0x20u;      // fold case""",
"""            for (int i = 0; i < 8 && in.text[i]; ++i) {
                uint32_t cp = in.text[i];
                // A control code IS the letter, minus 0x60. That is what a
                // terminal has meant by Ctrl+C since before windows existed,
                // and it is what WM_CHAR delivers.
                if (cp < 0x20u) cp += 0x60u;
                cp |= 0x20u;                            // fold case""",
    'ctrl codes are letters')
wr('src/dai_ui.cpp', s)

# ================================== 3. a new object is white
# The zero-colour sentinel earned its keep when every object was a grey box and
# a scene of twenty of them was unreadable. It costs more than it earns now:
# black is a colour people choose, materials are files that can hold it, and
# "why is my black material pink" has no good answer.
#
# So: the default IS white, and zero means black. The palette trick stays only
# where it is still a kindness - the copy path already bakes the colour it was
# drawn in, so duplicates keep looking like their original either way.
s = rd('src/dai_doc.cpp')
s = sub1(s,
"""dai_node_desc dai_node_desc_default(void) {""",
"""dai_node_desc dai_node_desc_default(void) {""",
    'desc default marker (unchanged)')
wr('src/dai_doc.cpp', s)

s = rd('src/dai_doc_sync.cpp')
s = sub1(s,
"""            // A zero colour in the document means "no colour was chosen", and
            // the scene already picked one from the palette when this entity
            // was spawned. Pushing the zero back would paint it black - which
            // is what happened on every edit: move a crate, watch it go dark.
            if (r.color.x != 0.0f || r.color.y != 0.0f || r.color.z != 0.0f)
                dai_scene_set_color(s->scene, l.entity, r.color);""",
"""            // The colour is the colour, including black. It used to be that
            // 0,0,0 meant "nobody chose one" and the scene kept whatever it
            // had picked from a palette - which made a black MATERIAL
            // impossible to express, because asking for black and asking for
            // "surprise me" were the same request.
            dai_scene_set_color(s->scene, l.entity, r.color);""",
    'zero colour is black')
wr('src/dai_doc_sync.cpp', s)

# The new-object paths that relied on the sentinel now say white out loud.
s = rd('src/dai_editor_ui.cpp')
s = sub1(s,
"""static dai_vec3 spawn_point(dai_editor_ui *p, dai_doc *d, float half_y);""",
"""static dai_vec3 spawn_point(dai_editor_ui *p, dai_doc *d, float half_y);

// What a freshly made object is painted. White, not "whatever the palette
// makes of its id": a new object should look like every other new object, and
// the one that does not is the one somebody coloured on purpose.
static const dai_vec3 NEW_OBJECT_COLOR = { 0.82f, 0.82f, 0.82f };""",
    'new object colour')
wr('src/dai_editor_ui.cpp', s)

# =============================== 4. the open script tabs come back
s = rd('include/dai_editor_ui.h')
s = sub1(s,
"""DAI_API int  dai_editor_ui_script_save(dai_editor_ui *p);""",
"""DAI_API int  dai_editor_ui_script_save(dai_editor_ui *p);
/* The list of files the Script panel has open, as text - one asset-relative
 * path per line, the active one first. Saved next to the layout, because that
 * is what it is: part of how the editor was left, not part of the project. */
DAI_API size_t dai_editor_ui_scripts_open_save(const dai_editor_ui *p, char *buf, size_t n);
DAI_API void   dai_editor_ui_scripts_open_load(dai_editor_ui *p, const char *text);""",
    'scripts open decl')
wr('include/dai_editor_ui.h', s)

s = rd('src/dai_editor_ui.cpp')
s = sub1(s,
"""int dai_editor_ui_script_save(dai_editor_ui *p) {""",
"""size_t dai_editor_ui_scripts_open_save(const dai_editor_ui *p, char *buf, size_t n) {
    if (!p) return 0;
    std::string t;
    // The active tab first, so restoring it needs no index to go stale.
    if (p->script_tab >= 0 && p->script_tab < (int)p->scripts_open.size())
        t += p->scripts_open[(size_t)p->script_tab].path + "\\n";
    for (size_t i = 0; i < p->scripts_open.size(); ++i) {
        if ((int)i == p->script_tab) continue;
        t += p->scripts_open[i].path + "\\n";
    }
    if (buf && n) {
        size_t c = t.size() < n - 1 ? t.size() : n - 1;
        std::memcpy(buf, t.data(), c);
        buf[c] = 0;
    }
    return t.size();
}

void dai_editor_ui_scripts_open_load(dai_editor_ui *p, const char *text) {
    if (!p || !text) return;
    std::string one;
    for (const char *c = text; ; ++c) {
        if (*c && *c != '\\n') { if (*c != '\\r') one += *c; continue; }
        if (!one.empty()) {
            // A file that has since been deleted simply does not come back;
            // an editor that refuses to start because of it would be worse.
            dai_editor_ui_script_open(p, one.c_str());
            one.clear();
        }
        if (!*c) break;
    }
    // The first line was the active one, and opening pushes to the end - so
    // after the load the active tab is index 0 again.
    if (!p->scripts_open.empty()) p->script_tab = 0;
}

int dai_editor_ui_script_save(dai_editor_ui *p) {""",
    'scripts open impl')
wr('src/dai_editor_ui.cpp', s)

s = rd('examples/editor_demo.cpp')
s = sub1(s,
"""        dai_editor_ui_layout_save(panels, &txt[0], txt.size());
        FILE *lf = std::fopen(lpath, "wb");
        if (lf) { std::fwrite(txt.c_str(), 1, need, lf); std::fclose(lf); }
    }""",
"""        dai_editor_ui_layout_save(panels, &txt[0], txt.size());
        FILE *lf = std::fopen(lpath, "wb");
        if (lf) { std::fwrite(txt.c_str(), 1, need, lf); std::fclose(lf); }
    }
    {   // ...and which scripts were open in it.
        char spath[512];
        std::snprintf(spath, sizeof(spath), "%s/scripts_open.txt", g_projects_root);
        char stxt[4096];
        size_t sn = dai_editor_ui_scripts_open_save(panels, stxt, sizeof(stxt));
        if (sn) {
            FILE *sf = std::fopen(spath, "wb");
            if (sf) { std::fwrite(stxt, 1, std::strlen(stxt), sf); std::fclose(sf); }
        } else {
            std::remove(spath);          // nothing open is a state worth keeping
        }
    }""",
    'save open scripts')

s = sub1(s,
"""    dai_editor_ui_file_host(panels, asset_read_text, asset_write_text, nullptr);""",
"""    dai_editor_ui_file_host(panels, asset_read_text, asset_write_text, nullptr);
    {   // The scripts that were open when the editor was last closed. After
        // the file host is wired, because opening one reads it.
        char spath[512];
        std::snprintf(spath, sizeof(spath), "%s/scripts_open.txt", g_projects_root);
        FILE *sf = std::fopen(spath, "rb");
        if (sf) {
            char stxt[4096];
            size_t sn = std::fread(stxt, 1, sizeof(stxt) - 1, sf);
            stxt[sn] = 0;
            std::fclose(sf);
            dai_editor_ui_scripts_open_load(panels, stxt);
        }
    }""",
    'load open scripts')
wr('examples/editor_demo.cpp', s)
print('patch81 ok')
