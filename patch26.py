# -*- coding: utf-8 -*-
import io, sys
P='examples/editor_demo.cpp'; s=io.open(P,encoding='utf-8').read()

old = '''static int prefab_save_cb(const char *node_id, const char *rel_path, void *) {'''
new = '''// Double click on a file in the Project window: hand it to the machine's
// editor. VS Code if it is installed - the path is the only argument it
// needs, and the project folder is already where it looks.
static int open_asset_cb(const char *, const char *rel_path, void *) {
    if (!rel_path || !*rel_path || !g_assets_dir[0]) return 0;
    char full[700];
    std::snprintf(full, sizeof(full), "%s/%s", g_assets_dir, rel_path);
#ifdef _WIN32
    // VS Code's installer puts its bin dir on PATH for the user who installed
    // it; a shipped editor cannot rely on being started from that shell, so
    // the usual install locations are checked too.
    const char *CODE_PATHS[] = {
        "%LOCALAPPDATA%\\\\Programs\\\\Microsoft VS Code\\\\bin\\\\code.cmd",
        "%ProgramFiles%\\\\Microsoft VS Code\\\\bin\\\\code.cmd",
        nullptr
    };
    char cmd[1200];
    for (int i = 0; CODE_PATHS[i]; ++i) {
        char path[700];
        DWORD n = ExpandEnvironmentStringsA(CODE_PATHS[i], path, sizeof(path));
        if (!n || GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) continue;
        std::snprintf(cmd, sizeof(cmd), "cmd /c start \"\" \"%s\" --goto \"%s\"", path, full);
        return std::system(cmd) == 0 ? 1 : 0;
    }
    std::snprintf(cmd, sizeof(cmd), "cmd /c start \"\" code --goto \"%s\"", full);
    if (std::system(cmd) == 0) return 1;
    // Nothing named code anywhere: let the OS open it with whatever the
    // extension is registered to - still better than silently doing nothing.
    std::snprintf(cmd, sizeof(cmd), "cmd /c start \"\" \"%s\"", full);
    return std::system(cmd) == 0 ? 1 : 0;
#else
    char cmd[1200];
    std::snprintf(cmd, sizeof(cmd),
                  "(command -v code >/dev/null && code --goto '%s') || xdg-open '%s' &", full, full);
    return std::system(cmd) == 0 ? 1 : 0;
#endif
}

static int prefab_save_cb(const char *node_id, const char *rel_path, void *) {'''
assert s.count(old)==1, s.count(old)
io.open(P,'w',encoding='utf-8').write(s.replace(old,new)); print("ok open_asset_cb")
