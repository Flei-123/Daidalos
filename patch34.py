#!/usr/bin/env python3
# patch34 - the host: no console window, output into the editor's own console,
# VS Code launched properly, a UI scale you can override, and a loud warning
# when the physics has no solver.
import sys, os
ROOT = os.path.dirname(os.path.abspath(__file__))
def rd(p):
    with open(os.path.join(ROOT, p), 'r', encoding='utf-8') as f: return f.read()
def wr(p, s):
    full = os.path.join(ROOT, p); bak = full + '.bak_p34'
    if not os.path.exists(bak):
        with open(bak, 'w', encoding='utf-8') as f: f.write(rd(p))
    with open(full, 'w', encoding='utf-8') as f: f.write(s)
def sub1(t, old, new, what):
    if old not in t: print('MISS: ' + what); sys.exit(1)
    if t.count(old) != 1: print('AMBIG %d: %s' % (t.count(old), what)); sys.exit(1)
    return t.replace(old, new)

# ======================================================= the scale host API
s = rd('include/dai_editor_ui.h')
s = sub1(s,
"""DAI_API void dai_editor_ui_settings_host(dai_editor_ui *p,
                                         void (*apply_font)(float px, void *user),
                                         float current_px, void *user);""",
"""DAI_API void dai_editor_ui_settings_host(dai_editor_ui *p,
                                         void (*apply_font)(float px, void *user),
                                         float current_px, void *user);

/* The display scale. Auto (0) asks the window system; anything else is the
 * user overruling it, which a laptop whose EDID lies about its size needs.
 * The host owns it because it owns the font atlas and the icon atlas. */
DAI_API void dai_editor_ui_scale_host(dai_editor_ui *p,
                                      void (*apply_scale)(float scale, void *user),
                                      float current, void *user);""",
'scale host decl')
wr('include/dai_editor_ui.h', s)

s = rd('src/dai_editor_ui.cpp')
s = sub1(s, "    float settings_font_px = 13.0f;",
"""    float settings_font_px = 13.0f;
    void (*apply_scale)(float, void *) = nullptr;
    void *apply_scale_user = nullptr;
    float settings_ui_scale = 0.0f;   // 0 = follow the display""",
'scale host fields')
s = sub1(s, "void dai_editor_ui_project_settings_host(dai_editor_ui *p, void (*draw)(void *user), void *user) {",
"""void dai_editor_ui_scale_host(dai_editor_ui *p,
                              void (*apply_scale)(float scale, void *user),
                              float current, void *user) {
    if (!p) return;
    p->apply_scale = apply_scale;
    p->apply_scale_user = user;
    p->settings_ui_scale = current;
}

void dai_editor_ui_project_settings_host(dai_editor_ui *p, void (*draw)(void *user), void *user) {""",
'scale host impl')
s = sub1(s,
"""    {
        static const char *const LANGS[] = { "English", "Deutsch" };""",
"""    // The display scale. Auto is right almost everywhere; "almost" is why
    // this row exists - a laptop panel that reports the wrong physical size
    // makes the whole interface half or double the size it should be.
    {
        static const float SCALE_V[6] = { 0.0f, 1.0f, 1.25f, 1.5f, 1.75f, 2.0f };
        static const char *const SCALES[] = { "Auto (display)", "100%", "125%",
                                              "150%", "175%", "200%" };
        int si = 0;
        for (int i = 1; i < 6; ++i)
            if (p->settings_ui_scale > SCALE_V[i] - 0.06f &&
                p->settings_ui_scale < SCALE_V[i] + 0.06f) si = i;
        if (dai_ui_option(ui, "UI scale", &si, SCALES, 6)) {
            p->settings_ui_scale = SCALE_V[si];
            if (p->apply_scale) p->apply_scale(SCALE_V[si], p->apply_scale_user);
        }
        char now[64];
        std::snprintf(now, sizeof(now), "now: %.0f%%", dai_ui_scale_get(ui) * 100.0f);
        dai_ui_label(ui, now);
    }

    {
        static const char *const LANGS[] = { "English", "Deutsch" };""",
'scale option row')
wr('src/dai_editor_ui.cpp', s)

# ============================================================ editor_demo
s = rd('examples/editor_demo.cpp')

# ---- 1. no console window; everything the editor prints goes into its own.
s = sub1(s,
"""// The settings window's font swap needs what main() owns, so main() publishes
// it here. One editor, one font - this is not a place that needs generality.""",
"""// ---- the editor's own console --------------------------------------------
//
// The editor used to run with a cmd window behind it, and everything it had
// to say went there - which means it went nowhere, because nobody reads the
// window they were told to ignore. stdout and stderr are redirected into a
// pipe here and drained into the Console panel every frame, so "the shader
// failed" is a line the user can actually see, copy and send.
#ifdef _WIN32
static HANDLE g_log_rd = nullptr, g_log_wr = nullptr;
static std::string g_log_tail;

static void console_capture_begin() {
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    if (!CreatePipe(&g_log_rd, &g_log_wr, &sa, 1 << 16)) return;
    SetStdHandle(STD_OUTPUT_HANDLE, g_log_wr);
    SetStdHandle(STD_ERROR_HANDLE, g_log_wr);
    int fd = _open_osfhandle((intptr_t)g_log_wr, _O_TEXT);
    if (fd >= 0) {
        _dup2(fd, 1);
        _dup2(fd, 2);
        std::setvbuf(stdout, nullptr, _IOLBF, 4096);
        std::setvbuf(stderr, nullptr, _IONBF, 0);
    }
}

// One line is one console entry, and the level is read off the words - an
// editor that colours "failed" red without being told to is doing its job.
static void console_capture_pump(dai_editor_ui *panels) {
    if (!g_log_rd || !panels) return;
    for (;;) {
        DWORD avail = 0;
        if (!PeekNamedPipe(g_log_rd, nullptr, 0, nullptr, &avail, nullptr) || !avail) break;
        char buf[4096];
        DWORD got = 0;
        DWORD want = avail < sizeof(buf) ? avail : (DWORD)sizeof(buf);
        if (!ReadFile(g_log_rd, buf, want, &got, nullptr) || !got) break;
        g_log_tail.append(buf, got);
    }
    for (;;) {
        size_t nl = g_log_tail.find('\\n');
        if (nl == std::string::npos) {
            if (g_log_tail.size() > 8192) g_log_tail.clear();   // a line that never ends
            break;
        }
        std::string line = g_log_tail.substr(0, nl);
        g_log_tail.erase(0, nl + 1);
        while (!line.empty() && (line.back() == '\\r' || line.back() == ' ')) line.pop_back();
        if (line.empty()) continue;
        std::string low = line;
        for (char &c : low) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        int level = 0;
        if (low.find("error") != std::string::npos || low.find("failed") != std::string::npos ||
            low.find("cannot") != std::string::npos || low.find("!!") != std::string::npos)
            level = 2;
        else if (low.find("warn") != std::string::npos || low.find("no ") == 0)
            level = 1;
        dai_editor_ui_log(panels, level, line.c_str());
    }
}
#else
static void console_capture_begin() {}
static void console_capture_pump(dai_editor_ui *) {}
#endif

// The settings window's font swap needs what main() owns, so main() publishes
// it here. One editor, one font - this is not a place that needs generality.""",
'console capture')

# ---- 2. the UI scale, overridable
s = sub1(s,
"""static float         g_dpi = 1.0f;""",
"""static float         g_dpi = 1.0f;
static float         g_dpi_auto = 1.0f;    // what the display says
static float         g_dpi_pref = 0.0f;    // 0 = follow the display
static dai_window   *g_win_for_scale = nullptr;
static dai_icons    *g_icons = nullptr;""",
'scale globals')

s = sub1(s,
"""    dai_font *nf = dai_font_load_ui_scaled(px, g_dpi, err, sizeof(err));""",
"""    dai_font *nf = dai_font_load_ui_scaled(px, g_dpi, err, sizeof(err));""",
'noop')

s = sub1(s,
"""// The renderer's inventory, so the inspector's mesh picker shows names
// instead of a number nobody chose.""",
"""// Changing the display scale rebuilds both atlases: the font and the icons
// are rasterised AT the scale, which is the whole point - a magnified 13 px
// atlas is exactly the blur this avoids.
static void apply_ui_scale(float scale, void *) {
    g_dpi_pref = scale;
    float want = scale > 0.05f ? scale : g_dpi_auto;
    if (!(want > 0.4f) || want > 4.0f) want = 1.0f;
    g_dpi = want;
    if (g_ui) dai_ui_scale_set(g_ui, g_dpi);
    apply_font(g_font ? 13.0f : 13.0f, nullptr);   // reloads at the new scale
    if (g_renderer && g_ui) {
        dai_icons *ni = dai_icons_create(16.0f * g_dpi);
        if (ni) {
            dai_icons_display_size(ni, 16.0f);
            uint32_t iw = 0, ih = 0;
            const uint8_t *irgba = dai_icons_atlas_rgba(ni, &iw, &ih);
            if (irgba && iw && ih) {
                dai_ui_set_icons(g_ui, ni, dai_render_texture_create(g_renderer, irgba, iw, ih, 0));
                if (g_icons) dai_icons_free(g_icons);
                g_icons = ni;
            } else {
                dai_icons_free(ni);
            }
        }
    }
    std::printf("UI scale is now %.0f%% (%s)\\n", g_dpi * 100.0f,
                scale > 0.05f ? "set in Settings" : "from the display");
}

// The renderer's inventory, so the inspector's mesh picker shows names
// instead of a number nobody chose.""",
'apply_ui_scale')

# apply_font has to keep the CURRENT size when the scale changes
s = sub1(s,
"""static void apply_font(float px, void *) {""",
"""static void apply_font(float px, void *);
static float g_font_px = 13.0f;
static void apply_font(float px, void *) {
    g_font_px = px < 2.0f ? 13.0f : px;""",
'apply_font remember')
s = sub1(s, """    apply_font(g_font ? 13.0f : 13.0f, nullptr);   // reloads at the new scale""",
            """    apply_font(g_font_px, nullptr);                // reloads at the new scale""",
         'apply_font reuse size')

s = sub1(s,
"""    g_dpi = dai_window_dpi_scale(win);
    if (!(g_dpi > 0.5f) || g_dpi > 4.0f) g_dpi = 1.0f;
    std::printf("display scale: %.2f\\n", g_dpi);""",
"""    g_dpi_auto = dai_window_dpi_scale(win);
    if (!(g_dpi_auto > 0.5f) || g_dpi_auto > 4.0f) g_dpi_auto = 1.0f;
    g_dpi_pref = prefs.ui_scale;
    g_dpi = g_dpi_pref > 0.05f ? g_dpi_pref : g_dpi_auto;
    g_win_for_scale = win;
    {
        uint32_t rw = 0, rh = 0;
        dai_window_size(win, &rw, &rh);
        std::printf("display scale: %.2f (auto %.2f), window %ux%u real px\\n",
                    g_dpi, g_dpi_auto, rw, rh);
    }""",
'scale from prefs')

s = sub1(s,
"""    dai_icons *icons = dai_icons_create(16.0f * g_dpi);
    if (icons) dai_icons_display_size(icons, 16.0f);""",
"""    dai_icons *icons = dai_icons_create(16.0f * g_dpi);
    if (icons) dai_icons_display_size(icons, 16.0f);
    g_icons = icons;""",
'icons global')

s = sub1(s,
"""    g_psettings = &psettings;""",
"""    dai_editor_ui_scale_host(panels, apply_ui_scale, g_dpi_pref, nullptr);
    g_psettings = &psettings;""",
'scale host wire')

# The scale can change at runtime, so the frame must read it, not a constant.
s = sub1(s,
"""        const float uis = g_dpi;""",
"""        const float uis = g_dpi;   // Settings can change this between frames""",
'uis comment')

# ---- 3. the physics, when there is no solver
s = sub1(s,
"""    std::printf("physics: %s, gravity %.2f %.2f %.2f\\n", dai_backend_name(w),""",
"""    if (std::strcmp(dai_backend_name(w), "null") == 0) {
        // "Nothing collides" is a setting, and a setting nobody can see is a
        // bug report. Say it where the user is already looking.
        std::printf("!! PHYSICS: no solver selected (Settings > Project Settings > "
                    "Physics). Colliders are ignored - the only thing that stops "
                    "a falling object is an invisible floor at y=0.\\n");
    }
    std::printf("physics: %s, gravity %.2f %.2f %.2f\\n", dai_backend_name(w),""",
'null solver warning')

# ---- 4. VS Code without a cmd window
old_open = s[s.index('    const char *CODE_PATHS[] = {'):s.index('#else\n    char cmd[1200];\n    std::snprintf(cmd, sizeof(cmd),\n                  "(command -v code >/dev/null && code --goto')]
new_open = """    // Code.exe, not code.cmd: a .cmd has to go through cmd.exe, and cmd.exe
    // is a black window that flashes up (and, when the path has a space in it
    // and the quoting is one level off, prints "C:\\\\Users\\\\justi\\\\AppData\\\\Local\\\\
    // Programs\\\\Microsoft is not recognised" instead of opening anything).
    const char *CODE_PATHS[] = {
        "%LOCALAPPDATA%\\\\Programs\\\\Microsoft VS Code\\\\Code.exe",
        "%ProgramFiles%\\\\Microsoft VS Code\\\\Code.exe",
        "%ProgramFiles(x86)%\\\\Microsoft VS Code\\\\Code.exe",
        "%LOCALAPPDATA%\\\\Programs\\\\Microsoft VS Code Insiders\\\\Code - Insiders.exe",
        nullptr
    };
    for (int i = 0; CODE_PATHS[i]; ++i) {
        char path[700];
        DWORD n = ExpandEnvironmentStringsA(CODE_PATHS[i], path, sizeof(path));
        if (!n || GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) continue;
        // The project folder AND the file: VS Code with a folder open is an
        // editor, VS Code with one loose file is notepad with colours.
        char cmd2[1600];
        if (g_assets_dir[0])
            std::snprintf(cmd2, sizeof(cmd2), "\\"%s\\" \\"%s\\" --goto \\"%s\\"",
                          path, g_assets_dir, full);
        else
            std::snprintf(cmd2, sizeof(cmd2), "\\"%s\\" --goto \\"%s\\"", path, full);
        STARTUPINFOA si{};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_SHOWNORMAL;
        PROCESS_INFORMATION pi{};
        if (CreateProcessA(nullptr, cmd2, nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                           nullptr, nullptr, &si, &pi)) {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
            std::printf("opened in VS Code: %s\\n", full);
            return 1;
        }
        std::printf("could not start %s (error %lu)\\n", path, (unsigned long)GetLastError());
    }
    // No VS Code anywhere: hand it to whatever the extension is registered
    // to. ShellExecute, not system() - there is no console to borrow.
    HINSTANCE rc = ShellExecuteA(nullptr, "open", full, nullptr, nullptr, SW_SHOWNORMAL);
    if ((INT_PTR)rc > 32) return 1;
    std::printf("no editor could open %s\\n", full);
    return 0;
"""
s = s.replace(old_open, new_open, 1)

# ---- 5. drain the pipe every frame
s = sub1(s,
"""        diag_step("frame begin");""",
"""        console_capture_pump(panels);
        diag_step("frame begin");""",
'pump each frame')
s = sub1(s,
"""    dai_renderer *r = dai_render_create(&rd, err, sizeof(err));""",
"""    console_capture_begin();
    dai_renderer *r = dai_render_create(&rd, err, sizeof(err));""",
'capture begin')

# prefs carry the scale override
s = sub1(s,
"""        prefs.language = dai_tr_lang_get();""",
"""        prefs.language = dai_tr_lang_get();
        prefs.ui_scale = g_dpi_pref;""",
'prefs scale save')

# the includes the capture needs
s = sub1(s, '#include "dai_editor_ui.h"',
            '#include "dai_editor_ui.h"\n#ifdef _WIN32\n#include <fcntl.h>\n#include <io.h>\n#endif',
         'io includes')
wr('examples/editor_demo.cpp', s)

# ------------------------------------------------------- no console window
s = rd('build_win.sh')
s = sub1(s,
"""    RES=""
    [ "$name" = "editor_demo" ] && RES="${ICON_RES:-}\"""",
"""    RES=""
    GUI=""
    # -mwindows: the editor is a GUI program. The cmd window that used to sit
    # behind it was where every diagnostic went to die; stdout is piped into
    # the editor's own Console panel now.
    [ "$name" = "editor_demo" ] && { RES="${ICON_RES:-}"; GUI="-mwindows"; }""",
'gui flag')
s = sub1(s,
"""        "$src" $RES $LIBS -o "$OUT/$name.exe\"""",
"""        "$src" $RES $LIBS $GUI -o "$OUT/$name.exe\"""",
'gui link')
wr('build_win.sh', s)
print('patch34 ok')
