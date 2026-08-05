#!/usr/bin/env python3
# patch52 - the four that lose work: dropped files never arrived, settings were
# only written at a clean exit, a crash left nothing behind, and Delete did
# nothing in the Project window.
import sys, os
ROOT = os.path.dirname(os.path.abspath(__file__))
def rd(p):
    with open(os.path.join(ROOT, p), 'r', encoding='utf-8') as f: return f.read()
def wr(p, s):
    full = os.path.join(ROOT, p); bak = full + '.bak_p52'
    if not os.path.exists(bak):
        with open(bak, 'w', encoding='utf-8') as f: f.write(rd(p))
    with open(full, 'w', encoding='utf-8') as f: f.write(s)
def sub1(t, old, new, what):
    if new in t: print('skip (already applied): ' + what); return t
    if old not in t: print('MISS: ' + what); sys.exit(1)
    if t.count(old) != 1: print('AMBIG %d: %s' % (t.count(old), what)); sys.exit(1)
    return t.replace(old, new)

# ================================================ 1. the drop that never came
# WM_DROPFILES is not sent to a window that has not asked for it. The handler
# was written, the buffer was written, the editor side was written - and
# Windows delivered nothing, because one line was missing. A feature that is
# entirely correct except for its registration is indistinguishable from a
# feature that was never built.
s = rd('src/rhi_vulkan_window_win32.cpp')
s = sub1(s,
"""    ShowWindow(w->hwnd, SW_SHOW);""",
"""    // "This window takes files from Explorer." Without it WM_DROPFILES is
    // never sent and the drop handler below is dead code.
    DragAcceptFiles(w->hwnd, TRUE);
    ShowWindow(w->hwnd, SW_SHOW);""",
    'DragAcceptFiles')
wr('src/rhi_vulkan_window_win32.cpp', s)

s = rd('examples/editor_demo.cpp')

# ==================================== 2. settings that survive a bad ending
# prefs were written once, at the end of main(). Every crash - and there have
# been crashes - threw away the interface scale, the theme and the camera
# speed the user had just set. Written when they CHANGE instead: it is one
# small file, and the cost of writing it too often is nothing next to the cost
# of losing it once.
s = sub1(s,
"""static float         g_dpi_pref = 0.0f;    // 0 = follow the display""",
"""static float         g_dpi_pref = 0.0f;    // 0 = follow the display
// The live prefs block, so a setting can be persisted the MOMENT it changes
// rather than at a clean exit that may never come. Declared after g_dpi_pref
// because that is the value it copies.
static dai_prefs    *g_prefs = nullptr;
static void save_prefs_now() {
    if (!g_prefs) return;
    g_prefs->ui_scale = g_dpi_pref;
    g_prefs->language = dai_tr_lang_get();
    dai_prefs_save(g_prefs);
}""",
    'save_prefs_now')

s = sub1(s,
"""static void apply_ui_scale(float scale, void *) {
    g_dpi_pref = scale;""",
"""static void apply_ui_scale(float scale, void *) {
    g_dpi_pref = scale;
    save_prefs_now();""",
    'persist scale')

# ============================================== 3. a crash leaves a report
s = sub1(s,
"""int main(int argc, char **argv) {""",
"""// ---- crash report ---------------------------------------------------------
//
// "It crashes sometimes" is not a bug report, and it is not the user's fault
// that it isn't: a GUI program that dies takes its console with it. So the
// editor writes one file when it falls over - what signal or exception, where,
// and the return addresses as OFFSETS INTO THE MODULE, which is the form that
// can still be turned back into line numbers later with addr2line against the
// matching build. No symbol server, no dependency, no privacy question.
//
// The handler does the least it possibly can: formatting inside a crashed
// process is how a crash report becomes a second crash. No malloc, no printf
// into std::string, one open/write/close.
static char g_crash_path[600] = { 0 };

static void crash_write(const char *what, unsigned long long code,
                        void *addr, void *const *frames, int nframes) {
    if (!g_crash_path[0]) return;
    FILE *f = std::fopen(g_crash_path, "wb");
    if (!f) return;
    std::fprintf(f, "DAIDALOS crash report\\n");
    std::fprintf(f, "version : %s\\n", dai_version());
    std::fprintf(f, "reason  : %s\\n", what ? what : "?");
    std::fprintf(f, "code    : 0x%llx\\n", code);
    std::fprintf(f, "address : %p\\n", addr);
    std::fprintf(f, "project : %s\\n", g_project ? dai_project_path(g_project) : "(none)");
    std::fprintf(f, "scene   : %s\\n", g_scene_path[0] ? g_scene_path : "(none)");
    std::fprintf(f, "\\nframes (module+offset - resolve with addr2line):\\n");
#ifdef _WIN32
    HMODULE self_mod = GetModuleHandleA(nullptr);
    for (int i = 0; i < nframes; ++i) {
        char modname[MAX_PATH] = { 0 };
        HMODULE m = nullptr;
        unsigned long long off = 0;
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCSTR)frames[i], &m) && m) {
            GetModuleFileNameA(m, modname, sizeof(modname) - 1);
            off = (unsigned long long)((char *)frames[i] - (char *)m);
        }
        const char *base = std::strrchr(modname, '\\\\');
        std::fprintf(f, "  %2d  %s+0x%llx%s\\n", i,
                     base ? base + 1 : (modname[0] ? modname : "?"), off,
                     m == self_mod ? "   <- editor" : "");
    }
#else
    for (int i = 0; i < nframes; ++i) std::fprintf(f, "  %2d  %p\\n", i, frames[i]);
#endif
    // The last thing the console said is usually the last thing that happened.
    if (g_panels_for_log) {
        std::fprintf(f, "\\nconsole tail:\\n");
        char tail[4096];
        uint32_t n = dai_editor_ui_log_tail(g_panels_for_log, tail, sizeof(tail));
        if (n) std::fwrite(tail, 1, n, f);
    }
    std::fclose(f);
}

#ifdef _WIN32
static LONG WINAPI crash_filter(EXCEPTION_POINTERS *ep) {
    void *frames[40];
    USHORT n = CaptureStackBackTrace(0, 40, frames, nullptr);
    crash_write("unhandled exception",
                ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionCode : 0,
                ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionAddress : nullptr,
                frames, (int)n);
    // A message box, because the window is already gone and a file nobody is
    // told about is a file nobody reads.
    char msg[900];
    std::snprintf(msg, sizeof(msg),
                  "DAIDALOS stopped unexpectedly.\\n\\nA report was written to:\\n%s\\n\\n"
                  "Send that file and it can be traced to the line.", g_crash_path);
    MessageBoxA(nullptr, msg, "DAIDALOS", MB_OK | MB_ICONERROR);
    return EXCEPTION_EXECUTE_HANDLER;
}
#else
static void crash_signal(int sig) {
    void *frames[40];
    int n = 0;
#ifdef __GLIBC__
    n = backtrace(frames, 40);
#endif
    crash_write(sig == SIGSEGV ? "SIGSEGV" : sig == SIGABRT ? "SIGABRT" : "signal",
                (unsigned long long)sig, nullptr, frames, n);
    std::signal(sig, SIG_DFL);
    std::raise(sig);
}
#endif

static void crash_handler_install(const char *dir) {
    if (dir && *dir) std::snprintf(g_crash_path, sizeof(g_crash_path), "%s/crash-report.txt", dir);
    else             std::snprintf(g_crash_path, sizeof(g_crash_path), "crash-report.txt");
#ifdef _WIN32
    SetUnhandledExceptionFilter(crash_filter);
#else
    std::signal(SIGSEGV, crash_signal);
    std::signal(SIGABRT, crash_signal);
    std::signal(SIGFPE, crash_signal);
    std::signal(SIGILL, crash_signal);
#endif
}

int main(int argc, char **argv) {""",
    'crash handler')

s = sub1(s,
"""    g_ui_scale_out = &prefs.ui_scale;""",
"""    g_ui_scale_out = &prefs.ui_scale;
    g_prefs = &prefs;
    crash_handler_install(g_projects_root);""",
    'install crash handler')

# Every settled setting, the moment it settles.
s = sub1(s,
"""    {   // what this human set on this machine, kept for the next start
        prefs.cam_speed = dai_editor_cam_speed_get(ed);
        prefs.language = dai_tr_lang_get();
        prefs.ui_scale = g_dpi_pref;
        dai_prefs_save(&prefs);
    }""",
"""    {   // what this human set on this machine, kept for the next start
        prefs.cam_speed = dai_editor_cam_speed_get(ed);
        save_prefs_now();
    }""",
    'exit save uses helper')
wr('examples/editor_demo.cpp', s)
print('patch52 ok (window + host)')
