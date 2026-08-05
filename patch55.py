#!/usr/bin/env python3
# patch55 - the host end of Delete, and the key that reaches it.
import sys, os
ROOT = os.path.dirname(os.path.abspath(__file__))
def rd(p):
    with open(os.path.join(ROOT, p), 'r', encoding='utf-8') as f: return f.read()
def wr(p, s):
    full = os.path.join(ROOT, p); bak = full + '.bak_p55'
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
"""static int asset_import(const char *src_abs, const char *dest_rel, void *) {""",
"""// Deleting an asset - a file, or a folder and everything under it. The editor
// has already asked; this end only has to be careful about WHERE.
static int remove_tree(const std::string &abs, int depth) {
    if (depth > 12) return 0;
    if (!is_dir_path(abs.c_str())) return std::remove(abs.c_str()) == 0 ? 1 : 0;
    DIR *dp = opendir(abs.c_str());
    if (dp) {
        while (dirent *de = readdir(dp)) {
            if (!std::strcmp(de->d_name, ".") || !std::strcmp(de->d_name, "..")) continue;
            remove_tree(abs + "/" + de->d_name, depth + 1);
        }
        closedir(dp);
    }
#ifdef _WIN32
    return RemoveDirectoryA(abs.c_str()) ? 1 : 0;
#else
    return rmdir(abs.c_str()) == 0 ? 1 : 0;
#endif
}

static int asset_delete(const char *rel, const char *, void *) {
    if (!rel || !*rel || !g_assets_dir[0]) return 0;
    // Never above the assets root, never the root itself. A path containing
    // ".." or starting with a separator is not a mistake to be corrected, it
    // is a request to delete something outside the project.
    if (std::strstr(rel, "..") || rel[0] == '/' || rel[0] == '\\\\') return 0;
    std::string abs = std::string(g_assets_dir) + "/" + rel;
    int ok = remove_tree(abs, 0);
    if (g_panels_for_log) {
        char msg[800];
        std::snprintf(msg, sizeof(msg), ok ? "deleted %s" : "delete FAILED: %s", rel);
        dai_editor_ui_log(g_panels_for_log, ok ? 0 : 2, msg);
    }
    return ok;
}

static int asset_import(const char *src_abs, const char *dest_rel, void *) {""",
    'asset_delete')

s = sub1(s,
"""    dai_editor_ui_import_host(panels, asset_import, nullptr);""",
"""    dai_editor_ui_import_host(panels, asset_import, nullptr);
    dai_editor_ui_delete_host(panels, asset_delete, nullptr);""",
    'wire delete host')

s = sub1(s,
"""        if ((pressed(5) || pressed(9)) && !typing) dai_editor_delete_selection(ed);""",
"""        // Delete belongs to whichever window the pointer is over. The Project
        // window takes it for the file that is selected there; otherwise it is
        // the scene's, as it always was.
        if ((pressed(5) || pressed(9)) && !typing) {
            if (!dai_editor_ui_delete_project_pick(panels))
                dai_editor_delete_selection(ed);
        }""",
    'delete key routing')
wr('examples/editor_demo.cpp', s)
print('patch55 ok')
