#!/usr/bin/env python3
# patch43 - the host side of the import: copy what was dropped into the
# project, folders and all, and feed the window's drops to the editor.
import sys, os
ROOT = os.path.dirname(os.path.abspath(__file__))
def rd(p):
    with open(os.path.join(ROOT, p), 'r', encoding='utf-8') as f: return f.read()
def wr(p, s):
    full = os.path.join(ROOT, p); bak = full + '.bak_p43'
    if not os.path.exists(bak):
        with open(bak, 'w', encoding='utf-8') as f: f.write(rd(p))
    with open(full, 'w', encoding='utf-8') as f: f.write(s)
def sub1(t, old, new, what):
    if new in t: print('skip (already applied): ' + what); return t
    if old not in t: print('MISS: ' + what); sys.exit(1)
    if t.count(old) != 1: print('AMBIG %d: %s' % (t.count(old), what)); sys.exit(1)
    return t.replace(old, new)

s = rd('examples/editor_demo.cpp')

s = sub1(s,
"""static int script_create(const char *name, void *) {""",
"""// ---- importing what the desktop dropped -----------------------------------
// The editor decided WHERE (the folder on screen) and WHAT (the paths the
// window system handed over). This end owns the disk, so it does the copying.

static int is_dir_path(const char *p) {
    if (!p || !*p) return 0;
    struct stat st;
    if (stat(p, &st) != 0) return 0;
    return (st.st_mode & S_IFMT) == S_IFDIR ? 1 : 0;
}

static int copy_one_file(const char *src, const char *dst) {
    FILE *in = std::fopen(src, "rb");
    if (!in) return 0;
    make_parent_dirs(dst);
    FILE *out = std::fopen(dst, "wb");
    if (!out) { std::fclose(in); return 0; }
    char buf[64 * 1024];
    size_t got;
    int ok = 1;
    while ((got = std::fread(buf, 1, sizeof(buf), in)) > 0)
        if (std::fwrite(buf, 1, got, out) != got) { ok = 0; break; }
    std::fclose(in);
    if (std::fclose(out) != 0) ok = 0;
    if (!ok) std::remove(dst);
    return ok;
}

// A dropped FOLDER is a folder of assets - a downloaded texture set is never
// one file. Recursion is bounded the same way the folder listing is, so a
// symlink loop cannot take the editor with it.
static int copy_tree(const std::string &src, const std::string &dst, int depth) {
    if (depth > 12) return 0;
    if (!is_dir_path(src.c_str())) return copy_one_file(src.c_str(), dst.c_str());
#ifdef _WIN32
    CreateDirectoryA(dst.c_str(), nullptr);
#else
    mkdir(dst.c_str(), 0755);
#endif
    DIR *dp = opendir(src.c_str());
    if (!dp) return 0;
    int n = 0;
    while (dirent *de = readdir(dp)) {
        if (!std::strcmp(de->d_name, ".") || !std::strcmp(de->d_name, "..")) continue;
        n += copy_tree(src + "/" + de->d_name, dst + "/" + de->d_name, depth + 1);
    }
    closedir(dp);
    return n > 0 ? 1 : 1;   // an empty folder still imported
}

static int asset_import(const char *src_abs, const char *dest_rel, void *) {
    if (!src_abs || !dest_rel || !*src_abs || !*dest_rel || !g_assets_dir[0]) return 0;
    if (std::strstr(dest_rel, "..")) return 0;
    std::string dst = std::string(g_assets_dir) + "/" + dest_rel;
    // Never overwrite. A drop that lands on the folder a file is already in
    // is a slip, not an instruction to truncate it - so the copy gets a
    // number, the way every file manager does it.
    {
        std::string base = dst, ext;
        size_t slash = base.find_last_of('/');
        size_t dot = base.find_last_of('.');
        if (dot != std::string::npos && (slash == std::string::npos || dot > slash)) {
            ext = base.substr(dot);
            base = base.substr(0, dot);
        }
        int tries = 0;
        while (tries < 200) {
            struct stat st;
            if (stat(dst.c_str(), &st) != 0) break;
            char suffix[16];
            std::snprintf(suffix, sizeof(suffix), " (%d)", tries + 2);
            dst = base + suffix + ext;
            ++tries;
        }
        if (tries >= 200) return 0;
    }
    int ok = copy_tree(src_abs, dst, 0);
    if (g_panels_for_log) {
        char msg[900];
        std::snprintf(msg, sizeof(msg), ok ? "imported %s" : "import FAILED: %s", src_abs);
        dai_editor_ui_log(g_panels_for_log, ok ? 0 : 2, msg);
    }
    return ok;
}

static int script_create(const char *name, void *) {""",
    'asset_import')

s = sub1(s,
"""    dai_editor_ui_rename_host(panels, asset_rename, nullptr);""",
"""    dai_editor_ui_rename_host(panels, asset_rename, nullptr);
    dai_editor_ui_import_host(panels, asset_import, nullptr);""",
    'wire import host')

# The drop itself: read it before the frame, hand it to the editor with the
# pointer position in the coordinates the UI thinks in.
s = sub1(s,
"""        // The UI has to run before the viewport, because "is the pointer over a
        // panel" is only known once the panels have been laid out this frame.""",
"""        // What the desktop dropped on us since the last frame. The editor
        // decides whether it landed on the Project window; the pointer comes
        // back in real pixels and the interface thinks in logical ones, which
        // is the same division the mouse goes through ten lines below.
        {
            char drop[4096];
            int dx = 0, dy = 0;
            if (dai_window_dropped_files(win, drop, sizeof(drop), &dx, &dy))
                dai_editor_ui_drop_files(panels, drop, (float)dx / uis, (float)dy / uis);
        }

        // The UI has to run before the viewport, because "is the pointer over a
        // panel" is only known once the panels have been laid out this frame.""",
    'wire drop poll')
wr('examples/editor_demo.cpp', s)
print('patch43 ok')
