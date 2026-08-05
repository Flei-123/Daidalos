#!/usr/bin/env python3
# patch66 - the scene knows when it is dirty, and closing with unsaved work
# asks first.
import sys, os
ROOT = os.path.dirname(os.path.abspath(__file__))
def rd(p):
    with open(os.path.join(ROOT, p), 'r', encoding='utf-8') as f: return f.read()
def wr(p, s):
    full = os.path.join(ROOT, p); bak = full + '.bak_p66'
    if not os.path.exists(bak):
        with open(bak, 'w', encoding='utf-8') as f: f.write(rd(p))
    with open(full, 'w', encoding='utf-8') as f: f.write(s)
def sub1(t, old, new, what):
    if new in t: print('skip (already applied): ' + what); return t
    if old not in t: print('MISS: ' + what); sys.exit(1)
    if t.count(old) != 1: print('AMBIG %d: %s' % (t.count(old), what)); sys.exit(1)
    return t.replace(old, new)

# ==================================================== 1. take the close back
s = rd('include/dai_render.h')
s = sub1(s,
"""DAI_API int  dai_window_poll(dai_window *w);""",
"""DAI_API int  dai_window_poll(dai_window *w);
/* Undo a close request. The window is not destroyed when the user presses the
 * X - the backend only records that it was asked to go - so a host that has an
 * unsaved document can ask the question and then decide to stay open.
 * Meaningless (and harmless) at any other time. */
DAI_API void dai_window_keep_open(dai_window *w);""",
    'keep_open decl')
wr('include/dai_render.h', s)

for f in ('src/rhi_vulkan_window_win32.cpp', 'src/rhi_vulkan_window.cpp',
          'src/rhi_vulkan_window_wayland.cpp'):
    s = rd(f)
    s = sub1(s,
"""int dai_window_poll(dai_window *w) {""",
"""void dai_window_keep_open(dai_window *w) { if (w) w->open = true; }

int dai_window_poll(dai_window *w) {""",
        'keep_open impl ' + f)
    wr(f, s)

# ============================================= 2. the host tracks the dirt
s = rd('examples/editor_demo.cpp')
s = sub1(s,
"""static char g_prefab_return[512] = { 0 };""",
"""static char g_prefab_return[512] = { 0 };
// The document revision as of the last successful write. Everything above it
// is unsaved work - which is the only definition of "dirty" that cannot drift,
// because it is the same counter the undo system moves.
static uint64_t g_saved_rev = 0;""",
    'saved_rev')

# Every place that writes the scene marks the water line.
s = sub1(s,
"""            const char *sp = scene_path ? scene_path : (g_scene_path[0] ? g_scene_path : nullptr);
            char msg[192];
            if (sp && dai_doc_save(doc, sp) == DAI_OK) {""",
"""            const char *sp = scene_path ? scene_path : (g_scene_path[0] ? g_scene_path : nullptr);
            char msg[192];
            if (sp && dai_doc_save(doc, sp) == DAI_OK) {
                g_saved_rev = dai_doc_revision(doc);""",
    'ctrl-s marks saved')

s = sub1(s,
"""            dai_editor_deselect_all(ed);
            dai_doc_sync_reset(sync);
            dai_doc_sync_apply(sync);""",
"""            dai_editor_deselect_all(ed);
            g_saved_rev = dai_doc_revision(doc);   // freshly loaded IS saved
            dai_doc_sync_reset(sync);
            dai_doc_sync_apply(sync);""",
    'load marks saved')

s = sub1(s,
"""                    if (g_scene_path[0]) dai_doc_save(doc, g_scene_path);
                    if (!g_prefab_return[0])""",
"""                    if (g_scene_path[0] && dai_doc_save(doc, g_scene_path) == DAI_OK)
                        g_saved_rev = dai_doc_revision(doc);
                    if (!g_prefab_return[0])""",
    'prefab enter marks saved')

s = sub1(s,
"""                    if (g_scene_path[0]) dai_doc_save(doc, g_scene_path);
                    if (g_prefab_return[0]) {""",
"""                    if (g_scene_path[0] && dai_doc_save(doc, g_scene_path) == DAI_OK)
                        g_saved_rev = dai_doc_revision(doc);
                    if (g_prefab_return[0]) {""",
    'prefab exit marks saved')

# ...and the hierarchy is told, every frame.
s = sub1(s,
"""        // The interface, under the net.""",
"""        // Unsaved? The asterisk in the hierarchy comes from here.
        dai_editor_ui_scene_dirty(panels, dai_doc_revision(doc) != g_saved_rev);

        // The interface, under the net.""",
    'push dirty flag')

# ======================================= 3. the question, on the way out
s = sub1(s,
"""int main(int argc, char **argv) {""",
"""// Closing with work that is not on disk. Three answers, because there are
// three things a person can mean by pressing the X with unsaved changes, and
// picking one for them is how work disappears.
//
// Returns 1 when it is all right to leave. On the platforms with no native
// dialog the honest thing is to say so on stdout and let the close happen -
// pretending to have asked would be worse than not asking.
static int quit_is_ok(dai_window *win, dai_doc *doc) {
    if (!g_prefs) return 1;
    if (dai_doc_revision(doc) == g_saved_rev) return 1;   // nothing to lose
#ifdef _WIN32
    char msg[700];
    std::snprintf(msg, sizeof(msg),
                  "The scene has unsaved changes.\\n\\n%s\\n\\n"
                  "Yes - save and close\\n"
                  "No  - close and lose them\\n"
                  "Cancel - keep working",
                  g_scene_path[0] ? g_scene_path : "(no file yet)");
    int r = MessageBoxA(nullptr, msg, "DAIDALOS - unsaved changes",
                        MB_YESNOCANCEL | MB_ICONWARNING | MB_DEFBUTTON1);
    if (r == IDCANCEL) {
        dai_window_keep_open(win);
        return 0;
    }
    if (r == IDYES) {
        if (g_scene_path[0] && dai_doc_save(doc, g_scene_path) == DAI_OK) {
            g_saved_rev = dai_doc_revision(doc);
        } else {
            MessageBoxA(nullptr, "Could not write the scene - nothing was closed.",
                        "DAIDALOS", MB_OK | MB_ICONERROR);
            dai_window_keep_open(win);
            return 0;
        }
    }
    return 1;
#else
    (void)win;
    std::printf("closing with unsaved changes (no dialog on this platform)\\n");
    return 1;
#endif
}

int main(int argc, char **argv) {""",
    'quit_is_ok')

# The loop asks on the way out. The || is the whole mechanism: poll returning
# 0 means "the user asked to close", and a cancelled question re-opens the
# window and lets the body run again.
s = sub1(s,
"""    while (dai_window_poll(win)) {""",
"""    while (dai_window_poll(win) || !quit_is_ok(win, doc)) {""",
    'loop asks before quitting')
wr('examples/editor_demo.cpp', s)
print('patch66 ok')
