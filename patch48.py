#!/usr/bin/env python3
# patch48 - scenes are assets: they live in assets/Scenes, the browser shows
# them, and double clicking one opens it instead of instantiating it.
import sys, os
ROOT = os.path.dirname(os.path.abspath(__file__))
def rd(p):
    with open(os.path.join(ROOT, p), 'r', encoding='utf-8') as f: return f.read()
def wr(p, s):
    full = os.path.join(ROOT, p); bak = full + '.bak_p48'
    if not os.path.exists(bak):
        with open(bak, 'w', encoding='utf-8') as f: f.write(rd(p))
    with open(full, 'w', encoding='utf-8') as f: f.write(s)
def sub1(t, old, new, what):
    if new in t: print('skip (already applied): ' + what); return t
    if old not in t: print('MISS: ' + what); sys.exit(1)
    if t.count(old) != 1: print('AMBIG %d: %s' % (t.count(old), what)); sys.exit(1)
    return t.replace(old, new)

s = rd('examples/editor_demo.cpp')

# ================================================== 1. where a scene lives now
s = sub1(s,
"""// ---- scenes as files -----------------------------------------------------
// Several scenes per project, like Unity: they live in <project>/scenes and
// opening one is just pointing g_scene_path at it - the main loop notices
// the change and does the load, the same path a project switch takes.
static dai_doc *g_scene_doc = nullptr;

static const char *scene_list(uint32_t index, void *) {
    static std::vector<std::string> names;
    if (index == 0) {
        names.clear();
        if (g_project) {
            std::string dir = std::string(dai_project_path(g_project)) + "/scenes";
            DIR *d = opendir(dir.c_str());
            if (d) {
                while (struct dirent *e = readdir(d)) {
                    std::string n = e->d_name;
                    if (n.size() > 9 && n.compare(n.size() - 9, 9, ".daidalos") == 0)
                        names.push_back(n);
                }
                closedir(d);
            }
            std::sort(names.begin(), names.end());
        }
    }
    if (index >= names.size()) return nullptr;
    return names[index].c_str();
}

static int scene_open(const char *name, void *) {
    if (!g_project || !name || !*name) return 0;
    std::snprintf(g_scene_path, sizeof(g_scene_path), "%s/scenes/%s",
                  dai_project_path(g_project), name);
    return 1;
}

static int scene_save_as(const char *name, void *) {
    if (!g_project || !g_scene_doc || !name || !*name) return 0;
    char path[640];
    std::snprintf(path, sizeof(path), "%s/scenes/%s", dai_project_path(g_project), name);
    if (dai_doc_save(g_scene_doc, path) != DAI_OK) return 0;
    std::snprintf(g_scene_path, sizeof(g_scene_path), "%s", path);
    return 1;
}""",
"""// ---- scenes as files -----------------------------------------------------
// Several scenes per project, like Unity - and, like Unity, a scene is an
// ASSET. It lives in assets/Scenes, which means the Project window lists it,
// F2 renames it, it can be dragged into a folder, and it is inside the one
// directory that gets shipped. <project>/scenes stayed invisible to every one
// of those, because the browser only ever mounts assets/.
//
// Opening one is still just pointing g_scene_path at it; the main loop
// notices the change and does the load, the same path a project switch takes.
static dai_doc *g_scene_doc = nullptr;

// <project>/assets/Scenes, made on demand. Capital S: it is a folder a person
// reads in a file browser, next to Materials and Models.
static std::string scene_dir() {
    if (!g_project) return std::string();
    std::string d = std::string(dai_project_asset_dir(g_project)) + "/Scenes";
#ifdef _WIN32
    CreateDirectoryA(d.c_str(), nullptr);
#else
    mkdir(d.c_str(), 0755);
#endif
    return d;
}

// Everything that used to be in <project>/scenes moves into assets/Scenes the
// first time a project is opened. Copy-then-unlink rather than rename: the two
// are always on the same filesystem here, but a failed rename would leave the
// project with no scene at all, and this way the worst case is a duplicate.
static void migrate_scenes_into_assets() {
    if (!g_project) return;
    std::string old_dir = std::string(dai_project_path(g_project)) + "/scenes";
    DIR *d = opendir(old_dir.c_str());
    if (!d) return;
    std::string dst_dir = scene_dir();
    int moved = 0;
    while (struct dirent *e = readdir(d)) {
        std::string n = e->d_name;
        if (n.size() <= 9 || n.compare(n.size() - 9, 9, ".daidalos") != 0) continue;
        std::string src = old_dir + "/" + n, dst = dst_dir + "/" + n;
        if (path_exists(dst.c_str())) continue;      // already there, leave both
        if (!copy_one_file(src.c_str(), dst.c_str())) continue;
        std::remove(src.c_str());
        ++moved;
    }
    closedir(d);
    if (moved && g_panels_for_log) {
        char msg[128];
        std::snprintf(msg, sizeof(msg), "moved %d scene(s) into assets/Scenes", moved);
        dai_editor_ui_log(g_panels_for_log, 0, msg);
    }
}

static const char *scene_list(uint32_t index, void *) {
    static std::vector<std::string> names;
    if (index == 0) {
        names.clear();
        if (g_project) {
            std::string dir = scene_dir();
            DIR *d = opendir(dir.c_str());
            if (d) {
                while (struct dirent *e = readdir(d)) {
                    std::string n = e->d_name;
                    if (n.size() > 9 && n.compare(n.size() - 9, 9, ".daidalos") == 0)
                        names.push_back(n);
                }
                closedir(d);
            }
            std::sort(names.begin(), names.end());
        }
    }
    if (index >= names.size()) return nullptr;
    return names[index].c_str();
}

static int scene_open(const char *name, void *) {
    if (!g_project || !name || !*name) return 0;
    // A bare file name, or the asset-relative "Scenes/x.daidalos" the Project
    // window hands over - both mean the same file.
    const char *slash = std::strrchr(name, '/');
    std::string base = slash ? slash + 1 : name;
    std::snprintf(g_scene_path, sizeof(g_scene_path), "%s/%s",
                  scene_dir().c_str(), base.c_str());
    return 1;
}

static int scene_save_as(const char *name, void *) {
    if (!g_project || !g_scene_doc || !name || !*name) return 0;
    const char *slash = std::strrchr(name, '/');
    std::string base = slash ? slash + 1 : name;
    std::string path = scene_dir() + "/" + base;
    if (dai_doc_save(g_scene_doc, path.c_str()) != DAI_OK) return 0;
    std::snprintf(g_scene_path, sizeof(g_scene_path), "%s", path.c_str());
    return 1;
}""",
    'scene dir + migration')

# ============================== 2. opening a project points at the new place
s = sub1(s,
"""    std::snprintf(g_scene_path, sizeof(g_scene_path), "%s", dai_project_scene_path(g_project));
    std::snprintf(g_assets_dir, sizeof(g_assets_dir), "%s", dai_project_asset_dir(g_project));""",
"""    std::snprintf(g_assets_dir, sizeof(g_assets_dir), "%s", dai_project_asset_dir(g_project));
    // Scenes are assets now; anything a previous version left in
    // <project>/scenes comes along, and only then is the startup scene
    // chosen - otherwise the first open of an old project finds nothing.
    migrate_scenes_into_assets();
    {
        std::string sd = scene_dir();
        std::string want = sd + "/main.daidalos";
        if (!path_exists(want.c_str())) {
            // Whatever scene the folder DOES have, alphabetically first, so a
            // project whose scene is called level1 still opens something.
            const char *first = scene_list(0, nullptr);
            if (first) want = sd + "/" + first;
        }
        if (path_exists(want.c_str()))
            std::snprintf(g_scene_path, sizeof(g_scene_path), "%s", want.c_str());
        else
            std::snprintf(g_scene_path, sizeof(g_scene_path), "%s", want.c_str());
    }""",
    'open_project points into assets/Scenes')

# ===================== 3. the assets dir comes from the project, not a substring
s = sub1(s,
"""            // <project>/assets is the mounted folder.
            std::snprintf(g_assets_dir, sizeof(g_assets_dir), "%s", current_scene);
            char *slash = std::strstr(g_assets_dir, "/scenes/");
            if (slash) {
                *slash = 0;
                std::strncat(g_assets_dir, "/assets", sizeof(g_assets_dir) - std::strlen(g_assets_dir) - 1);
                if (assets) {""",
"""            // <project>/assets is the mounted folder. Asked of the project,
            // not carved out of the scene path with strstr("/scenes/") - the
            // scene lives INSIDE assets now, so that substring is gone and
            // the old code would have stopped remounting anything at all.
            if (g_project) {
                std::snprintf(g_assets_dir, sizeof(g_assets_dir), "%s",
                              dai_project_asset_dir(g_project));
                if (assets) {""",
    'assets dir from project')

# =========================== 4. a scene asset OPENS, a prefab asset instantiates
s = sub1(s,
"""            if (dai_editor_ui_take_asset(panels, &pick, &as_tree) && pick) {
                size_t pl = std::strlen(pick);
                if (pl > 9 && std::strcmp(pick + pl - 9, ".daidalos") == 0) {""",
"""            if (dai_editor_ui_take_asset(panels, &pick, &as_tree) && pick) {
                size_t pl = std::strlen(pick);
                bool in_scenes = std::strncmp(pick, "Scenes/", 7) == 0 ||
                                 std::strncmp(pick, "scenes/", 7) == 0;
                if (pl > 9 && std::strcmp(pick + pl - 9, ".daidalos") == 0 && in_scenes) {
                    // A scene file OPENS. Instantiating it as a prefab would
                    // paste the whole level into the level you are standing
                    // in, which is never what a double click on a scene means.
                    if (scene_open(pick, nullptr))
                        dai_editor_ui_toast(panels, "opening scene", 1.5f);
                } else if (pl > 9 && std::strcmp(pick + pl - 9, ".daidalos") == 0) {""",
    'scene asset opens')
wr('examples/editor_demo.cpp', s)
print('patch48 ok')
