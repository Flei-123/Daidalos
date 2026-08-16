// A real editor window: hierarchy, inspector, gizmo, play mode, and a Unity
// style viewport camera.
//
//   DAI_SHADER_DIR=shaders ./build/editor_demo [scene.scene]
//
// Camera (Unity, not Blender):
//   right mouse      look around; while held WASD moves, Q/E down/up,
//                    shift boosts, wheel changes the move speed
//   wheel            dolly
//   middle mouse     pan
//   alt + left       orbit
//   F                frame the selection
// Editing: left click selects, drag a gizmo arm to move/rotate/scale.
//   W/E/R switch gizmo mode (when the right button is not held), Ctrl+Z / Y
//   undo and redo, Ctrl+S saves, Delete removes, Ctrl+D duplicates.

#include "dai_editor_ui.h"
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif
#include "dai_tr.h"
#include "dai_render.h"
#include "dai_assets.h"
#include "dai_thumb.h"

#include <map>
#include "dai_material.h"
#include "dai_project.h"
#include "dai_show_ui.h"
#include "dai_gltf.h"
#include "dai_update.h"

#include <ctime>
#ifdef DAI_WITH_SCRIPT
#include "dai_script.h"
#include "dai_prelude.h"
#include "dai_native.h"
#endif

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include <algorithm>
#include <new>
#include <csignal>    // the crash handler's POSIX half
#include <dirent.h>   // mingw has it too - one directory API for both
#if !defined(_WIN32) && defined(__GLIBC__)
#include <execinfo.h>  // backtrace(); glibc only, and only used there
#endif
#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>
#endif
#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#endif

// ---- crash diagnostics ---------------------------------------------------
// A crash that only happens on someone else's machine is only debuggable if
// the console says WHERE it was. Three things do that: unbuffered output (a
// buffered line is lost when the process aborts), a breadcrumb per stage of
// the first frames, and a stack of return addresses when an allocation fails
// - std::bad_alloc with no stack is a shrug, addresses map back to lines.
// Off unless DAIDALOS_DIAG=1 is set in the environment: the breadcrumbs are
// for debugging a machine that crashes, not for everyone's console.
static int g_diag_on = 0;
static int g_diag_frames = 0;

static void diag_stack(const char *why) {
    std::printf("\n!! DIAG: %s\n", why);
    std::fflush(stdout);
#ifdef _WIN32
    void *frames[26] = { nullptr };
    USHORT n = CaptureStackBackTrace(0, 26, frames, nullptr);
    HMODULE base = GetModuleHandleA(nullptr);
    std::printf("!! module base %p\n", (void *)base);
    for (USHORT i = 0; i < n; ++i)
        std::printf("!! #%02u %p  +0x%llx\n", (unsigned)i, frames[i],
                    (unsigned long long)((char *)frames[i] - (char *)base));
    std::fflush(stdout);
#endif
}

static void diag_step(const char *what) {
    if (g_diag_frames <= 0) return;
    std::printf("[step] %s\n", what);
    std::fflush(stdout);
}

static void diag_new_failed() {
    diag_stack("allocation failed (this is the std::bad_alloc)");
    std::fflush(stdout);
    std::abort();
}

// ---- self update: a newer build replaces this one, politely ---------------
// The check runs on its own thread so a slow or absent server never delays
// the window opening. When a staged build is verified the editor exits and a
// tiny batch file (written by dai_self_update_restart) swaps the exe once
// this process is gone and starts it again.
static struct UpdateCheck {
    std::atomic<int> state{0};   // 0 running, 1 current, 2 unreachable, 3 staged, 4 refused
    dai_self_update info{};
    char exe_path[512]{};
    char note[256]{};
} g_update;

// Every verdict, appended to a file beside the exe.
//
// Until now the only place the update said anything was stdout, and a Windows
// editor has no stdout: "update: not applied (cannot write ...)" was printed
// into a handle nobody owns. The result was a self update that either worked
// or did nothing at all with no way to tell which, and "irgendwie wird's nicht
// geupdatet" is the bug report that produces. One line per start, kept short,
// next to the binary it is about - so the next time the question is asked it
// is answerable by reading a file rather than by guessing.
static void update_log(const char *what) {
    if (!g_update.exe_path[0] || !what) return;
    char path[600];
    std::snprintf(path, sizeof(path), "%s.update.log", g_update.exe_path);
    FILE *f = std::fopen(path, "ab");
    if (!f) return;
    std::time_t t = std::time(nullptr);
    char when[32] = { 0 };
    std::tm tmv{};
#ifdef _WIN32
    if (gmtime_s(&tmv, &t) == 0) std::strftime(when, sizeof(when), "%Y-%m-%dT%H:%M:%SZ", &tmv);
#else
    if (gmtime_r(&t, &tmv)) std::strftime(when, sizeof(when), "%Y-%m-%dT%H:%M:%SZ", &tmv);
#endif
    std::fprintf(f, "%s  %s\n", when[0] ? when : "?", what);
    std::fclose(f);
}

static void update_worker() {
    char err[256] = { 0 };
    if (dai_self_update_check("https://daidalos.fleitec.com/api/version.json",
                              g_update.exe_path, &g_update.info, err, sizeof(err)) != DAI_OK) {
        std::snprintf(g_update.note, sizeof(g_update.note), "update: no check (%s)", err);
        update_log(g_update.note);
        g_update.state = 2;
        return;
    }
    if (!g_update.info.needed) {
        std::snprintf(g_update.note, sizeof(g_update.note), "update: current (%s)",
                      g_update.info.version);
        update_log(g_update.note);
        g_update.state = 1;
        if (!g_update.info.sidecar)
            dai_self_update_mark_current(g_update.exe_path, g_update.info.sha256);
        return;
    }
    {
        char line[256];
        std::snprintf(line, sizeof(line), "update: %s available (%llu bytes), downloading to \"%s.new\"",
                      g_update.info.version, (unsigned long long)g_update.info.size,
                      g_update.exe_path);
        update_log(line);
        std::printf("%s\n", line);
    }
    char err2[256] = { 0 };
    if (dai_self_update_stage(&g_update.info, g_update.exe_path, err2, sizeof(err2)) != DAI_OK) {
        // A refused download is a log line, not a crash - the old build runs on.
        std::snprintf(g_update.note, sizeof(g_update.note), "update: not applied (%s)", err2);
        update_log(g_update.note);
        g_update.state = 4;
        return;
    }
    std::snprintf(g_update.note, sizeof(g_update.note), "update: %s verified, restarting",
                  g_update.info.version);
    update_log(g_update.note);
    g_update.state = 3;
}

// ---- the project host: the editor's "New Project" needs a disk ------------
// A project is a folder: scenes and assets under one root. That is the entire
// definition - a Unity project is also just a folder with opinions.
static char g_projects_root[512] = { 0 };
static char g_scene_path[512] = { 0 };

static dai_project *g_project = nullptr;
static char g_assets_dir[512] = { 0 };
// The project's string tables live further down, next to the settings panel
// that picks between them - but the project OPENS up here, which is the one
// moment the answer to "which languages exist" can change.
static void strings_scan();
static void strings_use(const char *code);
static std::vector<std::string> &strings_langs();

// The console panel, so anything in this file can report into the editor
// instead of only onto a stdout nobody is looking at.
static dai_editor_ui *g_panels_for_log = nullptr;

// ---- the drone show half of the editor ------------------------------------
//
// A droneshow project is the same binary with a different panel set. The
// document lives here, next to the scene document, for the same reason: the
// host is what opens a project, so the host is what knows which kind it is.
static dai_show          *g_show = nullptr;
static dai_show_ui       *g_show_ui = nullptr;
// The mesh the storyboard samples from: the selected asset, repacked into the
// plain arrays the pipeline takes. Kept alive between frames because the
// sample descriptor holds pointers into it.
static std::vector<float>    g_show_mesh_pos;
static std::vector<uint32_t> g_show_mesh_idx;
static std::string           g_show_mesh_src;
// The asset list as the Project panel last saw it, so the show can turn a
// selected row back into a file name. The panel is fed pointers, not names.
static char     g_asset_paths[256][96];
static uint32_t g_asset_count = 0;

static int show_read_file(const char *path, std::vector<uint8_t> &out) {
    FILE *f = std::fopen(path, "rb");
    if (!f) return 0;
    std::fseek(f, 0, SEEK_END);
    long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (n <= 0) { std::fclose(f); return 0; }
    out.resize((size_t)n);
    size_t got = std::fread(out.data(), 1, (size_t)n, f);
    std::fclose(f);
    out.resize(got);
    return got > 0;
}


// Defined further down, used by open_project_path right below: where scenes
// live, how old projects get theirs moved, and the two file questions the
// migration asks.
static std::string scene_dir();
static void        migrate_scenes_into_assets();
static const char *scene_list(uint32_t index, void *user);
static int         path_exists(const char *p);
static int         copy_one_file(const char *src, const char *dst);
static void        write_editor_support_files();

// The project list, from disk, through the project layer - it knows what a
// project is (assets/, scenes/, settings/) so this does not have to.
static const char *project_list(uint32_t index, void *) {
    static std::vector<std::string> names;
    if (index == 0) {
        names.clear();
        char buf[64][DAI_PROJECT_NAME_MAX];
        uint32_t n = dai_project_list(g_projects_root, buf[0], 64, DAI_PROJECT_NAME_MAX);
        if (n > 64) n = 64;
        // dai_project_list returns FULL paths (its tested contract). The
        // picker shows and hands over bare names - a full path here built
        // 'root/root/Name' and clicking a project silently did nothing.
        std::string root = g_projects_root;
        for (char &c : root) if (c == '\\') c = '/';
        if (!root.empty() && root.back() != '/') root += '/';
        for (uint32_t i = 0; i < n; ++i) {
            std::string f = buf[i];
            for (char &c : f) if (c == '\\') c = '/';
            names.push_back(f.compare(0, root.size(), root) == 0 ? f.substr(root.size()) : f);
        }
    }
    if (index >= names.size()) return nullptr;
    return names[index].c_str();
}

// Where a show is kept: one file per project, next to the scenes, because a
// show IS the project in a droneshow project - there is no second one.
static std::string show_path(void) {
    if (!g_project) return std::string();
    return std::string(dai_project_path(g_project)) + "/scenes/show.dshow";
}

// Closes whatever show was open and, if the project that is now open is a
// droneshow, builds the document and the panel set for it. A game project
// leaves both null, and then the editor is byte for byte the editor it was.
static void show_open_for_project(void) {
    if (g_show_ui) { dai_show_ui_destroy(g_show_ui); g_show_ui = nullptr; }
    if (g_show)    { dai_show_destroy(g_show);       g_show    = nullptr; }
    g_show_mesh_src.clear();
    if (g_panels_for_log) dai_editor_ui_show_host(g_panels_for_log, nullptr);
    if (!g_project || dai_project_kind(g_project) != DAI_PROJECT_DRONESHOW) return;

    // The safety numbers come out of settings/project.txt, which is where they
    // are versioned. The show document never invents them.
    dai_project_settings ps = dai_project_settings_default();
    dai_project_settings_load(g_project, &ps);
    dai_show_settings ss = dai_show_settings_default();
    ss.min_distance_m       = ps.min_distance_m;
    ss.v_max_ms             = ps.v_max_ms;
    ss.a_max_ms2            = ps.a_max_ms2;
    ss.drone_count          = ps.drone_count;
    ss.show_origin_lat      = ps.show_origin_lat;
    ss.show_origin_lon      = ps.show_origin_lon;
    ss.show_origin_amsl     = ps.show_origin_amsl;
    ss.show_orientation_deg = ps.show_orientation_deg;
    ss.takeoff_alt_m        = ps.takeoff_alt_m;
    ss.fps                  = ps.fps;
    ss.fence_half_x         = ps.fence_half_x;
    ss.fence_half_z         = ps.fence_half_z;
    ss.fence_top_m          = ps.fence_top_m;
    ss.min_ground_m         = ps.min_ground_m;
    ss.seed                 = ps.show_seed;

    char err[256] = { 0 };
    std::string sp = show_path();
    g_show = path_exists(sp.c_str()) ? dai_show_load(sp.c_str(), err, sizeof(err)) : nullptr;
    if (!g_show) g_show = dai_show_create(&ss);
    else         dai_show_set_settings(g_show, &ss);
    g_show_ui = g_show ? dai_show_ui_create(g_show) : nullptr;
    if (g_panels_for_log) dai_editor_ui_show_host(g_panels_for_log, g_show_ui);
}

static void show_save(void) {
    if (!g_show) return;
    char err[256] = { 0 };
    std::string sp = show_path();
    if (dai_show_save(g_show, sp.c_str(), err, sizeof(err)) != DAI_OK && g_panels_for_log)
        dai_editor_ui_log(g_panels_for_log, 2, err);
}

// Opening a project is the ONE thing that decides where everything else comes
// from: the scene, the assets, the settings. The editor never runs without
// one - that is why this is called before the first frame as well as from the
// project window.
static void layout_save_for_project(void);
static void layout_load_for_project(void);

static int open_project_path(const char *path) {
    char err[256] = { 0 };
    layout_save_for_project();   // g_project is still the one being LEFT
    dai_project *np = dai_project_open(path, err, sizeof(err));
    if (!np) { std::printf("project: %s\n", err); return 0; }
    if (g_project) dai_project_close(g_project);
    g_project = np;
    std::snprintf(g_assets_dir, sizeof(g_assets_dir), "%s", dai_project_asset_dir(g_project));
    // Which languages this project has, and which one to preview in. Done
    // here because it is the one moment the answer can change.
    strings_scan();
    {
        dai_project_settings ps = dai_project_settings_default();
        dai_project_settings_load(g_project, &ps);
        std::vector<std::string> &ls = strings_langs();
        const char *want = ps.language[0] ? ps.language
                         : (ls.empty() ? "" : ls[0].c_str());
        strings_use(want);
    }
    // Scenes are assets now; anything a previous version left in
    // <project>/scenes comes along, and only then is the startup scene
    // chosen - otherwise the first open of an old project finds nothing.
    migrate_scenes_into_assets();
    // The .d.ts and jsconfig.json that make an external editor understand the
    // engine. Written every open so they cannot go stale.
    write_editor_support_files();
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
    }
    dai_prefs pr = dai_prefs_default();
    dai_prefs_load(&pr);
    std::snprintf(pr.last_project, sizeof(pr.last_project), "%s", dai_project_path(g_project));
    dai_prefs_save(&pr);
    show_open_for_project();
    // The layout belongs to the project, so it switches WITH the project:
    // the show that was open gets its arrangement written, the one being
    // opened gets its own back - or the default for its kind on the first
    // open. g_project still names the OLD project at the top of this
    // function, which is why the save happens before the switch, below.
    layout_load_for_project();
    return 1;
}

static int project_create(const char *name, void *) {
    char err[256] = { 0 };
    // Game or drone show - the picker's toggle, read at the moment of the
    // click. Everything else about creating a project is identical.
    int kind = g_panels_for_log ? dai_editor_ui_project_new_kind(g_panels_for_log)
                                : DAI_PROJECT_GAME;
    dai_project *np = dai_project_create_kind(g_projects_root, name, kind, err, sizeof(err));
    if (!np) { std::printf("project: %s\n", err); return 0; }
    std::string path = dai_project_path(np);
    dai_project_close(np);
    return open_project_path(path.c_str());
}

static int project_open(const char *name, void *) {
    // Bare name from the picker, or a full path (startup fallback, older
    // callers) - both open.
    if (name && (std::strchr(name, '/') || std::strchr(name, '\\') || std::strchr(name, ':')))
        return open_project_path(name);
    char path[640];
    std::snprintf(path, sizeof(path), "%s/%s", g_projects_root, name);
    return open_project_path(path);
}

static int folder_create(const char *name, void *) {
    if (!name || !*name || !g_assets_dir[0]) return 0;
    char path[640];
    std::snprintf(path, sizeof(path), "%s/%s", g_assets_dir, name);
#ifdef _WIN32
    return CreateDirectoryA(path, nullptr) ? 1 : 0;
#else
    return mkdir(path, 0755) == 0 ? 1 : 0;
#endif
}

// ---- named layouts --------------------------------------------------------
static void layout_save_file(const char *name, const char *text, size_t n, void *) {
    if (!name || !text) return;
    char path[640];
    std::snprintf(path, sizeof(path), "%s/%s.layout", g_projects_root, name);
    FILE *f = std::fopen(path, "wb");
    if (!f) return;
    std::fwrite(text, 1, n, f);
    std::fclose(f);
}
static int layout_load_file(const char *name, char *out, size_t n, void *) {
    if (!name || !out || !n) return 0;
    char path[640];
    std::snprintf(path, sizeof(path), "%s/%s.layout", g_projects_root, name);
    FILE *f = std::fopen(path, "rb");
    if (!f) return 0;
    size_t got = std::fread(out, 1, n - 1, f);
    out[got] = 0;
    std::fclose(f);
    return (int)got;
}

// ---- the working layout, per project --------------------------------------
// One layout.txt for every project was one layout too few: a droneshow docks
// Storyboard, Show Parameters and Validation where a game docks nothing, so
// whichever kind was closed second overwrote the other's arrangement and the
// next open apologised with the wrong one. The layout now lives in the
// project's settings/ - it IS a project setting - and a project switch saves
// the old project's and loads the new one's, the same rule the scene and the
// strings already follow. The old global file is still READ as a fallback, so
// an existing install keeps its arrangement once, and then migrates itself.
static std::string project_layout_path(void) {
    if (!g_project) return std::string();
    return std::string(dai_project_path(g_project)) + "/settings/editor_layout.txt";
}

static void layout_save_for_project(void) {
    if (!g_panels_for_log || !g_project) return;
    std::string path = project_layout_path();
    size_t need = dai_editor_ui_layout_save(g_panels_for_log, nullptr, 0);
    if (!need) return;
    std::string txt(need + 1, '\0');
    dai_editor_ui_layout_save(g_panels_for_log, &txt[0], txt.size());
    FILE *f = std::fopen(path.c_str(), "wb");
    if (!f) return;
    std::fwrite(txt.c_str(), 1, need, f);
    std::fclose(f);
}

static void layout_load_for_project(void) {
    if (!g_panels_for_log) return;
    std::string txt;
    {
        std::string path = project_layout_path();
        FILE *f = path.empty() ? nullptr : std::fopen(path.c_str(), "rb");
        // The first open of a project that predates per-project layouts: the
        // old global file, so nothing anybody arranged is thrown away.
        if (!f && g_project) {
            char gp[640];
            std::snprintf(gp, sizeof(gp), "%s/layout.txt", g_projects_root);
            f = std::fopen(gp, "rb");
        }
        if (f) {
            char chunk[1024]; size_t got;
            while ((got = std::fread(chunk, 1, sizeof(chunk), f)) > 0) txt.append(chunk, got);
            std::fclose(f);
        }
    }
    if (!txt.empty() && dai_editor_ui_layout_load(g_panels_for_log, txt.c_str()) == DAI_OK)
        return;
    // Nothing stored (or a file that does not parse): the default for the
    // KIND of project this is - a show opens as a show, a game as a game.
    dai_editor_ui_layout_reset(g_panels_for_log, 0.0f, 0.0f);
}

// ---- scenes as files -----------------------------------------------------
// Several scenes per project, like Unity - and, like Unity, a scene is an
// ASSET. It lives in assets/Scenes, which means the Project window lists it,
// F2 renames it, it can be dragged into a folder, and it is inside the one
// directory that gets shipped. <project>/scenes stayed invisible to every one
// of those, because the browser only ever mounts assets/.
//
// Opening one is still just pointing g_scene_path at it; the main loop
// notices the change and does the load, the same path a project switch takes.
static dai_doc *g_scene_doc = nullptr;
// Prefab mode: the scene to come back to. Empty means we are in the world.
static char g_prefab_return[512] = { 0 };
// The document revision as of the last successful write. Everything above it
// is unsaved work - which is the only definition of "dirty" that cannot drift,
// because it is the same counter the undo system moves.
static uint64_t g_saved_rev = 0;

// The model a drag is currently holding over the viewport. It is a real node
// in the real document - that is the whole point - so abandoning the drag has
// to take it back out again, and taking it out is exactly the undo step the
// instantiate pushed.
static dai_node    g_preview_node = DAI_INVALID_NODE;
static std::string g_preview_path;
static uint32_t    g_preview_undo = 0;

static void preview_drop(dai_doc *doc, dai_doc_sync *sync) {
    if (g_preview_node == DAI_INVALID_NODE) { g_preview_path.clear(); return; }
    // Undo, not delete: the instantiate was one transaction and popping it
    // takes the whole tree - the root and every piece under it - in one go.
    // Deleting the root by hand would leave the children orphaned in the
    // document, which is the bug this used to be.
    if (dai_doc_undo_depth(doc) == g_preview_undo) dai_doc_undo(doc);
    else                                           dai_doc_remove(doc, g_preview_node);
    g_preview_node = DAI_INVALID_NODE;
    g_preview_path.clear();
    dai_doc_sync_apply(sync);
}

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
}

// (g_panels_for_log is declared with the other globals at the top - the scene
// migration reports through it, and that runs before this line.)

#ifdef DAI_WITH_SCRIPT
// ---- the behaviour runner --------------------------------------------------
// Play presses start: every node's scripts load, get the scene bindings and
// their assigned references as `params`, and hear init(). frame() runs every
// rendered frame until Stop tears the whole set down with the world.
static dai_editor *g_script_ed = nullptr;

static double sh_find(const char *name, void *) {
    if (!name || !*name || !g_scene_doc) return -1.0;
    uint32_t n = dai_doc_count(g_scene_doc);
    std::vector<dai_node> all(n);
    if (n) dai_doc_nodes(g_scene_doc, all.data(), n);
    for (dai_node id : all) {
        dai_node_desc r{};
        if (dai_doc_get(g_scene_doc, id, &r) == DAI_OK && std::strcmp(r.name, name) == 0)
            return (double)(uint32_t)id;
    }
    return -1.0;
}
static int sh_get_pos(double id, double *xyz, void *) {
    dai_vec3 p{};
    if (!g_script_ed || !dai_editor_live_transform(g_script_ed, (dai_node)(uint32_t)id, &p, nullptr, nullptr)) return 0;
    xyz[0] = p.x; xyz[1] = p.y; xyz[2] = p.z;
    return 1;
}
static void sh_set_pos(double id, const double *xyz, void *) {
    if (!g_script_ed) return;
    dai_vec3 p{ (float)xyz[0], (float)xyz[1], (float)xyz[2] };
    dai_editor_live_set_transform(g_script_ed, (dai_node)(uint32_t)id, &p, nullptr);
}
static int sh_get_rot(double id, double *xyzw, void *) {
    dai_quat q{};
    if (!g_script_ed || !dai_editor_live_transform(g_script_ed, (dai_node)(uint32_t)id, nullptr, &q, nullptr)) return 0;
    xyzw[0] = q.x; xyzw[1] = q.y; xyzw[2] = q.z; xyzw[3] = q.w;
    return 1;
}
static void sh_set_rot(double id, const double *xyzw, void *) {
    if (!g_script_ed) return;
    dai_quat q{ (float)xyzw[0], (float)xyzw[1], (float)xyzw[2], (float)xyzw[3] };
    dai_editor_live_set_transform(g_script_ed, (dai_node)(uint32_t)id, nullptr, &q);
}
// The label on a node, from a script. THE reason a HUD exists: a score that
// cannot change is a decoration.
//
// It writes the DOCUMENT, not a live copy, because a Text component has no
// body and nothing simulates it - and dai_hud_draw reads the document. Play
// still restores it, because Stop restores the whole document snapshot.
static void sh_set_text(double id, const char *str, void *) {
    if (!g_scene_doc) return;
    dai_node_desc r{};
    if (dai_doc_get(g_scene_doc, (dai_node)(uint32_t)id, &r) != DAI_OK) return;
    std::snprintf(r.text, sizeof(r.text), "%s", str ? str : "");
    if (!r.text_on) r.text_on = 1;      // setting text on a node means show it
    // No dai_doc_begin/commit: a score changing sixty times a second must not
    // put sixty entries on the undo stack. dai_doc_set without a transaction
    // is the "this is not an edit" path.
    dai_doc_set(g_scene_doc, (dai_node)(uint32_t)id, &r);
}

// ---- the component bridge ------------------------------------------------
// ONE table, addressed by name, shared by the JavaScript prelude and the C++
// behaviour header. Both languages ask "what is light.intensity on node 7";
// neither needs its own path into the document, and a component added to the
// editor tomorrow is one line here rather than two new bindings and a header.
//
// It writes with dai_doc_set and NO dai_doc_begin/commit, for the reason
// sh_set_text gives above: a behaviour changing a colour sixty times a second
// must not put sixty entries on the undo stack. Play restores the document
// snapshot on Stop, so nothing a script did survives into the saved scene.
namespace {

// Where the name points. Split so a getter and a setter cannot disagree about
// which field a name means, which is exactly the bug a second switch invites.
enum PropKind { P_NONE, P_NUM, P_VEC, P_STR };

struct PropRef {
    PropKind kind = P_NONE;
    float   *f = nullptr;
    int     *i = nullptr;
    dai_vec3 *v = nullptr;
    char    *str = nullptr;
    size_t   str_len = 0;
    int      bool_of_int = 0;   // 1 = the int is a flag, and 0/1 is the answer
    int      invert = 0;        // no_body is "enabled" upside down
};

PropRef prop_ref(dai_node_desc &r, const char *name) {
    PropRef p;
    auto num = [&](float *f) { p.kind = P_NUM; p.f = f; return p; };
    auto ival = [&](int *i) { p.kind = P_NUM; p.i = i; return p; };
    auto flag = [&](int *i, int inv) { p.kind = P_NUM; p.i = i; p.bool_of_int = 1; p.invert = inv; return p; };
    auto vec = [&](dai_vec3 *v) { p.kind = P_VEC; p.v = v; return p; };
    auto text = [&](char *c, size_t n) { p.kind = P_STR; p.str = c; p.str_len = n; return p; };
    if (!name) return p;

    if (!std::strcmp(name, "node.name"))            return text(r.name, sizeof(r.name));
    if (!std::strcmp(name, "node.tag"))             return text(r.tag, sizeof(r.tag));
    if (!std::strcmp(name, "node.asset"))           return text(r.asset, sizeof(r.asset));

    if (!std::strcmp(name, "transform.scale"))      return vec(&r.scale);

    if (!std::strcmp(name, "rigidbody.density"))    return num(&r.density);
    if (!std::strcmp(name, "rigidbody.friction"))   return num(&r.friction);
    if (!std::strcmp(name, "rigidbody.restitution"))return num(&r.restitution);
    if (!std::strcmp(name, "rigidbody.motion"))     return ival(&r.motion);
    if (!std::strcmp(name, "rigidbody.trigger"))    return flag(&r.trigger, 0);
    // "enabled" is the readable side of no_body: a node WITH a rigidbody is
    // the normal case, and a script should not have to spell a double negative.
    if (!std::strcmp(name, "rigidbody.enabled"))    return flag(&r.no_body, 1);

    if (!std::strcmp(name, "camera.mode"))          return ival(&r.camera);
    if (!std::strcmp(name, "camera.enabled"))       return flag(&r.camera, 0);
    if (!std::strcmp(name, "camera.fov"))           return num(&r.camera_fov);
    if (!std::strcmp(name, "camera.size"))          return num(&r.camera_size);

    if (!std::strcmp(name, "light.mode"))           return ival(&r.light);
    if (!std::strcmp(name, "light.enabled"))        return flag(&r.light, 0);
    if (!std::strcmp(name, "light.range"))          return num(&r.light_range);
    if (!std::strcmp(name, "light.intensity"))      return num(&r.light_intensity);
    if (!std::strcmp(name, "light.cone"))           return num(&r.light_cone);
    if (!std::strcmp(name, "light.color"))          return vec(&r.light_color);

    if (!std::strcmp(name, "text.enabled"))         return flag(&r.text_on, 0);
    if (!std::strcmp(name, "text.value"))           return text(r.text, sizeof(r.text));
    if (!std::strcmp(name, "text.size"))            return num(&r.text_size);
    if (!std::strcmp(name, "text.anchor"))          return ival(&r.text_anchor);
    if (!std::strcmp(name, "text.color"))           return vec(&r.text_color);

    if (!std::strcmp(name, "image.enabled"))        return flag(&r.sprite, 0);
    if (!std::strcmp(name, "image.size"))           return vec(&r.sprite_size);
    if (!std::strcmp(name, "image.asset"))          return text(r.asset, sizeof(r.asset));
    return p;                                        // unknown: answers the fallback
}

// The words a component property can be read out of. Returned by pointer, so
// the buffer has to outlive the call - a static one is right here: the value
// is copied into a JS string or handed to a behaviour that uses it that frame.
char g_prop_str[512];

double comp_get_num(dai_node id, const char *name, double fallback) {
    if (!g_scene_doc) return fallback;
    dai_node_desc r{};
    if (dai_doc_get(g_scene_doc, id, &r) != DAI_OK) return fallback;
    PropRef p = prop_ref(r, name);
    if (p.kind != P_NUM) return fallback;
    if (p.f) return (double)*p.f;
    if (!p.i) return fallback;
    int v = *p.i;
    if (p.bool_of_int) return (p.invert ? (v == 0) : (v != 0)) ? 1.0 : 0.0;
    return (double)v;
}

void comp_set_num(dai_node id, const char *name, double value) {
    if (!g_scene_doc) return;
    dai_node_desc r{};
    if (dai_doc_get(g_scene_doc, id, &r) != DAI_OK) return;
    PropRef p = prop_ref(r, name);
    if (p.kind != P_NUM) return;
    if (p.f) *p.f = (float)value;
    else if (p.i) {
        if (p.bool_of_int) {
            int on = value != 0.0 ? 1 : 0;
            // A flag that names a KIND (camera 1/2, light 1/2/3) must not be
            // stamped back to 1 when it was already 3 - turning a sun on
            // would quietly make it a point light.
            if (p.invert)                  *p.i = on ? 0 : 1;
            else if (!on)                  *p.i = 0;
            else if (*p.i == 0)            *p.i = 1;
        } else {
            *p.i = (int)value;
        }
    }
    dai_doc_set(g_scene_doc, id, &r);
}

int comp_get_vec(dai_node id, const char *name, double *xyz) {
    xyz[0] = xyz[1] = xyz[2] = 0.0;
    if (!g_scene_doc) return 0;
    dai_node_desc r{};
    if (dai_doc_get(g_scene_doc, id, &r) != DAI_OK) return 0;
    PropRef p = prop_ref(r, name);
    if (p.kind != P_VEC || !p.v) return 0;
    xyz[0] = p.v->x; xyz[1] = p.v->y; xyz[2] = p.v->z;
    return 1;
}

void comp_set_vec(dai_node id, const char *name, const double *xyz) {
    if (!g_scene_doc) return;
    dai_node_desc r{};
    if (dai_doc_get(g_scene_doc, id, &r) != DAI_OK) return;
    PropRef p = prop_ref(r, name);
    if (p.kind != P_VEC || !p.v) return;
    p.v->x = (float)xyz[0]; p.v->y = (float)xyz[1]; p.v->z = (float)xyz[2];
    dai_doc_set(g_scene_doc, id, &r);
}

const char *comp_get_str(dai_node id, const char *name) {
    g_prop_str[0] = 0;
    if (!g_scene_doc) return g_prop_str;
    dai_node_desc r{};
    if (dai_doc_get(g_scene_doc, id, &r) != DAI_OK) return g_prop_str;
    PropRef p = prop_ref(r, name);
    if (p.kind != P_STR || !p.str) return g_prop_str;
    std::snprintf(g_prop_str, sizeof(g_prop_str), "%s", p.str);
    return g_prop_str;
}

void comp_set_str(dai_node id, const char *name, const char *value) {
    if (!g_scene_doc) return;
    dai_node_desc r{};
    if (dai_doc_get(g_scene_doc, id, &r) != DAI_OK) return;
    PropRef p = prop_ref(r, name);
    if (p.kind != P_STR || !p.str) return;
    std::snprintf(p.str, p.str_len, "%s", value ? value : "");
    // Putting words in a Text turns it on, the same shortcut sh_set_text has.
    if (!std::strcmp(name, "text.value") && !r.text_on) r.text_on = 1;
    dai_doc_set(g_scene_doc, id, &r);
}

} // namespace

static double sh_get_num(double id, const char *prop, void *) {
    return comp_get_num((dai_node)(uint32_t)id, prop, 0.0);
}
static void sh_set_num(double id, const char *prop, double v, void *) {
    comp_set_num((dai_node)(uint32_t)id, prop, v);
}
static int sh_get_vec(double id, const char *prop, double *xyz, void *) {
    return comp_get_vec((dai_node)(uint32_t)id, prop, xyz);
}
static void sh_set_vec(double id, const char *prop, const double *xyz, void *) {
    comp_set_vec((dai_node)(uint32_t)id, prop, xyz);
}
static const char *sh_get_str(double id, const char *prop, void *) {
    return comp_get_str((dai_node)(uint32_t)id, prop);
}
static void sh_set_str(double id, const char *prop, const char *v, void *) {
    comp_set_str((dai_node)(uint32_t)id, prop, v);
}

static dai_script_node_host g_node_host = { sh_find, sh_get_pos, sh_set_pos, sh_get_rot, sh_set_rot,
                                            sh_set_text,
                                            sh_get_num, sh_set_num, sh_get_vec, sh_set_vec,
                                            sh_get_str, sh_set_str, nullptr };

// ---- native (C++) behaviours -------------------------------------------
//
// The same play/stop lifetime as the .js ones: a .cpp attached to a node is
// compiled to a shared library the first time Play is pressed after it
// changed, loaded, and called every frame. The API it sees is the C struct in
// dai_native.h - see that file for why it is not a base class.
static dai_native *g_native = nullptr;
static float       g_native_time = 0.0f;
static dai_window *g_win_for_scripts = nullptr;
// Pointer movement this frame, shared by the JS and the C++ side. Declared
// here because the native API table is built above where the play host lives.
static double g_mouse_dx = 0.0, g_mouse_dy = 0.0;
static int    g_mouse_buttons = 0;

static void nv_log(const dai_native_api *, const char *text) {
    if (!text) return;
    std::printf("cpp: %s\n", text);
    if (g_panels_for_log) dai_editor_ui_log(g_panels_for_log, 0, text);
}
static dai_nvec3 nv_get_pos(const dai_native_api *, dai_nentity e) {
    dai_vec3 p{};
    if (g_script_ed) dai_editor_live_transform(g_script_ed, (dai_node)e, &p, nullptr, nullptr);
    return dai_nvec3{ p.x, p.y, p.z };
}
static void nv_set_pos(const dai_native_api *, dai_entity e, dai_nvec3 v) {
    if (!g_script_ed) return;
    dai_vec3 p{ v.x, v.y, v.z };
    dai_editor_live_set_transform(g_script_ed, (dai_node)e, &p, nullptr);
}
static dai_nvec3 nv_get_vel(const dai_native_api *, dai_nentity e) {
    dai_vec3 l{}, a{};
    if (g_script_ed) dai_editor_live_velocity(g_script_ed, (dai_node)e, &l, &a);
    return dai_nvec3{ l.x, l.y, l.z };
}
static void nv_set_vel(const dai_native_api *, dai_entity e, dai_nvec3 v) {
    if (g_script_ed) dai_editor_live_set_velocity(g_script_ed, (dai_node)e, dai_vec3{ v.x, v.y, v.z });
}
static void nv_impulse(const dai_native_api *, dai_entity e, dai_nvec3 v) {
    if (g_script_ed) dai_editor_live_impulse(g_script_ed, (dai_node)e, dai_vec3{ v.x, v.y, v.z });
}
static dai_nvec3 nv_get_scale(const dai_native_api *, dai_nentity e) {
    dai_node_desc r{};
    if (g_scene_doc && dai_doc_get(g_scene_doc, (dai_node)e, &r) == DAI_OK)
        return dai_nvec3{ r.scale.x, r.scale.y, r.scale.z };
    return dai_nvec3{ 1, 1, 1 };
}
static void nv_set_scale(const dai_native_api *, dai_entity e, dai_nvec3 v) {
    dai_node_desc r{};
    if (!g_scene_doc || dai_doc_get(g_scene_doc, (dai_node)e, &r) != DAI_OK) return;
    r.scale = dai_vec3{ v.x, v.y, v.z };
    dai_doc_set(g_scene_doc, (dai_node)e, &r);
}
static void nv_get_rot(const dai_native_api *, dai_entity e, float *xyzw) {
    dai_quat q{ 0, 0, 0, 1 };
    if (g_script_ed) dai_editor_live_transform(g_script_ed, (dai_node)e, nullptr, &q, nullptr);
    if (xyzw) { xyzw[0] = q.x; xyzw[1] = q.y; xyzw[2] = q.z; xyzw[3] = q.w; }
}
static void nv_set_rot(const dai_native_api *, dai_entity e, const float *xyzw) {
    if (!g_script_ed || !xyzw) return;
    dai_quat q{ xyzw[0], xyzw[1], xyzw[2], xyzw[3] };
    dai_editor_live_set_transform(g_script_ed, (dai_node)e, nullptr, &q);
}
static dai_nentity nv_find(const dai_native_api *, const char *name) {
    double id = sh_find(name, nullptr);
    return id < 0 ? (dai_nentity)0 : (dai_nentity)(uint32_t)id;
}
static const char *nv_name_of(const dai_native_api *, dai_nentity e) {
    static char buf[64];
    dai_node_desc r{};
    if (g_scene_doc && dai_doc_get(g_scene_doc, (dai_node)e, &r) == DAI_OK)
        std::snprintf(buf, sizeof(buf), "%s", r.name);
    else buf[0] = 0;
    return buf;
}
static float nv_time(const dai_native_api *) { return g_native_time; }
static int nv_key(const dai_native_api *, uint32_t key) {
    return g_win_for_scripts ? dai_window_key_down(g_win_for_scripts, key) : 0;
}

// The inspector's fields, for a C++ behaviour. They are stored on the NODE in
// the same "path{key=value,...}" form the .js behaviours use, so this reads
// the same string the JS path reads - one storage, two readers.
static std::string nv_param_raw(dai_nentity e, const char *name) {
    if (!g_scene_doc || !name) return std::string();
    dai_node_desc r{};
    if (dai_doc_get(g_scene_doc, (dai_node)e, &r) != DAI_OK) return std::string();
    std::string all = r.script;
    // entry{k=v,k=v};entry{...} - find any brace group holding this key.
    size_t pos = 0;
    while (pos < all.size()) {
        size_t br = all.find('{', pos);
        if (br == std::string::npos) break;
        size_t en = all.find('}', br);
        if (en == std::string::npos) break;
        std::string inner = all.substr(br + 1, en - br - 1);
        size_t p2 = 0;
        while (p2 < inner.size()) {
            size_t comma = inner.find(',', p2);
            std::string kv = inner.substr(p2, comma == std::string::npos ? std::string::npos : comma - p2);
            size_t eq = kv.find('=');
            if (eq != std::string::npos && kv.substr(0, eq) == name) return kv.substr(eq + 1);
            if (comma == std::string::npos) break;
            p2 = comma + 1;
        }
        pos = en + 1;
    }
    return std::string();
}
static double nv_param_num(const dai_native_api *, dai_nentity e, const char *name, double fb) {
    std::string v = nv_param_raw(e, name);
    if (v.empty()) return fb;
    if (v == "true") return 1.0;
    if (v == "false") return 0.0;
    return std::atof(v.c_str());
}
static const char *nv_param_str(const dai_native_api *, dai_nentity e, const char *name,
                                const char *fb) {
    static std::string held;      // valid until the next call, like name_of
    held = nv_param_raw(e, name);
    return held.empty() ? (fb ? fb : "") : held.c_str();
}
static dai_nentity nv_param_node(const dai_native_api *, dai_nentity e, const char *name) {
    // A node reference is stored as the object's NAME, the same as in JS.
    std::string v = nv_param_raw(e, name);
    if (v.empty() || !g_scene_doc) return 0;
    return (dai_nentity)dai_doc_find(g_scene_doc, v.c_str());
}
static void nv_mouse(const dai_native_api *, float *dx, float *dy, int *buttons) {
    if (dx) *dx = (float)g_mouse_dx;
    if (dy) *dy = (float)g_mouse_dy;
    if (buttons) *buttons = g_mouse_buttons;
}

// The component bridge, C++ side. Literally the same four functions the
// JavaScript prelude reaches - one table, one meaning for "light.intensity".
static double nv_get_num(const dai_native_api *, dai_nentity e, const char *prop, double fb) {
    return comp_get_num((dai_node)e, prop, fb);
}
static void nv_set_num(const dai_native_api *, dai_nentity e, const char *prop, double v) {
    comp_set_num((dai_node)e, prop, v);
}
static dai_nvec3 nv_get_vec(const dai_native_api *, dai_nentity e, const char *prop) {
    double xyz[3] = { 0, 0, 0 };
    comp_get_vec((dai_node)e, prop, xyz);
    dai_nvec3 r{ (float)xyz[0], (float)xyz[1], (float)xyz[2] };
    return r;
}
static void nv_set_vec(const dai_native_api *, dai_nentity e, const char *prop, dai_nvec3 v) {
    double xyz[3] = { v.x, v.y, v.z };
    comp_set_vec((dai_node)e, prop, xyz);
}
static const char *nv_get_str(const dai_native_api *, dai_nentity e, const char *prop) {
    return comp_get_str((dai_node)e, prop);
}
static void nv_set_str(const dai_native_api *, dai_nentity e, const char *prop, const char *v) {
    comp_set_str((dai_node)e, prop, v);
}

static dai_native_api g_native_api = {
    DAI_NATIVE_ABI, nullptr,
    nv_log, nv_get_pos, nv_set_pos, nv_get_vel, nv_set_vel, nv_impulse,
    nv_get_scale, nv_set_scale, nv_get_rot, nv_set_rot,
    nv_find, nv_name_of, nv_time, nv_key,
    nv_param_num, nv_param_str, nv_param_node, nv_mouse,
    nv_get_num, nv_set_num, nv_get_vec, nv_set_vec, nv_get_str, nv_set_str
};

struct RunningNative { int id; dai_node node; std::string path; };
static std::vector<RunningNative> g_natives;

static bool is_cpp_script(const std::string &p) {
    size_t dot = p.find_last_of('.');
    if (dot == std::string::npos) return false;
    std::string e = p.substr(dot + 1);
    for (char &c : e) c = (char)std::tolower((unsigned char)c);
    return e == "cpp" || e == "cc" || e == "cxx";
}

// What the inspector draws for a behaviour, reported as "type:name=default"
// entries separated by commas, with an optional "|description" on the end:
//
//     let walkSpeed = 5           ->  float:walkSpeed=5
//     let _timer = 0              ->  nothing: a leading _ means private
//     // @param float speed = 6   ->  float:speed=6
//     // @header Movement         ->  header:Movement=
//
// Two ways in, because there are two kinds of day. A top level declaration in
// a .js IS the field - that is Unity's rule for a public member, and it means
// a script with no comments in it still has an inspector. The "// @param"
// line stays for the cases a declaration cannot express: a node reference has
// no literal to infer a type from, and a .cpp behaviour's members are not
// where an editor can see them.
//
// Headers and descriptions follow Unity too: "// @header X" or the C# form
// "// [Header("X")]" groups what comes after it, and "// @tooltip X",
// "// [Tooltip("X")]" or simply a comment line directly above a field is the
// text shown when the pointer rests on that row.
static void params_emit(char *buf, size_t n, size_t *used, const char *one) {
    size_t kl = std::strlen(one);
    if (*used + kl + 2 >= n) return;
    if (*used) buf[(*used)++] = ',';
    std::memcpy(buf + *used, one, kl);
    *used += kl;
    buf[*used] = 0;
}

// To the end of the line, trimmed, with the four characters this report uses
// as separators turned into spaces - a description may not redraw the format.
static void params_text(const char *e, char *out, int cap) {
    while (*e == ' ' || *e == '\t') ++e;
    int i = 0;
    while (*e && *e != '\n' && *e != '\r' && i < cap - 1) {
        char c = *e++;
        if (c == ',' || c == '|' || c == ':' || c == '=') c = ' ';
        out[i++] = c;
    }
    while (i > 0 && (out[i - 1] == ' ' || out[i - 1] == '\t')) --i;
    out[i] = 0;
}

// The text inside the first pair of quotes, for the C# attribute forms.
static bool params_quoted(const char *line, char *out, int cap) {
    const char *a = std::strchr(line, '"');
    if (!a) return false;
    const char *b = std::strchr(a + 1, '"');
    if (!b) return false;
    int i = 0;
    for (const char *c = a + 1; c < b && i < cap - 1; ++c)
        out[i++] = (*c == ',' || *c == '|' || *c == ':' || *c == '=') ? ' ' : *c;
    out[i] = 0;
    return i > 0;
}

static bool params_ident_ok(const char *s) {
    if (!s || !*s) return false;
    if (!((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') || *s == '_')) return false;
    for (const char *c = s; *c; ++c)
        if (!((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') ||
              (*c >= '0' && *c <= '9') || *c == '_')) return false;
    return true;
}

static void script_params_of(const char *path, char *buf, size_t n, void *) {
    if (!buf || !n) return;
    buf[0] = 0;
    if (!path || !g_assets_dir[0]) return;
    char full[640];
    std::snprintf(full, sizeof(full), "%s/%s", g_assets_dir, path);
    FILE *f = std::fopen(full, "rb");
    if (!f) return;
    bool js = !is_cpp_script(path);
    char line[512];
    size_t used = 0;
    char pend_tip[160] = { 0 };
    std::vector<std::string> seen;      // one field, however it was declared
    while (std::fgets(line, sizeof(line), f)) {
        // ---- headers ------------------------------------------------------
        const char *h = std::strstr(line, "@header");
        char txt[160];
        if (!h && std::strstr(line, "[Header(") && params_quoted(line, txt, sizeof(txt))) {
            char one[200];
            std::snprintf(one, sizeof(one), "header:%s=", txt);
            params_emit(buf, n, &used, one);
            pend_tip[0] = 0;
            continue;
        }
        if (h) {
            params_text(h + 7, txt, sizeof(txt));
            if (txt[0]) {
                char one[200];
                std::snprintf(one, sizeof(one), "header:%s=", txt);
                params_emit(buf, n, &used, one);
            }
            pend_tip[0] = 0;
            continue;
        }
        // ---- descriptions --------------------------------------------------
        if (std::strstr(line, "[Tooltip(") && params_quoted(line, txt, sizeof(txt))) {
            std::snprintf(pend_tip, sizeof(pend_tip), "%s", txt);
            continue;
        }
        const char *t = std::strstr(line, "@tooltip");
        int tskip = 8;
        if (!t) { t = std::strstr(line, "@desc"); tskip = 5; }
        if (t) {
            params_text(t + tskip, pend_tip, sizeof(pend_tip));
            continue;
        }
        // ---- "// @param [type] name [= default]" ----------------------------
        const char *p = std::strstr(line, "// @param");
        if (p) {
            p += 9;
            auto word = [&p](char *out, int cap) {
                while (*p == ' ' || *p == '\t') ++p;
                int i = 0;
                while ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
                       (*p >= '0' && *p <= '9') || *p == '_' || *p == '-') {
                    if (i < cap - 1) out[i++] = *p;
                    ++p;
                }
                out[i] = 0;
                return i;
            };
            char first[64], second[64];
            if (!word(first, sizeof(first))) { pend_tip[0] = 0; continue; }
            const char *type = "node";
            char *key = first;
            // Two words means the first was a type. One word is a node
            // reference, which is what this line meant before types existed.
            if (word(second, sizeof(second))) {
                // Unity's rule: the DECLARED TYPE of a reference decides what
                // the picker offers and what a drag is allowed to drop. There
                // the type is a class (Transform, Rigidbody, Camera); here it
                // is the component a node carries, which is the same question
                // asked of a different object model.
                static const char *TYPES[] = { "float", "number", "int", "bool", "string", "text",
                                               "node", "object", "transform", "camera", "light",
                                               "rigidbody", "body", "collider", "sprite", "audio",
                                               "mesh" };
                for (const char *ty : TYPES)
                    if (std::strcmp(first, ty) == 0) { type = first; break; }
                key = second;
            }
            char def[96] = { 0 };
            {
                const char *ee = std::strchr(p, '=');
                if (ee) params_text(ee + 1, def, sizeof(def));
            }
            bool dup = false;
            for (const std::string &sname : seen) if (sname == key) dup = true;
            if (!dup) {
                seen.push_back(key);
                char one[400];
                if (pend_tip[0])
                    std::snprintf(one, sizeof(one), "%s:%s=%s|%s", type, key, def, pend_tip);
                else
                    std::snprintf(one, sizeof(one), "%s:%s=%s", type, key, def);
                params_emit(buf, n, &used, one);
            }
            pend_tip[0] = 0;
            continue;
        }
        // ---- a plain comment line is the next field's description ----------
        {
            const char *c = line;
            while (*c == ' ' || *c == '\t') ++c;
            if (c[0] == '/' && c[1] == '/') {
                params_text(c + 2, pend_tip, sizeof(pend_tip));
                continue;
            }
            if (!*c || *c == '\n' || *c == '\r') { pend_tip[0] = 0; continue; }
        }
        // ---- a top level declaration IS a field ----------------------------
        // Column zero only: an indented "let" is a local inside a function,
        // and a behaviour whose loop counters showed up in the inspector
        // would be worse than no inspector at all.
        if (!js) { pend_tip[0] = 0; continue; }
        const char *c = line;
        int kw = 0;
        if (!std::strncmp(c, "let ", 4))        kw = 4;
        else if (!std::strncmp(c, "var ", 4))   kw = 4;
        else if (!std::strncmp(c, "const ", 6)) kw = 6;
        if (!kw) { pend_tip[0] = 0; continue; }
        c += kw;
        while (*c == ' ' || *c == '\t') ++c;
        char name[64];
        int i = 0;
        while (((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') ||
                (*c >= '0' && *c <= '9') || *c == '_') && i < (int)sizeof(name) - 1)
            name[i++] = *c++;
        name[i] = 0;
        while (*c == ' ' || *c == '\t') ++c;
        // A leading underscore is JavaScript's word for private, and a
        // behaviour needs somewhere to keep its own counters that is not the
        // inspector. `_t` is bookkeeping; `speed` is a field.
        if (!i || name[0] == '_' || *c != '=') { pend_tip[0] = 0; continue; }
        ++c;
        while (*c == ' ' || *c == '\t') ++c;
        // The literal, and only a literal: an expression has no value the
        // editor could show and no type it could guess.
        char val[128] = { 0 };
        const char *type = nullptr;
        if (*c == '"' || *c == '\'') {
            char q = *c++;
            int j = 0;
            while (*c && *c != q && j < (int)sizeof(val) - 1) {
                char ch = *c++;
                val[j++] = (ch == ',' || ch == '|' || ch == ':' || ch == '=') ? ' ' : ch;
            }
            val[j] = 0;
            type = "string";
        } else if (!std::strncmp(c, "true", 4) || !std::strncmp(c, "false", 5)) {
            std::snprintf(val, sizeof(val), "%s", *c == 't' ? "true" : "false");
            type = "bool";
        } else if ((*c >= '0' && *c <= '9') || *c == '-' || *c == '+' || *c == '.') {
            int j = 0, dots = 0;
            bool ok = true;
            const char *q = c;
            if (*q == '-' || *q == '+') val[j++] = *q++;
            while (*q && j < (int)sizeof(val) - 1) {
                if (*q >= '0' && *q <= '9') val[j++] = *q++;
                else if (*q == '.') { if (++dots > 1) { ok = false; break; } val[j++] = *q++; }
                else break;
            }
            val[j] = 0;
            // Trailing junk on the number ("5e3", "5px") is not a number.
            while (*q == ' ' || *q == '\t') ++q;
            if (*q && *q != ';' && *q != '/' && *q != '\n' && *q != '\r') ok = false;
            if (ok && j) type = dots ? "float" : "int";
        }
        if (!type) { pend_tip[0] = 0; continue; }
        bool dup = false;
        for (const std::string &sname : seen) if (sname == name) dup = true;
        if (!dup) {
            seen.push_back(name);
            char one[400];
            if (pend_tip[0])
                std::snprintf(one, sizeof(one), "%s:%s=%s|%s", type, name, val, pend_tip);
            else
                std::snprintf(one, sizeof(one), "%s:%s=%s", type, name, val);
            params_emit(buf, n, &used, one);
        }
        pend_tip[0] = 0;
    }
    std::fclose(f);
}

// The JS side of what the C++ behaviours already had: a keyboard, a body, and
// the node the script is attached to. Without these a .js could read the scene
// and move a transform by hand, which is not a player controller - it is a
// cutscene.
static int sp_key(const char *name, void *) {
    if (!g_win_for_scripts || !name || !*name) return 0;
    std::string n;
    for (const char *c = name; *c; ++c) n += (char)std::tolower((unsigned char)*c);
    uint32_t code = 0;
    if (n.size() == 1) {
        char c = n[0];
        if (c >= 'a' && c <= 'z') code = (uint32_t)c;
        else if (c >= '0' && c <= '9') code = (uint32_t)c;
    }
    if (!code) {
        if (n == "space")  code = DAI_KEY_SPACE;
        else if (n == "enter" || n == "return") code = DAI_KEY_RETURN;
        else if (n == "escape") code = DAI_KEY_ESCAPE;
        else if (n == "tab")   code = DAI_KEY_TAB;
        else if (n == "left")  code = DAI_KEY_LEFT;
        else if (n == "right") code = DAI_KEY_RIGHT;
        else if (n == "up")    code = DAI_KEY_UP;
        else if (n == "down")  code = DAI_KEY_DOWN;
        else if (n == "shift")
            return dai_window_key_down(g_win_for_scripts, DAI_KEY_SHIFT_L) ||
                   dai_window_key_down(g_win_for_scripts, DAI_KEY_SHIFT_R);
        else if (n == "ctrl" || n == "control")
            return dai_window_key_down(g_win_for_scripts, DAI_KEY_CTRL_L) ||
                   dai_window_key_down(g_win_for_scripts, DAI_KEY_CTRL_R);
        else if (n == "alt")
            return dai_window_key_down(g_win_for_scripts, DAI_KEY_ALT_L) ||
                   dai_window_key_down(g_win_for_scripts, DAI_KEY_ALT_R);
    }
    if (!code) return 0;                    // an unknown name is false, not an error
    return dai_window_key_down(g_win_for_scripts, code) ? 1 : 0;
}

static int sp_get_vel(double id, double *xyz, void *) {
    dai_vec3 l{}, a{};
    if (!g_script_ed || !xyz) return 0;
    dai_editor_live_velocity(g_script_ed, (dai_node)(uint32_t)id, &l, &a);
    xyz[0] = l.x; xyz[1] = l.y; xyz[2] = l.z;
    return 1;
}

static void sp_set_vel(double id, const double *xyz, void *) {
    if (!g_script_ed || !xyz) return;
    dai_editor_live_set_velocity(g_script_ed, (dai_node)(uint32_t)id,
                                 dai_vec3{ (float)xyz[0], (float)xyz[1], (float)xyz[2] });
}

static void sp_impulse(double id, const double *xyz, void *) {
    if (!g_script_ed || !xyz) return;
    dai_editor_live_impulse(g_script_ed, (dai_node)(uint32_t)id,
                            dai_vec3{ (float)xyz[0], (float)xyz[1], (float)xyz[2] });
}

// "Is there floor under me." Not a raycast: the engine has no query API bound
// here, and a controller only needs to know whether it is falling. A body that
// is neither rising nor sinking measurably is standing on something - which is
// exactly the test a platformer wants, and it costs nothing.
static int sp_grounded(double id, void *) {
    dai_vec3 l{}, a{};
    if (!g_script_ed) return 0;
    dai_editor_live_velocity(g_script_ed, (dai_node)(uint32_t)id, &l, &a);
    return (l.y > -0.35f && l.y < 0.35f) ? 1 : 0;
}

// Mouse movement since the last frame: declared next to g_win_for_scripts,
// because the native API table is built before this point.
static void sp_mouse(double *dx, double *dy, int *buttons, void *) {
    if (dx) *dx = g_mouse_dx;
    if (dy) *dy = g_mouse_dy;
    if (buttons) *buttons = g_mouse_buttons;
}

static dai_script_play_host g_play_host = {
    sp_key, sp_get_vel, sp_set_vel, sp_impulse, sp_grounded, sp_mouse, nullptr
};

// The node is part of it now: a button click has to reach the script ON
// that button, and "which script belongs to which object" was the one
// thing this list did not know.
struct RunningScript { dai_script *s = nullptr; std::string path; dai_node node = DAI_INVALID_NODE; };
static std::vector<RunningScript> g_running;
static int g_scripts_live = 0;

// ---- gui: what the scripts asked to be drawn this frame -----------------
//
// COLLECTED, not drawn on the spot. frame() runs while the world is being
// stepped, long before the UI pass; drawing from there would put a label in
// the middle of the scene geometry or, worse, into last frame's vertex
// buffer. So the calls land in a list and the list is played back in one
// place, which is also what makes the coordinates mean the same thing every
// time.
struct GuiCmd {
    int kind;                 // 0 text, 1 rect, 2 image, 3 button
    float x, y, w, h;
    float size;
    uint32_t color;
    std::string text;
};
static std::vector<GuiCmd> g_gui_cmds;
static float g_gui_x = 0, g_gui_y = 0, g_gui_w = 0, g_gui_h = 0;   // the view
static float g_gui_mx = 0, g_gui_my = 0;
static int   g_gui_down = 0, g_gui_released = 0;

static uint32_t gui_col(double v) {
    // JavaScript numbers are doubles; 0xFFFFFFFF survives exactly, but a
    // negative or absurd value must not wrap into something opaque black.
    if (!(v >= 0.0)) return 0xFFFFFFFFu;
    if (v > 4294967295.0) return 0xFFFFFFFFu;
    return (uint32_t)v;
}

static void gui_text_cb(double x, double y, const char *t, double size, double rgba, void *) {
    GuiCmd c{}; c.kind = 0; c.x = (float)x; c.y = (float)y;
    c.size = size > 0 ? (float)size : 24.0f; c.color = gui_col(rgba); c.text = t ? t : "";
    g_gui_cmds.push_back(c);
}
static void gui_rect_cb(double x, double y, double w, double h, double rgba, void *) {
    GuiCmd c{}; c.kind = 1; c.x = (float)x; c.y = (float)y; c.w = (float)w; c.h = (float)h;
    c.color = gui_col(rgba);
    g_gui_cmds.push_back(c);
}
static void gui_image_cb(double x, double y, double w, double h, const char *path,
                         double rgba, void *) {
    GuiCmd c{}; c.kind = 2; c.x = (float)x; c.y = (float)y; c.w = (float)w; c.h = (float)h;
    c.color = gui_col(rgba); c.text = path ? path : "";
    g_gui_cmds.push_back(c);
}
static int gui_button_cb(double x, double y, double w, double h, const char *label, void *) {
    GuiCmd c{}; c.kind = 3; c.x = (float)x; c.y = (float)y; c.w = (float)w; c.h = (float)h;
    c.color = 0; c.text = label ? label : "";
    g_gui_cmds.push_back(c);
    // The ANSWER has to come now, in the same call, because the script uses it
    // in an if. So the hit test runs against last frame's pointer state - one
    // frame of latency on a click, which nobody can feel, in exchange for an
    // API that reads like every immediate mode UI ever written.
    float px = g_gui_x + (float)x, py = g_gui_y + (float)y;
    bool over = g_gui_mx >= px && g_gui_mx < px + (float)w &&
                g_gui_my >= py && g_gui_my < py + (float)h;
    return (over && g_gui_released) ? 1 : 0;
}
static void gui_size_cb(double *w, double *h, void *) {
    if (w) *w = g_gui_w;
    if (h) *h = g_gui_h;
}
static dai_script_gui_host g_gui_host = {
    gui_text_cb, gui_rect_cb, gui_image_cb, gui_button_cb, gui_size_cb, nullptr
};


static void scripts_stop() {
    g_gui_cmds.clear();     // or the last frame's menu stays on the screen
    for (RunningScript &r : g_running) dai_script_destroy(r.s);
    g_running.clear();
    // Unloading the libraries on Stop is what makes editing a .cpp and
    // pressing Play again pick up the change: Windows will not replace a DLL
    // that is still mapped.
    if (g_native) for (const RunningNative &r : g_natives) dai_native_unload(g_native, r.id);
    g_natives.clear();
    g_native_time = 0.0f;
}

static void scripts_start() {
    scripts_stop();
    if (!g_scene_doc) return;
    uint32_t n = dai_doc_count(g_scene_doc);
    std::vector<dai_node> all(n);
    if (n) dai_doc_nodes(g_scene_doc, all.data(), n);
    char err[256];
    for (dai_node id : all) {
        dai_node_desc r{};
        if (dai_doc_get(g_scene_doc, id, &r) != DAI_OK || !r.script[0]) continue;
        // entries: "a.js{target=Box};b.js" - path plus its assigned references
        std::string cur;
        std::vector<std::string> entries;
        for (const char *c = r.script; ; ++c) {
            if (*c == ';' || !*c) {
                if (!cur.empty()) entries.push_back(cur);
                cur.clear();
                if (!*c) break;
            } else cur += *c;
        }
        for (const std::string &entry : entries) {
            std::string path = entry, params_js, assign_js;
            size_t b = entry.find('{');
            if (b != std::string::npos) {
                path = entry.substr(0, b);
                size_t en = entry.find('}', b);
                std::string inner = entry.substr(b + 1, en == std::string::npos ? en : en - b - 1);
                params_js = "var params = {";
                size_t pos = 0;
                while (pos < inner.size()) {
                    size_t comma = inner.find(',', pos);
                    std::string kv = inner.substr(pos, comma == std::string::npos ? comma : comma - pos);
                    size_t eq = kv.find('=');
                    if (eq != std::string::npos && eq > 0) {
                        std::string k = kv.substr(0, eq), v = kv.substr(eq + 1);
                        // This line used to have its quote escapes eaten:
                        //     params_js += """ + kv.substr(0, eq) + ...
                        // which C++ happily reads as ONE adjacent-literal
                        // string, so every script got the TEXT
                        // "+ kv.substr(0, eq) +:..." instead of its values,
                        // the eval failed, and `params` was undefined in every
                        // behaviour. Nothing said so - the error from
                        // dai_script_eval was never looked at.
                        //
                        // While it is being written properly: a number stays a
                        // number and a bool stays a bool. Quoting everything
                        // makes params.speed * dt string arithmetic, and in JS
                        // that is a silent NaN.
                        bool numeric = !v.empty();
                        int dots = 0;
                        for (size_t ci = 0; ci < v.size(); ++ci) {
                            char c = v[ci];
                            if (c == '.') { if (++dots > 1) { numeric = false; break; } }
                            else if (c == '-' || c == '+') { if (ci) { numeric = false; break; } }
                            else if (c < '0' || c > '9') { numeric = false; break; }
                        }
                        std::string lit = (v == "true" || v == "false" || numeric)
                                        ? v : ("\"" + v + "\"");
                        params_js += "\"" + k + "\":" + lit + ",";
                        // ...and onto the declaration itself. A script that
                        // says `let walkSpeed = 5` at the top has just told
                        // the inspector its default; the value the user typed
                        // has to land in THAT variable, or the field is a
                        // display that changes nothing. A const cannot be
                        // assigned - that throws, and a throw here would take
                        // the rest of the fields with it.
                        if (params_ident_ok(k.c_str()))
                            assign_js += "try{" + k + "=" + lit + ";}catch(e){}";
                    }
                    if (comma == std::string::npos) break;
                    pos = comma + 1;
                }
                params_js += "};";
            }
            char full[640];
            std::snprintf(full, sizeof(full), "%s/%s", g_assets_dir, path.c_str());

            // A .cpp goes down the native path: compile, load, init. Errors
            // are the compiler's own text, in the console, where a user can
            // read them.
            if (is_cpp_script(path)) {
                if (!g_native) {
                    char cache[700];
                    std::snprintf(cache, sizeof(cache), "%s/.native", g_assets_dir);
                    g_native = dai_native_create(cache);
                }
                char nerr[1024] = { 0 };
                char incdir[700];
                std::snprintf(incdir, sizeof(incdir), "%s/../include", g_assets_dir);
                int nid = dai_native_load(g_native, full, incdir, nerr, sizeof(nerr));
                if (nid < 0) {
                    std::printf("cpp %s: %s\n", path.c_str(), nerr);
                    if (g_panels_for_log) {
                        char line[1200];
                        std::snprintf(line, sizeof(line), "%s: %s", path.c_str(), nerr);
                        dai_editor_ui_log(g_panels_for_log, 2, line);
                    }
                } else {
                    dai_native_init(g_native, nid, &g_native_api, (dai_nentity)(uint32_t)id);
                    g_natives.push_back({ nid, id, path });
                }
                continue;
            }

            dai_script *s = dai_script_create(err, sizeof(err));
            if (!s) {
                std::printf("script: %s\n", err);
                if (g_panels_for_log) dai_editor_ui_log(g_panels_for_log, 2, err);
                continue;
            }
            dai_script_bind_nodes(s, &g_node_host);
            dai_script_bind_play(s, &g_play_host);
            dai_script_bind_gui(s, &g_gui_host);
            if (dai_script_load(s, full, err, sizeof(err)) != DAI_OK) {
                std::printf("script %s: %s\n", path.c_str(), err);
                if (g_panels_for_log) {
                    char line[400];
                    std::snprintf(line, sizeof(line), "%s: %s", path.c_str(), err);
                    dai_editor_ui_log(g_panels_for_log, 2, line);
                }
                dai_script_destroy(s);
                continue;
            }
            // Which object am I on. Unity calls it gameObject, Godot calls it
            // self; either way it is the first thing a behaviour needs and the
            // only one it cannot look up.
            // The object model, then `self` as one of its Nodes. In this
            // order: the wrapper has to exist before anything is wrapped.
            {
                if (dai_script_eval(s, DAI_JS_PRELUDE, "prelude", err, sizeof(err)) != DAI_OK && err[0])
                    std::printf("prelude: %s\n", err);
                char selfjs[96];
                std::snprintf(selfjs, sizeof(selfjs),
                              "var self = __wrapSelf(%u);", (unsigned)id);
                dai_script_eval(s, selfjs, "self", err, sizeof(err));
            }
            if (!params_js.empty()) dai_script_eval(s, params_js.c_str(), "params", err, sizeof(err));
            // After the file has run, so it overwrites the defaults the
            // declarations just installed, and before init(), so the first
            // frame already sees the inspector's numbers.
            if (!assign_js.empty()) dai_script_eval(s, assign_js.c_str(), "fields", err, sizeof(err));
            dai_script_call(s, "init", err, sizeof(err));
            g_running.push_back({ s, path, id });
        }
    }
    if (!g_natives.empty()) {
        char line[96];
        std::snprintf(line, sizeof(line), "play: %u C++ behaviour(s) running",
                      (unsigned)g_natives.size());
        std::printf("%s\n", line);
        if (g_panels_for_log) dai_editor_ui_log(g_panels_for_log, 0, line);
    }
    if (!g_running.empty()) {
        std::printf("scripts: %u running\n", (unsigned)g_running.size());
        if (g_panels_for_log) {
            char line[96];
            std::snprintf(line, sizeof(line), "play: %u script(s) running", (unsigned)g_running.size());
            dai_editor_ui_log(g_panels_for_log, 0, line);
        }
    }
}
#endif // DAI_WITH_SCRIPT

// "Create Prefab": the node becomes a file under <assets>/prefabs/, which the
// browser then lists like any other asset and the document layer can
// instantiate again.
static dai_doc *g_prefab_doc = nullptr;

// Double click on a file in the Project window: hand it to the machine's
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
    // Code.exe, not code.cmd: a .cmd has to go through cmd.exe, and cmd.exe
    // is a black window that flashes up (and, when the path has a space in it
    // and the quoting is one level off, prints "C:\\Users\\justi\\AppData\\Local\\
    // Programs\\Microsoft is not recognised" instead of opening anything).
    const char *CODE_PATHS[] = {
        "%LOCALAPPDATA%\\Programs\\Microsoft VS Code\\Code.exe",
        "%ProgramFiles%\\Microsoft VS Code\\Code.exe",
        "%ProgramFiles(x86)%\\Microsoft VS Code\\Code.exe",
        "%LOCALAPPDATA%\\Programs\\Microsoft VS Code Insiders\\Code - Insiders.exe",
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
            std::snprintf(cmd2, sizeof(cmd2), "\"%s\" \"%s\" --goto \"%s\"",
                          path, g_assets_dir, full);
        else
            std::snprintf(cmd2, sizeof(cmd2), "\"%s\" --goto \"%s\"", path, full);
        STARTUPINFOA si{};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_SHOWNORMAL;
        PROCESS_INFORMATION pi{};
        if (CreateProcessA(nullptr, cmd2, nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                           nullptr, nullptr, &si, &pi)) {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
            std::printf("opened in VS Code: %s\n", full);
            return 1;
        }
        std::printf("could not start %s (error %lu)\n", path, (unsigned long)GetLastError());
    }
    // No VS Code anywhere: hand it to whatever the extension is registered
    // to. ShellExecute, not system() - there is no console to borrow.
    HINSTANCE rc = ShellExecuteA(nullptr, "open", full, nullptr, nullptr, SW_SHOWNORMAL);
    if ((INT_PTR)rc > 32) return 1;
    std::printf("no editor could open %s\n", full);
    return 0;
#else
    char cmd[1200];
    std::snprintf(cmd, sizeof(cmd),
                  "(command -v code >/dev/null && code --goto '%s') || xdg-open '%s' &", full, full);
    return std::system(cmd) == 0 ? 1 : 0;
#endif
}

static int prefab_save_cb(const char *node_id, const char *rel_path, void *) {
    if (!g_prefab_doc || !node_id || !rel_path || !g_assets_dir[0]) return 0;
    dai_node n = (dai_node)strtoul(node_id, nullptr, 10);
    char path[700];
    std::snprintf(path, sizeof(path), "%s/%s", g_assets_dir, rel_path);
    // Create the folder the prefab is going INTO, not a fixed "prefabs" one.
    // Dropping an object on a subfolder of the project window wrote to a
    // directory that did not exist, the save failed, and the editor said
    // nothing - which is what "I still cannot make prefabs" was.
    {
        char dir[700];
        std::snprintf(dir, sizeof(dir), "%s", path);
        char *slash = std::strrchr(dir, '/');
        if (slash) {
            *slash = 0;
            // every missing level, not just the last
            for (char *c = dir + 1; *c; ++c) {
                if (*c != '/') continue;
                *c = 0;
#ifdef _WIN32
                CreateDirectoryA(dir, nullptr);
#else
                mkdir(dir, 0755);
#endif
                *c = '/';
            }
#ifdef _WIN32
            CreateDirectoryA(dir, nullptr);
#else
            mkdir(dir, 0755);
#endif
        }
    }
    dai_result rc = dai_doc_prefab_save(g_prefab_doc, n, path);
    if (rc != DAI_OK) {
        char msg[800];
        std::snprintf(msg, sizeof(msg), "prefab save failed: %s", path);
        if (g_panels_for_log) dai_editor_ui_log(g_panels_for_log, 2, msg);
        std::printf("%s\n", msg);
        return 0;
    }
    // The object it was made FROM becomes an instance of it. That is what
    // Unity does and it is the only version that is any use: without it you
    // get a file nothing points at, editing the prefab leaves the original
    // untouched, and the scene has a silent duplicate of everything. The link
    // is a path relative to the assets root - the same string the browser and
    // dai_doc_prefab_reload use, so a reload finds it.
    {
        dai_node_desc rec{};
        if (dai_doc_get(g_prefab_doc, n, &rec) == DAI_OK) {
            dai_doc_begin(g_prefab_doc, "Create prefab");
            std::snprintf(rec.prefab, sizeof(rec.prefab), "%s", rel_path);
            dai_doc_set(g_prefab_doc, n, &rec);
            dai_doc_commit(g_prefab_doc);
        }
    }
    return 1;
}

// The inline rename of the Project window: asset-relative paths, same rules
// as script creation (never escape the assets folder).
// Every directory under the assets root, recursively, as '/'-separated
// relative paths. The Project browser derives its tree from FILE paths, so
// without this feed an empty folder is invisible - "New Folder" vanished
// the moment it was made, and a renamed folder went with it.
// Plain files in one directory, names only. The recursive folder walk next to
// this one answers a different question and answering both with one function
// would mean a flag argument that every caller has to look up.
static void list_dir_files(const std::string &abs, std::vector<std::string> &out) {
#ifdef _WIN32
    std::string pat = abs + "\\*";
    WIN32_FIND_DATAA fd{};
    HANDLE h = FindFirstFileA(pat.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (fd.cFileName[0] == '.') continue;
        out.push_back(fd.cFileName);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR *dp = opendir(abs.c_str());
    if (!dp) return;
    while (dirent *de = readdir(dp)) {
        if (de->d_name[0] == '.') continue;
        std::string sub = abs + "/" + de->d_name;
        struct stat sb{};
        if (stat(sub.c_str(), &sb) != 0 || !S_ISREG(sb.st_mode)) continue;
        out.push_back(de->d_name);
    }
    closedir(dp);
#endif
    std::sort(out.begin(), out.end());
}

static void list_dirs_rec(const std::string &abs, const std::string &rel,
                          char (*out)[160], uint32_t *n, uint32_t max, int depth) {
    if (depth > 8 || *n >= max) return;
#ifdef _WIN32
    WIN32_FIND_DATAA fd;
    std::string pat = abs + "/*";
    HANDLE h = FindFirstFileA(pat.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (fd.cFileName[0] == '.') continue;   // . and .. and dotfiles
        std::string r = rel.empty() ? fd.cFileName : rel + "/" + fd.cFileName;
        std::snprintf(out[*n], 160, "%s", r.c_str()); ++*n;
        list_dirs_rec(abs + "/" + fd.cFileName, r, out, n, max, depth + 1);
    } while (FindNextFileA(h, &fd) && *n < max);
    FindClose(h);
#else
    DIR *dp = opendir(abs.c_str());
    if (!dp) return;
    while (dirent *de = readdir(dp)) {
        if (de->d_name[0] == '.') continue;
        std::string sub = abs + "/" + de->d_name;
        struct stat sb{};
        if (stat(sub.c_str(), &sb) != 0 || !S_ISDIR(sb.st_mode)) continue;
        std::string r = rel.empty() ? de->d_name : rel + "/" + de->d_name;
        std::snprintf(out[*n], 160, "%s", r.c_str()); ++*n;
        list_dirs_rec(sub, r, out, n, max, depth + 1);
        if (*n >= max) break;
    }
    closedir(dp);
#endif
}

static int asset_rename(const char *old_rel, const char *new_rel, void *) {
    if (!old_rel || !new_rel || !*old_rel || !*new_rel || !g_assets_dir[0]) return 0;
    // Reject only what a path genuinely cannot contain (the '/' separator
    // stays legal: new_rel is relative to the assets root). Spaces are fine -
    // "New Folder" is the DEFAULT name, blocking spaces blocked every rename.
    for (const char *c = new_rel; *c; ++c) {
        bool bad = *c == '\\' || *c == ':' || *c == '*' || *c == '?' ||
                   *c == '"' || *c == '<' || *c == '>' || *c == '|';
        if (bad) return 0;
    }
    if (std::strstr(new_rel, "..")) return 0;
    char a[640], b[640];
    std::snprintf(a, sizeof(a), "%s/%s", g_assets_dir, old_rel);
    std::snprintf(b, sizeof(b), "%s/%s", g_assets_dir, new_rel);
    return std::rename(a, b) == 0 ? 1 : 0;
}

// "New Script" writes a template, because the first script anyone writes
// should not start with guessing what the entry point is called.
// Creating inside a SUBFOLDER needs the folders to actually exist: fopen
// will not make them, and "Create: Script" inside models/props silently
// failed - which is exactly what "scripts cannot be created in folders" was.
static void make_parent_dirs(const char *abs_path) {
    char dir[700];
    std::snprintf(dir, sizeof(dir), "%s", abs_path);
    char *slash = std::strrchr(dir, '/');
    if (!slash) return;
    *slash = 0;
    for (char *c = dir + 1; *c; ++c) {
        if (*c != '/') continue;
        *c = 0;
#ifdef _WIN32
        CreateDirectoryA(dir, nullptr);
#else
        mkdir(dir, 0755);
#endif
        *c = '/';
    }
#ifdef _WIN32
    CreateDirectoryA(dir, nullptr);
#else
    mkdir(dir, 0755);
#endif
}

// ---- importing what the desktop dropped -----------------------------------
// The editor decided WHERE (the folder on screen) and WHAT (the paths the
// window system handed over). This end owns the disk, so it does the copying.

// mingw's <sys/stat.h> declares `struct stat` without the C++ courtesy of
// keeping the FUNCTION stat() visible under the same name, and leaves S_ISDIR
// out entirely - so this cannot be written once for both. Asking each platform
// in its own words is shorter than fighting it, and the Win32 answer is the
// cheaper call anyway.
static int is_dir_path(const char *p) {
    if (!p || !*p) return 0;
#ifdef _WIN32
    DWORD a = GetFileAttributesA(p);
    return (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY)) ? 1 : 0;
#else
    struct stat st;
    if (::stat(p, &st) != 0) return 0;
    return S_ISDIR(st.st_mode) ? 1 : 0;
#endif
}

static int path_exists(const char *p) {
    if (!p || !*p) return 0;
#ifdef _WIN32
    return GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES ? 1 : 0;
#else
    struct stat st;
    return ::stat(p, &st) == 0 ? 1 : 0;
#endif
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

// Deleting an asset - a file, or a folder and everything under it. The editor
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

// The built-in script editor's two file operations. Same rules as everything
// else that touches the assets folder: relative, never upwards, never absolute.
static uint32_t asset_read_text(const char *rel, char *out, uint32_t max, void *) {
    if (!rel || !out || max < 2 || !g_assets_dir[0]) return 0;
    out[0] = 0;
    if (std::strstr(rel, "..") || rel[0] == '/' || rel[0] == '\\') return 0;
    char full[700];
    std::snprintf(full, sizeof(full), "%s/%s", g_assets_dir, rel);
    FILE *f = std::fopen(full, "rb");
    if (!f) return 0;
    size_t n = std::fread(out, 1, max - 1, f);
    std::fclose(f);
    // Editors work in '\n'. A file written on Windows arrives with '\r\n' and
    // every one of those carriage returns would otherwise be a visible glyph
    // at the end of every line.
    size_t w = 0;
    for (size_t i = 0; i < n; ++i) if (out[i] != '\r') out[w++] = out[i];
    out[w] = 0;
    return (uint32_t)w;
}

// ---- asset thumbnails ---------------------------------------------------
//
// The Project window asks "what does this asset look like" once per visible
// row per frame, so the answer has to be a lookup. Making one costs a file
// read and a few thousand triangles through dai_thumb, which is nothing on
// its own and quite a lot fifty times in the frame a folder is opened - so
// at most ONE new picture is made per frame and the rest arrive over the next
// second. A thumbnail that appears a few frames late is not a bug; a browser
// that stutters when you open a folder is.
//
// A path that produced nothing is cached as 0 as well: without that, every
// frame retries every .txt in the folder for ever.
static std::map<std::string, dai_texture> g_thumbs;
static dai_renderer *g_thumb_r = nullptr;
static int g_thumb_budget = 0;

static bool thumb_ext_is(const std::string &p2, const char *ext) {
    size_t n = std::strlen(ext);
    if (p2.size() <= n) return false;
    std::string tail = p2.substr(p2.size() - n);
    for (char &c : tail) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    return tail == ext;
}

// Every triangle a .glb holds, in the file's own space. The node hierarchy is
// deliberately ignored: a thumbnail wants the SHAPE of the thing, and the
// transforms in a well behaved export are the identity anyway.
static bool thumb_model_soup(const std::string &full, std::vector<float> &pos) {
    FILE *f = std::fopen(full.c_str(), "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    long len = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    // 64 MB is a lot of crate. Past that a thumbnail is not worth the stall.
    if (len <= 0 || len > 64 * 1024 * 1024) { std::fclose(f); return false; }
    std::vector<uint8_t> bytes((size_t)len);
    size_t got = std::fread(bytes.data(), 1, bytes.size(), f);
    std::fclose(f);
    if (got != bytes.size()) return false;

    dai_mesh_data prims[64]{};
    char err[256] = { 0 };
    uint32_t n = dai_gltf_read_geometry(bytes.data(), bytes.size(), prims, 64, err, sizeof(err));
    if (!n) return false;
    for (uint32_t i = 0; i < n && i < 64; ++i) {
        const dai_mesh_data &m = prims[i];
        if (!m.vertices) continue;
        if (m.indices && m.index_count) {
            for (uint32_t k = 0; k < m.index_count; ++k) {
                uint32_t vi = m.indices[k];
                if (vi >= m.vertex_count) continue;
                pos.push_back(m.vertices[vi].position.x);
                pos.push_back(m.vertices[vi].position.y);
                pos.push_back(m.vertices[vi].position.z);
            }
        } else {
            for (uint32_t k = 0; k < m.vertex_count; ++k) {
                pos.push_back(m.vertices[k].position.x);
                pos.push_back(m.vertices[k].position.y);
                pos.push_back(m.vertices[k].position.z);
            }
        }
    }
    dai_gltf_free_geometry(prims, n < 64 ? n : 64);
    return pos.size() >= 9;
}

// A prefab is a scene file: its nodes, at their own places, as the shapes they
// are. Loaded into a throwaway document - the one on screen must not notice.
static bool thumb_prefab_parts(const std::string &full, std::vector<dai_thumb_part> &parts) {
    dai_doc *pd = dai_doc_create();
    if (!pd) return false;
    char err[256] = { 0 };
    if (dai_doc_load(pd, full.c_str(), err, sizeof(err)) != DAI_OK) {
        dai_doc_destroy(pd);
        return false;
    }
    std::vector<dai_node> ids(dai_doc_count(pd) + 1);
    uint32_t n = dai_doc_nodes(pd, ids.data(), (uint32_t)ids.size());
    for (uint32_t i = 0; i < n && parts.size() < 64; ++i) {
        dai_node_desc d{};
        if (dai_doc_get(pd, ids[i], &d) != DAI_OK) continue;
        if (d.hidden || d.no_body) continue;          // empties draw nothing
        dai_vec3 wp{}, ws{ 1, 1, 1 };
        dai_quat wr{ 0, 0, 0, 1 };
        if (dai_doc_world_transform(pd, ids[i], &wp, &wr, &ws) != DAI_OK) continue;
        // The unit shapes are one unit across, so the basis carries the FULL
        // size: rotation, times scale, times twice the half extent.
        float sx = 2.0f * d.half_extent.x * ws.x;
        float sy = 2.0f * d.half_extent.y * ws.y;
        float sz = 2.0f * d.half_extent.z * ws.z;
        if (sx < 1e-4f) sx = 1e-4f;
        if (sy < 1e-4f) sy = 1e-4f;
        if (sz < 1e-4f) sz = 1e-4f;
        float x = wr.x, y = wr.y, z = wr.z, w = wr.w;
        float R[9] = {
            1 - 2 * (y * y + z * z), 2 * (x * y - z * w),     2 * (x * z + y * w),
            2 * (x * y + z * w),     1 - 2 * (x * x + z * z), 2 * (y * z - x * w),
            2 * (x * z - y * w),     2 * (y * z + x * w),     1 - 2 * (x * x + y * y),
        };
        dai_thumb_part pt{};
        pt.shape = d.shape;
        pt.xform[0] = R[0] * sx; pt.xform[1] = R[1] * sy; pt.xform[2]  = R[2] * sz; pt.xform[3]  = wp.x;
        pt.xform[4] = R[3] * sx; pt.xform[5] = R[4] * sy; pt.xform[6]  = R[5] * sz; pt.xform[7]  = wp.y;
        pt.xform[8] = R[6] * sx; pt.xform[9] = R[7] * sy; pt.xform[10] = R[8] * sz; pt.xform[11] = wp.z;
        parts.push_back(pt);
    }
    dai_doc_destroy(pd);
    return !parts.empty();
}

static dai_texture thumb_for(const char *rel, void *) {
    if (!rel || !*rel || !g_thumb_r || !g_assets_dir[0]) return 0;
    std::string key = rel;
    auto it = g_thumbs.find(key);
    if (it != g_thumbs.end()) return it->second;

    bool model  = thumb_ext_is(key, ".glb") || thumb_ext_is(key, ".gltf");
    bool prefab = thumb_ext_is(key, ".daidalos");
    if (!model && !prefab) { g_thumbs[key] = 0; return 0; }
    // One new picture per frame. Not cached as a failure - it is a "not yet".
    if (g_thumb_budget <= 0) return 0;
    --g_thumb_budget;

    char full[700];
    std::snprintf(full, sizeof(full), "%s/%s", g_assets_dir, rel);

    const uint32_t SZ = 64;
    std::vector<uint8_t> rgba((size_t)SZ * SZ * 4, 0);
    int ok = 0;
    if (model) {
        std::vector<float> pos;
        if (thumb_model_soup(full, pos)) {
            dai_thumb_mesh m{ pos.data(), (uint32_t)(pos.size() / 3), nullptr, 0 };
            ok = dai_thumb_render(&m, rgba.data(), SZ, 0xE0936C);   // the model amber
        }
    } else {
        std::vector<dai_thumb_part> parts;
        if (thumb_prefab_parts(full, parts))
            ok = dai_thumb_render_parts(parts.data(), (uint32_t)parts.size(),
                                        rgba.data(), SZ, 0x8FB6E8);
    }
    dai_texture t = 0;
    if (ok) t = dai_render_texture_create(g_thumb_r, rgba.data(), SZ, SZ, 0);
    g_thumbs[key] = t;
    return t;
}

// A file changed on disk: its picture is a lie now. Cheap enough to throw the
// whole cache away - it rebuilds one row at a time.
static void thumbs_forget_all(void) {
    for (auto &kv : g_thumbs)
        if (kv.second && g_thumb_r) dai_render_texture_destroy(g_thumb_r, kv.second);
    g_thumbs.clear();
}

static int asset_write_text(const char *rel, const char *text, void *) {
    if (!rel || !text || !g_assets_dir[0]) return 0;
    if (std::strstr(rel, "..") || rel[0] == '/' || rel[0] == '\\') return 0;
    char full[700];
    std::snprintf(full, sizeof(full), "%s/%s", g_assets_dir, rel);
    make_parent_dirs(full);
    FILE *f = std::fopen(full, "wb");
    if (!f) return 0;
    size_t n = std::strlen(text);
    size_t got = std::fwrite(text, 1, n, f);
    int ok = (std::fclose(f) == 0) && got == n;
    return ok ? 1 : 0;
}

static int asset_delete(const char *rel, const char *, void *) {
    if (!rel || !*rel || !g_assets_dir[0]) return 0;
    // Never above the assets root, never the root itself. A path containing
    // ".." or starting with a separator is not a mistake to be corrected, it
    // is a request to delete something outside the project.
    if (std::strstr(rel, "..") || rel[0] == '/' || rel[0] == '\\') return 0;
    std::string abs = std::string(g_assets_dir) + "/" + rel;
    int ok = remove_tree(abs, 0);
    if (g_panels_for_log) {
        char msg[800];
        std::snprintf(msg, sizeof(msg), ok ? "deleted %s" : "delete FAILED: %s", rel);
        dai_editor_ui_log(g_panels_for_log, ok ? 0 : 2, msg);
    }
    return ok;
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
            if (!path_exists(dst.c_str())) break;
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

// ---- what an external editor needs to know about this engine -------------
//
// VS Code is not "integrated" by embedding it - it is Electron, a second
// browser, three hundred megabytes, and it would still not know what
// body.setVel is. What makes an external editor useful is TYPES: a .d.ts
// beside the scripts and a jsconfig that points at it, and suddenly
// completion, hover docs and go-to-definition all work on the engine API,
// with every extension the user already has.
//
// Written on project open, overwritten every time, so the definitions can
// never drift behind the engine that ships them.
static const char *DAIDALOS_DTS =
"// DAIDALOS scripting API - generated by the editor, do not edit.\n"
"// Every .js in this folder is a behaviour: attach it to an object and the\n"
"// editor calls init() once and frame() every frame while the game plays.\n"
"\n"
"/** The node this script is attached to. */\n"
"declare const self: number;\n"
"/** Values the inspector stored for this script's fields. Every top level\n"
"    declaration below is one - `let speed = 6` is a field, `let _t = 0` is\n"
"    not - and the inspector writes its value straight into the variable\n"
"    before init() runs, so reading `params` by hand is never necessary. */\n"
"declare const params: { [key: string]: number | string | boolean };\n"
"/** Host values. state.dt is the length of this frame, in seconds. */\n"
"declare const state: { dt: number, [key: string]: number | string };\n"
"declare function print(msg: any): void;\n"
"\n"
"declare namespace scene {\n"
"    /** Node id by name, or -1. */\n"
"    function find(name: string): number;\n"
"}\n"
"\n"
"declare namespace node {\n"
"    function getPos(id: number): [number, number, number];\n"
"    function setPos(id: number, x: number, y: number, z: number): void;\n"
"    function getRot(id: number): [number, number, number, number];\n"
"    function setRot(id: number, x: number, y: number, z: number, w: number): void;\n"
"}\n"
"\n"
"declare namespace input {\n"
"    /** Held? \"w\", \"space\", \"shift\", \"left\", \"a\"... */\n"
"    function key(name: string): boolean;\n"
"}\n"
"\n"
"declare namespace body {\n"
"    function getVel(id: number): [number, number, number];\n"
"    function setVel(id: number, x: number, y: number, z: number): void;\n"
"    function impulse(id: number, x: number, y: number, z: number): void;\n"
"    function grounded(id: number): boolean;\n"
"}\n"
"\n"
"declare namespace ui {\n"
"    function text(x: number, y: number, msg: string): void;\n"
"    function button(label: string): boolean;\n"
"}\n";

static const char *DAIDALOS_JSCONFIG =
"{\n"
"  \"compilerOptions\": {\n"
"    \"target\": \"ES2020\",\n"
"    \"checkJs\": false,\n"
"    \"module\": \"none\"\n"
"  },\n"
"  \"include\": [\"**/*.js\", \"daidalos.d.ts\"]\n"
"}\n";

static void write_editor_support_files() {
    if (!g_assets_dir[0]) return;
    struct { const char *name; const char *body; } FILES[] = {
        { "daidalos.d.ts", DAIDALOS_DTS },
        { "jsconfig.json", DAIDALOS_JSCONFIG },
    };
    for (const auto &f : FILES) {
        char path[700];
        std::snprintf(path, sizeof(path), "%s/%s", g_assets_dir, f.name);
        FILE *out = std::fopen(path, "wb");
        if (!out) continue;
        std::fwrite(f.body, 1, std::strlen(f.body), out);
        std::fclose(out);
    }
}

// A new material file, with the defaults in it - not an empty file. Something
// you can immediately drag onto an object and then edit.
static int material_create(const char *rel, void *) {
    if (!rel || !*rel || !g_assets_dir[0]) return 0;
    if (std::strstr(rel, "..")) return 0;
    char path[700];
    std::snprintf(path, sizeof(path), "%s/%s", g_assets_dir, rel);
    if (path_exists(path)) return 0;              // the caller walks the number up
    make_parent_dirs(path);
    dai_matfile m = dai_matfile_default();
    // Written in full: a new material is something you open and change, and
    // the short form has nothing in it to change.
    char text[1024];
    size_t need = dai_matfile_to_text_full(&m, text, sizeof(text));
    FILE *f = std::fopen(path, "wb");
    if (!f) return 0;
    size_t wrote = std::fwrite(text, 1, need, f);
    return (std::fclose(f) == 0 && wrote == need) ? 1 : 0;
}

// Every node that points at a .daimat gets that file's numbers. Called when a
// material is assigned and whenever the browser refreshes, which is also what
// happens after the built-in editor saves one - so editing the file and
// pressing Ctrl+S recolours every object using it.
//
// The cache is by PATH: a scene with two hundred crates on one material reads
// the file once. It is dropped wholesale on each pass rather than watched,
// because the pass only runs when something already told us the disk moved.
static void apply_materials(dai_doc *doc) {
    if (!doc || !g_assets_dir[0]) return;
    std::vector<dai_node> ids(dai_doc_count(doc));
    if (ids.empty()) return;
    dai_doc_nodes(doc, ids.data(), (uint32_t)ids.size());

    std::vector<std::pair<std::string, dai_matfile>> cache;
    auto material_of = [&](const std::string &rel, dai_matfile *out) {
        for (const auto &kv : cache)
            if (kv.first == rel) { *out = kv.second; return true; }
        char full[700];
        std::snprintf(full, sizeof(full), "%s/%s", g_assets_dir, rel.c_str());
        dai_matfile m = dai_matfile_default();
        char merr[256] = { 0 };
        if (dai_matfile_load(&m, full, merr, sizeof(merr)) != DAI_OK) {
            if (g_panels_for_log && merr[0]) dai_editor_ui_log(g_panels_for_log, 1, merr);
            return false;
        }
        cache.push_back({ rel, m });
        *out = m;
        return true;
    };

    int changed = 0;
    for (dai_node n : ids) {
        dai_node_desc r{};
        if (dai_doc_get(doc, n, &r) != DAI_OK || !r.materials[0]) continue;
        // Slot 0 is the surface of the whole object; the array is there for
        // meshes with several, which the renderer does not split yet.
        std::string first = r.materials;
        size_t semi = first.find(';');
        if (semi != std::string::npos) first = first.substr(0, semi);
        if (!dai_matfile_is_file(first.c_str())) continue;
        dai_matfile m{};
        if (!material_of(first, &m)) continue;
        dai_node_desc before = r;
        r.color = m.color;
        // Never exactly 0,0,0 - see above. Nudged, not clamped to grey.
        if (r.color.x == 0.0f && r.color.y == 0.0f && r.color.z == 0.0f)
            r.color = dai_vec3{ 1.0f / 255.0f, 1.0f / 255.0f, 1.0f / 255.0f };
        r.roughness = m.roughness;
        r.emissive = m.emissive;
        if (std::memcmp(&before, &r, sizeof(r)) != 0) {
            dai_doc_set(doc, n, &r);
            ++changed;
        }
    }
    if (changed && g_panels_for_log) {
        char msg[96];
        std::snprintf(msg, sizeof(msg), "materials applied to %d object(s)", changed);
        dai_editor_ui_log(g_panels_for_log, 0, msg);
    }
}

static int script_create(const char *name, void *) {
    if (!name || !*name || !g_assets_dir[0]) return 0;
    for (const char *c = name; *c; ++c) {
        bool ok = (*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') ||
                  (*c >= '0' && *c <= '9') || *c == '-' || *c == '_' || *c == '/' ||
                  *c == '.' || *c == ' ' || *c == '(' || *c == ')';
        if (!ok) return 0;
    }
    if (std::strstr(name, "..")) return 0;   // never escape the assets folder

    // A name that already carries an extension is taken at its word - that is
    // how "Create: C++ Behaviour" asks for a .cpp without a second callback.
    size_t nlen = std::strlen(name);
    bool is_cpp = nlen > 4 && std::strcmp(name + nlen - 4, ".cpp") == 0;
    char path[640];
    if (is_cpp) {
        std::snprintf(path, sizeof(path), "%s/%s", g_assets_dir, name);
        if (FILE *ex = std::fopen(path, "rb")) { std::fclose(ex); return 0; }
        make_parent_dirs(path);
        FILE *cf = std::fopen(path, "wb");
        if (!cf) {
            if (g_panels_for_log) {
                char msg[700];
                std::snprintf(msg, sizeof(msg), "create failed: %s", path);
                dai_editor_ui_log(g_panels_for_log, 2, msg);
            }
            return 0;
        }
        // "PlayerController.cpp" is the WORKED example, not the stub. It is
        // the file examples/scripts/PlayerController.cpp in the engine repo,
        // embedded here - because an example you have to go and find in a
        // source tree you did not clone is not an example, it is a rumour.
        //
        // Recognised by name on purpose: the Create menu asks for it by
        // asking for that file, and no second callback has to exist for one
        // template.
        {
            const char *base_name = std::strrchr(name, '/');
            base_name = base_name ? base_name + 1 : name;
            if (std::strcmp(base_name, "PlayerController.cpp") == 0) {
                std::fputs(
                   "// PlayerController.cpp - a player controller in C++, kept deliberately short.\n"
                   "//\n"
                   "// Drop this on an object that has a Rigidbody and press Ctrl+P.\n"
                   "//\n"
                   "//   WASD / arrows   move\n"
                   "//   Space           jump\n"
                   "//   Shift           sprint\n"
                   "//\n"
                   "// NEEDS A C++ COMPILER on PATH (g++ or clang++). The editor compiles this file\n"
                   "// the first time you press Play after it changed; with no compiler it says so\n"
                   "// in the Console and nothing happens. The .js behaviours need nothing.\n"
                   "//\n"
                   "// The numbers below are the inspector's - \"// @param\" lines are read from\n"
                   "// THIS FILE by the editor, drawn as fields, stored on the object, and handed\n"
                   "// back through me.param(). So the value written here is the default and the\n"
                   "// inspector is the truth, which is the same deal the .js behaviours have.\n"
                   "//\n"
                   "// @header Movement\n"
                   "// @tooltip Metres per second on the ground.\n"
                   "// @param float speed      = 6\n"
                   "// @tooltip Multiplied onto speed while Shift is held.\n"
                   "// @param float sprintMul  = 1.7\n"
                   "// @tooltip Upward impulse. Roughly: jump height in metres, times two.\n"
                   "// @param float jumpForce  = 5.5\n"
                   "// @tooltip How much steering is left while airborne, 0 to 1.\n"
                   "// @param float airControl = 0.35\n"
                   "// @header Camera\n"
                   "// @tooltip Optional: an object that follows this one from behind.\n"
                   "// @param camera followCam\n"
                   "// @param float camDistance = 8\n"
                   "// @param float camHeight   = 3\n"
                   "\n"
                   "#include \"dai_native.h\"\n"
                   "\n"
                   "// One flag that has to survive between frames - \"was space already down last\n"
                   "// frame\". Everything else is read fresh from the object each frame, which is\n"
                   "// what makes this file short.\n"
                   "static bool g_jump_held = false;\n"
                   "\n"
                   "DAI_BEHAVIOUR_INIT(api, self) {\n"
                   "    Node me(api, self);\n"
                   "    me.log(\"PlayerController ready - WASD, Space, Shift\");\n"
                   "}\n"
                   "\n"
                   "DAI_BEHAVIOUR_FRAME(api, self, dt) {\n"
                   "    Node me(api, self);\n"
                   "\n"
                   "    // ---- the inspector's numbers ----------------------------------------\n"
                   "    float speed      = me.param(\"speed\", 6.0f);\n"
                   "    float sprintMul  = me.param(\"sprintMul\", 1.7f);\n"
                   "    float jumpForce  = me.param(\"jumpForce\", 5.5f);\n"
                   "    float airControl = me.param(\"airControl\", 0.35f);\n"
                   "\n"
                   "    // ---- what the keyboard is asking for ---------------------------------\n"
                   "    Vec3 dir;\n"
                   "    if (me.key('a') || me.key(DAI_KEY_LEFT))  dir.x -= 1;\n"
                   "    if (me.key('d') || me.key(DAI_KEY_RIGHT)) dir.x += 1;\n"
                   "    if (me.key('w') || me.key(DAI_KEY_UP))    dir.z -= 1;\n"
                   "    if (me.key('s') || me.key(DAI_KEY_DOWN))  dir.z += 1;\n"
                   "    // Diagonals are not faster. Without this, W and D together move you 41%\n"
                   "    // quicker than W alone - the classic bug of every first controller.\n"
                   "    dir = dir.normalised();\n"
                   "\n"
                   "    float want = speed;\n"
                   "    if (me.key(DAI_KEY_SHIFT_L) || me.key(DAI_KEY_SHIFT_R)) want *= sprintMul;\n"
                   "\n"
                   "    // ---- move by setting the horizontal velocity -------------------------\n"
                   "    // Keeping Y is what makes gravity still apply: a controller that writes\n"
                   "    // all three components every frame cannot fall.\n"
                   "    bool on_ground = me.grounded();\n"
                   "    Vec3 v = me.velocity();\n"
                   "    float t = on_ground ? 1.0f : airControl;      // less authority in the air\n"
                   "    me.velocity(Vec3(v.x + (dir.x * want - v.x) * t,\n"
                   "                     v.y,\n"
                   "                     v.z + (dir.z * want - v.z) * t));\n"
                   "\n"
                   "    // ---- jumping ---------------------------------------------------------\n"
                   "    // Edge triggered: held down, space is true every frame, and a jump that\n"
                   "    // fires every frame is a rocket.\n"
                   "    bool down = me.key(DAI_KEY_SPACE);\n"
                   "    if (down && !g_jump_held && on_ground) me.impulse(Vec3(0, jumpForce, 0));\n"
                   "    g_jump_held = down;\n"
                   "\n"
                   "    // ---- face the way we are going ---------------------------------------\n"
                   "    if (dir.length() > 0.01f) me.yaw(__builtin_atan2f(dir.x, dir.z) * 57.2957795f);\n"
                   "\n"
                   "    // ---- the camera, if one was dragged into the field --------------------\n"
                   "    Node cam = me.param_node(\"followCam\");\n"
                   "    if (cam) {\n"
                   "        Vec3 p = me.position();\n"
                   "        Vec3 goal(p.x, p.y + me.param(\"camHeight\", 3.0f), p.z + me.param(\"camDistance\", 8.0f));\n"
                   "        Vec3 c = cam.position();\n"
                   "        // Frame rate independent smoothing: close a fraction of the remaining\n"
                   "        // distance derived from a half life, not a fixed step per frame.\n"
                   "        float k = 1.0f - __builtin_powf(0.0001f, dt);\n"
                   "        cam.position(c + (goal - c) * k);\n"
                   "    }\n"
                   "}\n"
                   "\n", cf);
                std::fclose(cf);
                return 1;
            }
        }
        // "PlayerController.cpp" is the WORKED example, not the stub. It is
        // the file examples/scripts/PlayerController.cpp in the engine repo,
        // embedded here - because an example you have to go and find in a
        // source tree you did not clone is not an example, it is a rumour.
        //
        // Recognised by name on purpose: the Create menu asks for it by
        // asking for that file, and no second callback has to exist for one
        // template.
        {
            const char *base_name = std::strrchr(name, '/');
            base_name = base_name ? base_name + 1 : name;
            if (std::strcmp(base_name, "PlayerController.cpp") == 0) {
                std::fputs(
                   "// PlayerController.cpp - a player controller in C++, kept deliberately short.\n"
                   "//\n"
                   "// Drop this on an object that has a Rigidbody and press Ctrl+P.\n"
                   "//\n"
                   "//   WASD / arrows   move\n"
                   "//   Space           jump\n"
                   "//   Shift           sprint\n"
                   "//\n"
                   "// NEEDS A C++ COMPILER on PATH (g++ or clang++). The editor compiles this file\n"
                   "// the first time you press Play after it changed; with no compiler it says so\n"
                   "// in the Console and nothing happens. The .js behaviours need nothing.\n"
                   "//\n"
                   "// The numbers below are the inspector's - \"// @param\" lines are read from\n"
                   "// THIS FILE by the editor, drawn as fields, stored on the object, and handed\n"
                   "// back through me.param(). So the value written here is the default and the\n"
                   "// inspector is the truth, which is the same deal the .js behaviours have.\n"
                   "//\n"
                   "// @header Movement\n"
                   "// @tooltip Metres per second on the ground.\n"
                   "// @param float speed      = 6\n"
                   "// @tooltip Multiplied onto speed while Shift is held.\n"
                   "// @param float sprintMul  = 1.7\n"
                   "// @tooltip Upward impulse. Roughly: jump height in metres, times two.\n"
                   "// @param float jumpForce  = 5.5\n"
                   "// @tooltip How much steering is left while airborne, 0 to 1.\n"
                   "// @param float airControl = 0.35\n"
                   "// @header Camera\n"
                   "// @tooltip Optional: an object that follows this one from behind.\n"
                   "// @param camera followCam\n"
                   "// @param float camDistance = 8\n"
                   "// @param float camHeight   = 3\n"
                   "\n"
                   "#include \"dai_native.h\"\n"
                   "\n"
                   "// One flag that has to survive between frames - \"was space already down last\n"
                   "// frame\". Everything else is read fresh from the object each frame, which is\n"
                   "// what makes this file short.\n"
                   "static bool g_jump_held = false;\n"
                   "\n"
                   "DAI_BEHAVIOUR_INIT(api, self) {\n"
                   "    Node me(api, self);\n"
                   "    me.log(\"PlayerController ready - WASD, Space, Shift\");\n"
                   "}\n"
                   "\n"
                   "DAI_BEHAVIOUR_FRAME(api, self, dt) {\n"
                   "    Node me(api, self);\n"
                   "\n"
                   "    // ---- the inspector's numbers ----------------------------------------\n"
                   "    float speed      = me.param(\"speed\", 6.0f);\n"
                   "    float sprintMul  = me.param(\"sprintMul\", 1.7f);\n"
                   "    float jumpForce  = me.param(\"jumpForce\", 5.5f);\n"
                   "    float airControl = me.param(\"airControl\", 0.35f);\n"
                   "\n"
                   "    // ---- what the keyboard is asking for ---------------------------------\n"
                   "    Vec3 dir;\n"
                   "    if (me.key('a') || me.key(DAI_KEY_LEFT))  dir.x -= 1;\n"
                   "    if (me.key('d') || me.key(DAI_KEY_RIGHT)) dir.x += 1;\n"
                   "    if (me.key('w') || me.key(DAI_KEY_UP))    dir.z -= 1;\n"
                   "    if (me.key('s') || me.key(DAI_KEY_DOWN))  dir.z += 1;\n"
                   "    // Diagonals are not faster. Without this, W and D together move you 41%\n"
                   "    // quicker than W alone - the classic bug of every first controller.\n"
                   "    dir = dir.normalised();\n"
                   "\n"
                   "    float want = speed;\n"
                   "    if (me.key(DAI_KEY_SHIFT_L) || me.key(DAI_KEY_SHIFT_R)) want *= sprintMul;\n"
                   "\n"
                   "    // ---- move by setting the horizontal velocity -------------------------\n"
                   "    // Keeping Y is what makes gravity still apply: a controller that writes\n"
                   "    // all three components every frame cannot fall.\n"
                   "    bool on_ground = me.grounded();\n"
                   "    Vec3 v = me.velocity();\n"
                   "    float t = on_ground ? 1.0f : airControl;      // less authority in the air\n"
                   "    me.velocity(Vec3(v.x + (dir.x * want - v.x) * t,\n"
                   "                     v.y,\n"
                   "                     v.z + (dir.z * want - v.z) * t));\n"
                   "\n"
                   "    // ---- jumping ---------------------------------------------------------\n"
                   "    // Edge triggered: held down, space is true every frame, and a jump that\n"
                   "    // fires every frame is a rocket.\n"
                   "    bool down = me.key(DAI_KEY_SPACE);\n"
                   "    if (down && !g_jump_held && on_ground) me.impulse(Vec3(0, jumpForce, 0));\n"
                   "    g_jump_held = down;\n"
                   "\n"
                   "    // ---- face the way we are going ---------------------------------------\n"
                   "    if (dir.length() > 0.01f) me.yaw(__builtin_atan2f(dir.x, dir.z) * 57.2957795f);\n"
                   "\n"
                   "    // ---- the camera, if one was dragged into the field --------------------\n"
                   "    Node cam = me.param_node(\"followCam\");\n"
                   "    if (cam) {\n"
                   "        Vec3 p = me.position();\n"
                   "        Vec3 goal(p.x, p.y + me.param(\"camHeight\", 3.0f), p.z + me.param(\"camDistance\", 8.0f));\n"
                   "        Vec3 c = cam.position();\n"
                   "        // Frame rate independent smoothing: close a fraction of the remaining\n"
                   "        // distance derived from a half life, not a fixed step per frame.\n"
                   "        float k = 1.0f - __builtin_powf(0.0001f, dt);\n"
                   "        cam.position(c + (goal - c) * k);\n"
                   "    }\n"
                   "}\n"
                   "\n", cf);
                std::fclose(cf);
                return 1;
            }
        }
        // The template is the documentation. A behaviour that starts as an
        // empty file means reading a header to find out what to type.
        std::fputs("// A Daidalos C++ behaviour.\n"
                   "//\n"
                   "// It is compiled to a shared library and loaded when you press Play,\n"
                   "// and rebuilt whenever this file is newer than the last build - so\n"
                   "// Ctrl+S, Play is the whole edit cycle. You need a C++ compiler on\n"
                   "// PATH (g++ or clang++); the console says so if there is none.\n"
                   "//\n"
                   "// Everything you can do is in dai_native.h, reached through `api`.\n"
                   "\n"
                   "#include <dai_native.h>\n"
                   "\n"
                   "DAI_BEHAVIOUR_INIT(api, self) {\n"
                   "    api->log(api, \"hello from C++\");\n"
                   "}\n"
                   "\n"
                   "DAI_BEHAVIOUR_FRAME(api, self, dt) {\n"
                   "    // spin gently around Y\n"
                   "    (void)dt; (void)self; (void)api;\n"
                   "}\n", cf);
        std::fclose(cf);
        return 1;
    }
    std::snprintf(path, sizeof(path), "%s/%s.js", g_assets_dir, name);
    // Refuse a name that is taken. The editor walks NewScript, NewScript (1),
    // ... and takes the first one the host accepts; saying yes to a file that
    // exists meant the new script silently replaced the old one.
    if (FILE *ex = std::fopen(path, "rb")) { std::fclose(ex); return 0; }
    make_parent_dirs(path);
    FILE *f = std::fopen(path, "wb");
    if (!f) {
        if (g_panels_for_log) {
            char msg[700];
            std::snprintf(msg, sizeof(msg), "create failed: %s", path);
            dai_editor_ui_log(g_panels_for_log, 2, msg);
        }
        return 0;
    }
    std::fputs("// A Daidalos behaviour. The engine calls the globals it finds:\n"
               "//   init()   once when play starts\n"
               "//   frame()  every rendered frame\n"
               "// The scene, from a script:\n"
               "//   scene.find(\"Crate\")       -> node id (or -1)\n"
               "//   node.getPos(id)             -> [x, y, z]\n"
               "//   node.setPos(id, x, y, z)    node.getRot(id) -> [x,y,z,w]\n"
               "// References: a line like\n"
               "//   // @param target\n"
               "// adds a drop field in the inspector; drag a node onto it and\n"
               "// read it here as params.target (the node's name).\n"
               "\n"
               "function init() {\n}\n"
               "\n"
               "function frame() {\n}\n", f);
    std::fclose(f);
    return 1;
}

// The Project Settings half of the settings panel: gravity, tick rate, tags -
// the values that belong to the project and are the same for everyone who
// opens it. Drawn by the host because the host is what owns dai_project.
// ---- the project's own string tables ------------------------------------
//
// Assets/Strings/*.daistr, one file per language. The editor previews in the
// one picked here, so a German label can be checked without exporting - and
// a missing entry shows up as the key, in the Game view, at the size it will
// really be.
#include "dai_strings.h"
static dai_strings *g_strings = nullptr;
static char g_lang[16] = { 0 };                 // "de", "" = no table loaded
static std::vector<std::string> g_langs;        // codes found in the project
static std::vector<std::string> g_lang_names;   // what each calls itself

// Reads Assets/Strings and remembers which languages exist. Cheap, and only
// run when a project opens or the folder is refreshed.
static void strings_scan() {
    g_langs.clear();
    g_lang_names.clear();
    if (!g_assets_dir[0]) return;
    char dir[700];
    std::snprintf(dir, sizeof(dir), "%s/Strings", g_assets_dir);
    std::vector<std::string> files;
    list_dir_files(dir, files);            // defined next to the other helpers
    for (const std::string &f : files) {
        if (f.size() < 8 || f.compare(f.size() - 7, 7, ".daistr") != 0) continue;
        char full[900];
        std::snprintf(full, sizeof(full), "%s/%s", dir, f.c_str());
        dai_strings *t = dai_strings_create();
        char err[256] = { 0 };
        if (dai_strings_load(t, full, err, sizeof(err)) == DAI_OK) {
            const char *code = dai_strings_lang(t);
            const char *nm = dai_strings_name(t);
            std::string c = code && code[0] ? code : f.substr(0, f.size() - 7);
            g_langs.push_back(c);
            g_lang_names.push_back(nm && nm[0] ? nm : c);
        } else if (g_panels_for_log) {
            char line[400];
            std::snprintf(line, sizeof(line), "Strings/%s: %s", f.c_str(), err);
            dai_editor_ui_log(g_panels_for_log, 1, line);
        }
        dai_strings_destroy(t);
    }
}

static std::vector<std::string> &strings_langs() { return g_langs; }

static void strings_use(const char *code) {
    if (!g_strings) g_strings = dai_strings_create();
    std::snprintf(g_lang, sizeof(g_lang), "%s", code ? code : "");
    if (!g_lang[0] || !g_assets_dir[0]) return;
    char full[900];
    std::snprintf(full, sizeof(full), "%s/Strings/%s.daistr", g_assets_dir, g_lang);
    char err[256] = { 0 };
    if (dai_strings_load(g_strings, full, err, sizeof(err)) != DAI_OK && g_panels_for_log) {
        char line[400];
        std::snprintf(line, sizeof(line), "language %s: %s", g_lang, err);
        dai_editor_ui_log(g_panels_for_log, 1, line);
    }
}

// What a Text component's contents mean to a player. Static buffer: the
// caller uses it immediately, and this is called once per label per frame.
// ---- the Localisation window's two halves --------------------------------
//
// Reading is easy: one dai_strings per file. WRITING is where the care is -
// the file is hand editable and lives in version control, so it is written
// sorted, with the same shape it was read in, and only the keys that have
// text. An empty cell writes NO line rather than an empty one: a key with no
// value would resolve to itself and look translated.
static int loc_load_cb(void *user) {
    dai_editor_ui *panels = (dai_editor_ui *)user;
    if (!panels || !g_assets_dir[0]) return 0;
    strings_scan();
    dai_editor_ui_loc_begin(panels);
    char dir[700];
    std::snprintf(dir, sizeof(dir), "%s/Strings", g_assets_dir);
    for (size_t i = 0; i < g_langs.size(); ++i)
        dai_editor_ui_loc_lang(panels, g_langs[i].c_str(), g_lang_names[i].c_str());
    for (const std::string &code : g_langs) {
        char full[900];
        std::snprintf(full, sizeof(full), "%s/%s.daistr", dir, code.c_str());
        dai_strings *t = dai_strings_create();
        char err[256] = { 0 };
        if (dai_strings_load(t, full, err, sizeof(err)) == DAI_OK) {
            std::vector<const char *> keys(dai_strings_count(t) + 1, nullptr);
            uint32_t nk = dai_strings_keys(t, keys.data(), (uint32_t)keys.size());
            for (uint32_t k = 0; k < nk && k < keys.size(); ++k)
                if (keys[k]) dai_editor_ui_loc_set(panels, keys[k], code.c_str(),
                                                   dai_strings_get(t, keys[k]));
        }
        dai_strings_destroy(t);
    }
    return 1;
}

static int loc_save_cb(void *user) {
    dai_editor_ui *panels = (dai_editor_ui *)user;
    if (!panels || !g_assets_dir[0]) return 0;
    char dir[700];
    std::snprintf(dir, sizeof(dir), "%s/Strings", g_assets_dir);
#ifdef _WIN32
    CreateDirectoryA(dir, nullptr);
#else
    mkdir(dir, 0755);
#endif
    uint32_t nl = dai_editor_ui_loc_lang_count(panels);
    uint32_t nk = dai_editor_ui_loc_key_count(panels);
    int wrote = 0;
    for (uint32_t l = 0; l < nl; ++l) {
        const char *name = nullptr;
        const char *code = dai_editor_ui_loc_lang_at(panels, l, &name);
        if (!code) continue;
        std::string out = "daidalos-strings 1\n";
        out += "lang "; out += code; out += "\n";
        if (name && name[0]) { out += "name "; out += name; out += "\n"; }
        out += "\n";
        for (uint32_t k = 0; k < nk; ++k) {
            const char *key = dai_editor_ui_loc_key_at(panels, k);
            const char *val = dai_editor_ui_loc_get(panels, k, l);
            if (!key || !val || !val[0]) continue;   // no text: no line
            out += key;
            out += "  ";
            // A newline inside a value goes back as the escape it came from,
            // or the file would grow a line the parser reads as a new key.
            for (const char *c = val; *c; ++c) {
                if (*c == '\n') out += "\\n";
                else if (*c == '\r') continue;
                else out += *c;
            }
            out += "\n";
        }
        char full[900];
        std::snprintf(full, sizeof(full), "%s/%s.daistr", dir, code);
        FILE *f = std::fopen(full, "wb");
        if (!f) continue;
        std::fwrite(out.data(), 1, out.size(), f);
        std::fclose(f);
        ++wrote;
    }
    // What was just written is what the editor should now be previewing.
    strings_scan();
    strings_use(g_lang);
    return wrote > 0;
}

static const char *hud_resolve(const char *text, void *) {
    static char buf[256];
    return dai_strings_resolve(g_strings, text, buf, sizeof(buf));
}

static dai_project_settings *g_psettings = nullptr;
static dai_world *g_world_for_settings = nullptr;
// What the running world was actually created with, so the panel can say
// "restart to switch" instead of pretending the dropdown took effect.
static int g_active_backend = 0;
static dai_ui *g_ui_for_settings = nullptr;
static void draw_project_settings(void *) {
    if (!g_psettings || !g_ui_for_settings || !g_project) return;
    dai_ui *ui = g_ui_for_settings;
    dai_project_settings &ps = *g_psettings;
    dai_project_settings before = ps;
    dai_ui_label_fmt(ui, "Project: %s", dai_project_name(g_project));
    dai_ui_num_vec3(ui, "Gravity", &ps.gravity[0], 0.05f);
    float hz = (float)ps.tick_hz;
    if (dai_ui_num_field(ui, "Tick Hz", &hz, 1.0f, 10.0f, 240.0f, "tickhz"))
        ps.tick_hz = (int)(hz + 0.5f);
    float mb = (float)ps.max_bodies;
    if (dai_ui_num_field(ui, "Max bodies", &mb, 16.0f, 16.0f, 100000.0f, "maxbodies"))
        ps.max_bodies = (int)(mb + 0.5f);
    // Talos is THE physics. Jolt was a second engine you could pick and then
    // not have - Jolt is not compiled into this build at all, so offering it
    // was only a way to lie to the user.
    //
    // Index mapping: the dropdown is built from backends that EXIST, in
    // dai_physics_backend order (TALOS=0, NULL=1). A scene file that names
    // Jolt still loads - it falls back to null, and the label says so.
    int sel = ps.physics_backend == DAI_PHYSICS_NULL ? 1 : 0;
    static const char *const BACKENDS[] = { "Talos", "Kein Solver (nur Fall)" };
    if (dai_ui_option(ui, "Physics", &sel, BACKENDS, 2))
        ps.physics_backend = sel == 1 ? DAI_PHYSICS_NULL : DAI_PHYSICS_TALOS;
    dai_ui_help(ui, "Talos: collisions, joints, friction. Kein Solver: gravity and a "
                    "floor at y=0 only, bodies pass through each other. Jolt is not "
                    "part of this engine any more.");
    if (ps.physics_backend != DAI_PHYSICS_TALOS && ps.physics_backend != DAI_PHYSICS_NULL) {
        dai_ui_label(ui, "!! scene asked for Jolt - not in this build, using 'Kein Solver'");
    }
    if (g_world_for_settings) {
        const char *live = dai_backend_name(g_world_for_settings);
        dai_ui_label_fmt(ui, "running: %s%s", live ? live : "?",
                         ps.physics_backend != g_active_backend ? "  (restart to switch)" : "");
    }
    dai_ui_separator(ui);
    dai_ui_label(ui, "Defaults for NEW rigidbodies");
    dai_ui_num_field(ui, "Def. friction", &ps.default_friction, 0.01f, 0.0f, 10.0f, "psfric");
    dai_ui_help(ui, "Coefficient mu given to every Rigidbody component added from now on. "
                    "0 = ice, 0.6 = wood, 1.0 = rubber. Existing objects are not touched.");
    dai_ui_num_field(ui, "Def. bounce", &ps.default_restitution, 0.01f, 0.0f, 1.0f, "psrest");
    dai_ui_help(ui, "Restitution 0..1 given to every new Rigidbody component. "
                    "Existing objects are not touched.");
    dai_ui_separator(ui);
    // Language. The list is what the project HAS, not a list of languages the
    // world contains: an option that resolves to no file is an option that
    // makes every label fall back to its key.
    {
        dai_ui_label(ui, "Language (Assets/Strings/*.daistr)");
        if (g_langs.empty()) {
            dai_ui_label(ui, "no tables yet - a label shows its own words until there are");
        } else {
            std::vector<const char *> names;
            int sel = 0;
            for (size_t i = 0; i < g_langs.size(); ++i) {
                names.push_back(g_lang_names[i].c_str());
                if (g_langs[i] == g_lang) sel = (int)i;
            }
            if (dai_ui_option(ui, "Preview", &sel, names.data(), (int)names.size()) &&
                sel >= 0 && sel < (int)g_langs.size()) {
                strings_use(g_langs[(size_t)sel].c_str());
                std::snprintf(ps.language, sizeof(ps.language), "%s", g_lang);
            }
            dai_ui_help(ui, "What the Game view shows, and what the exported game starts in. "
                            "A key with no entry shows the key.");
        }
        char rescan[32] = "Rescan";
        if (dai_ui_button_fit(ui, rescan)) { strings_scan(); strings_use(g_lang); }
    }
    dai_ui_separator(ui);
    dai_ui_input_text(ui, "App name", ps.app_name, sizeof(ps.app_name));
    dai_ui_separator(ui);
    dai_ui_label(ui, "Tags");
    for (int i = 0; i < 4; ++i) {
        char lbl[16];
        std::snprintf(lbl, sizeof(lbl), "Tag %d", i);
        dai_ui_input_text(ui, lbl, ps.tags[i], DAI_PROJECT_TAG_MAX);
    }
    if (std::memcmp(&before, &ps, sizeof(ps)) != 0) {
        dai_project_settings_save(g_project, &ps);
        // Saved is not applied: the world keeps last tick's gravity until the
        // host pushes the new one. A settings panel that only takes effect
        // after an export is indistinguishable from a broken one.
        if (g_world_for_settings)
            dai_set_gravity(g_world_for_settings,
                            dai_vec3{ ps.gravity[0], ps.gravity[1], ps.gravity[2] });
    }
    dai_ui_label(ui, "Changes are saved immediately.");
}

// ---- the editor's own console --------------------------------------------
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
        size_t nl = g_log_tail.find('\n');
        if (nl == std::string::npos) {
            if (g_log_tail.size() > 8192) g_log_tail.clear();   // a line that never ends
            break;
        }
        std::string line = g_log_tail.substr(0, nl);
        g_log_tail.erase(0, nl + 1);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
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
// it here. One editor, one font - this is not a place that needs generality.
static dai_renderer *g_renderer = nullptr;

// A texture for the HUD, by project path. Cached: the HUD asks once per image
// per frame, and loading a PNG sixty times a second would be a slideshow.
// {score} in a HUD label -> what the script on that node says right now.
//
// Looked up as a global first and then on `state`, because both spellings are
// real: `var score = 0` at the top of a behaviour is a global, and
// `state.score` is what survives a hot reload. A name that is neither is left
// alone - the placeholder stays on screen, which is exactly what you want
// while you are still placing the label.
//
// The name is checked before it is pasted into source: letters, digits and
// underscore only. "{a; drop()}" is a label, not a command.
static int hud_var_cb(dai_node n, const char *name, char *out, size_t out_size, void *) {
#ifdef DAI_WITH_SCRIPT
    if (!name || !name[0] || !out || out_size < 2) return 0;
    for (const char *q = name; *q; ++q)
        if (!((*q >= 'a' && *q <= 'z') || (*q >= 'A' && *q <= 'Z') ||
              (*q >= '0' && *q <= '9') || *q == '_')) return 0;
    for (RunningScript &rs : g_running) {
        if (rs.node != n) continue;
        char js[512];
        std::snprintf(js, sizeof(js),
            "state.__hvok = 0;"
            "try { var __t = (typeof %s !== 'undefined') ? %s"
            "               : (state && state.%s !== undefined ? state.%s : undefined);"
            "      if (__t !== undefined && __t !== null) {"
            "          state.__hv = String(__t); state.__hvok = 1; } } catch (e) {}",
            name, name, name, name);
        char err[128] = { 0 };
        if (dai_script_eval(rs.s, js, "hudvar", err, sizeof(err)) != DAI_OK) continue;
        if (dai_script_get_number(rs.s, "__hvok", 0.0) < 0.5) continue;
        if (!dai_script_get_string(rs.s, "__hv", out, out_size)) continue;
        return 1;
    }
#else
    (void)n; (void)name; (void)out; (void)out_size;
#endif
    return 0;
}

static uint32_t hud_image_cb(const char *path, float *out_w, float *out_h, void *) {
    if (!path || !path[0] || !g_renderer || !g_assets_dir[0]) return 0;
    struct Entry { dai_texture tex; float w, h; };
    static std::unordered_map<std::string, Entry> cache;
    auto it = cache.find(path);
    if (it != cache.end()) {
        if (out_w) *out_w = it->second.w;
        if (out_h) *out_h = it->second.h;
        return it->second.tex;
    }
    char full[700];
    std::snprintf(full, sizeof(full), "%s/%s", g_assets_dir, path);
    Entry e{ 0, 0, 0 };
    e.tex = dai_render_texture_load(g_renderer, full, 1);
    // "image not loaded" on its own is not a bug report. Say WHICH file was
    // tried, once per path - the answer is almost always that it is not under
    // the project's asset folder, or that the PNG is a flavour the decoder
    // does not read.
    if (!e.tex && g_panels_for_log) {
        // The DECODER already knows why it said no. Guessing from the IHDR
        // bytes was a guess: it told the boss "16 bit samples and unusual
        // layouts" about an 8 bit paletted image, which was simply wrong.
        // Ask the renderer for the sentence it wrote and print THAT.
        char line[900];
        const char *why = dai_render_last_error(g_renderer);
        FILE *tf = std::fopen(full, "rb");
        if (!tf) {
            std::snprintf(line, sizeof(line), "image: no such file - %s", full);
        } else {
            std::fclose(tf);
            std::snprintf(line, sizeof(line), "image: %s - %s",
                          (why && why[0]) ? why : "could not be decoded", full);
        }
        dai_editor_ui_log(g_panels_for_log, 2, line);
    }
    // The renderer does not hand back the pixel size, and the honest answer
    // to "how big is this by default" is the file's own size. Read the PNG
    // header - eight bytes at a fixed offset - rather than decode the whole
    // image a second time.
    // A picture that loaded but is not all there says so ONCE. A logo that is
    // 58 rows of 330 is not a decoder bug and not a mystery - it is a damaged
    // file, and the only way anybody finds that out is if the editor says it.
    if (e.tex && g_panels_for_log) {
        const char *why = dai_render_last_error(g_renderer);
        if (why && why[0]) {
            char line[900];
            std::snprintf(line, sizeof(line), "image: %s - %s", why, full);
            dai_editor_ui_log(g_panels_for_log, 1, line);
        }
    }
    if (e.tex) {
        FILE *pf = std::fopen(full, "rb");
        if (pf) {
            unsigned char hdr[24] = { 0 };
            if (std::fread(hdr, 1, sizeof(hdr), pf) == sizeof(hdr) &&
                hdr[1] == 'P' && hdr[2] == 'N' && hdr[3] == 'G') {
                e.w = (float)((hdr[16] << 24) | (hdr[17] << 16) | (hdr[18] << 8) | hdr[19]);
                e.h = (float)((hdr[20] << 24) | (hdr[21] << 16) | (hdr[22] << 8) | hdr[23]);
            }
            std::fclose(pf);
        }
        if (e.w <= 0.0f) { e.w = 128.0f; e.h = 128.0f; }
    }
    // A failure is cached too: a missing file must cost one failed load, not
    // one per frame for the rest of the session.
    cache[path] = e;
    if (out_w) *out_w = e.w;
    if (out_h) *out_h = e.h;
    return e.tex;
}


// Plays the list back into the UI draw list, clipped to the view.
static void gui_flush(dai_ui *ui, float vx, float vy, float vw, float vh) {
    if (g_gui_cmds.empty()) return;
    dai_ui_clip_begin(ui, vx, vy, vw, vh);
    const float base = dai_ui_text_height(ui) > 0.0f ? dai_ui_text_height(ui) : 13.0f;
    for (const GuiCmd &c : g_gui_cmds) {
        float x = vx + c.x, y = vy + c.y;
        if (c.kind == 1) {
            dai_ui_rect(ui, x, y, c.w, c.h, c.color);
        } else if (c.kind == 0) {
            float k = c.size / base;
            dai_ui_text_scaled(ui, x + 1.0f, y + 1.0f, c.text.c_str(), 0xB0000000u, k);
            dai_ui_text_scaled(ui, x, y, c.text.c_str(), c.color, k);
        } else if (c.kind == 2) {
            float iw = 0, ih = 0;
            uint32_t tex = hud_image_cb(c.text.c_str(), &iw, &ih, nullptr);
            if (tex) dai_ui_image_at(ui, tex, x, y, c.w > 0 ? c.w : iw, c.h > 0 ? c.h : ih,
                                     0, 0, 1, 1, c.color);
        } else if (c.kind == 3) {
            bool over = g_gui_mx >= x && g_gui_mx < x + c.w &&
                        g_gui_my >= y && g_gui_my < y + c.h;
            const dai_ui_style *st = dai_ui_style_of(ui);
            uint32_t bg = over ? (g_gui_down ? st->button_active : st->button_hover) : st->button;
            dai_ui_rrect(ui, x, y, c.w, c.h, 4.0f, bg);
            dai_ui_rect_outline(ui, x, y, c.w, c.h, 1.0f, st->panel_border);
            float tw = dai_ui_text_width(ui, c.text.c_str());
            dai_ui_text(ui, x + (c.w - tw) * 0.5f,
                        y + (c.h - dai_ui_text_height(ui)) * 0.5f, c.text.c_str(), st->text);
        }
    }
    dai_ui_clip_end(ui);
    g_gui_cmds.clear();
}



static dai_ui       *g_ui = nullptr;
static dai_font     *g_font = nullptr;
static float        *g_ui_scale_out = nullptr;   // prefs.ui_scale lives in main()
// The desktop's scale (1.25 at 125%) times whatever the user chose in
// Settings. Everything the editor lays out is in logical pixels; this is the
// only number that turns them into the real ones the window is made of.
static float         g_dpi = 1.0f;
static float         g_dpi_auto = 1.0f;    // what the display says
static float         g_dpi_pref = 0.0f;    // 0 = follow the display
// The live prefs block, so a setting can be persisted the MOMENT it changes
// rather than at a clean exit that may never come. Declared after g_dpi_pref
// because that is the value it copies.
static dai_prefs    *g_prefs = nullptr;
static dai_editor_ui *g_panels_for_prefs = nullptr;
static void save_prefs_now() {
    if (!g_prefs) return;
    // NOT ui_scale: that one belongs to apply_font, which writes it through
    // g_ui_scale_out the moment the size changes. Writing the display
    // override into it here is what used to erase the interface size.
    g_prefs->dpi_scale = g_dpi_pref;
    g_prefs->language = dai_tr_lang_get();
    if (g_panels_for_prefs)
        g_prefs->script_editor = dai_editor_ui_script_editor_pref_get(g_panels_for_prefs);
    dai_prefs_save(g_prefs);
}
static dai_window   *g_win_for_scale = nullptr;
static dai_icons    *g_icons = nullptr;

// A fractional pixel size (13 * 1.5 = 19.5) puts every glyph baseline half
// a texel off the pixel grid and blurs the whole UI. Snapped here, once, so
// both the texture and the layout agree on a whole pixel size.
static void apply_font(float px, void *);
static float g_font_px = 13.0f;
static void apply_font(float px, void *) {
    g_font_px = px < 2.0f ? 13.0f : px;
    // Whole pixels or not at all: a 19.5 px font's baseline sits on half a
    // texel, and every glyph in the UI smears. Snap up; the layout uses the
    // same number, so nothing is scaled.
    px = px < 2.0f ? 2.0f : (float)((int)(px + 0.9999f));
    if (g_ui_scale_out) *g_ui_scale_out = px / 13.0f;
    save_prefs_now();      // the size settles now, not at some clean exit
    if (!g_renderer || !g_ui) return;
    char err[256] = { 0 };
    dai_font *nf = dai_font_load_ui_scaled(px, g_dpi, err, sizeof(err));
    if (!nf) { std::printf("font reload failed: %s\n", err); return; }
    uint32_t aw = 0, ah = 0;
    const uint8_t *atlas = dai_font_atlas(nf, &aw, &ah);
    std::vector<uint8_t> rgba((size_t)aw * ah * 4);
    for (size_t i = 0; i < (size_t)aw * ah; ++i) {
        rgba[i*4+0] = 255; rgba[i*4+1] = 255; rgba[i*4+2] = 255; rgba[i*4+3] = atlas[i];
    }
    dai_texture nt = dai_render_texture_create(g_renderer, rgba.data(), aw, ah, 0);
    if (!nt) { dai_font_free(nf); return; }
    dai_ui_font_set(g_ui, nf, nt);
    if (g_font) dai_font_free(g_font);
    g_font = nf;
}

// Changing the display scale rebuilds both atlases: the font and the icons
// are rasterised AT the scale, which is the whole point - a magnified 13 px
// atlas is exactly the blur this avoids.
static void apply_ui_scale(float scale, void *) {
    g_dpi_pref = scale;
    save_prefs_now();
    float want = scale > 0.05f ? scale : g_dpi_auto;
    if (!(want > 0.4f) || want > 4.0f) want = 1.0f;
    g_dpi = want;
    if (g_ui) dai_ui_scale_set(g_ui, g_dpi);
    apply_font(g_font_px, nullptr);                // reloads at the new scale
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
    std::printf("UI scale is now %.0f%% (%s)\n", g_dpi * 100.0f,
                scale > 0.05f ? "set in Settings" : "from the display");
}

// The renderer's inventory, so the inspector's mesh picker shows names
// instead of a number nobody chose.
static const char *mesh_name_of(uint32_t mesh, void *) {
    switch (mesh) {
    case DAI_MESH_BOX:     return "Box (builtin)";
    case DAI_MESH_SPHERE:  return "Sphere (builtin)";
    case DAI_MESH_CAPSULE: return "Capsule (builtin)";
    case DAI_MESH_CYLINDER: return "Cylinder (builtin)";
    case DAI_MESH_PLANE:   return "Plane (builtin)";
    default: break;
    }
    static char buf[48];
    std::snprintf(buf, sizeof(buf), "mesh %u", mesh);
    return buf;
}

// ---- crash report ---------------------------------------------------------
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

// The recovery point, armed only around the work we are willing to abandon.
#include <csetjmp>
static jmp_buf  g_guard_jmp;
static int      g_guard_armed = 0;
static int      g_guard_faults = 0;        // consecutive, reset by a clean frame
static unsigned long g_guard_last_code = 0;
static void    *g_guard_last_addr = nullptr;

// Runs fn under the net. 1 = it returned normally, 0 = it faulted and was
// abandoned. Never call anything from fn that must not be half-done: this is
// for a UI frame, not for a file write.
static int guard_run(void (*fn)(void *), void *user) {
    if (setjmp(g_guard_jmp) != 0) {
        g_guard_armed = 0;
        return 0;                          // came back through the handler
    }
    g_guard_armed = 1;
    fn(user);
    g_guard_armed = 0;
    return 1;
}

static void crash_write(const char *what, unsigned long long code,
                        void *addr, void *const *frames, int nframes) {
    if (!g_crash_path[0]) return;
    FILE *f = std::fopen(g_crash_path, "wb");
    if (!f) return;
    std::fprintf(f, "DAIDALOS crash report\n");
    std::fprintf(f, "version : %s\n", dai_version());
    std::fprintf(f, "reason  : %s\n", what ? what : "?");
    std::fprintf(f, "code    : 0x%llx\n", code);
    std::fprintf(f, "address : %p\n", addr);
    std::fprintf(f, "project : %s\n", g_project ? dai_project_path(g_project) : "(none)");
    std::fprintf(f, "scene   : %s\n", g_scene_path[0] ? g_scene_path : "(none)");
    std::fprintf(f, "\nframes (module+offset - resolve with addr2line):\n");
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
        const char *base = std::strrchr(modname, '\\');
        std::fprintf(f, "  %2d  %s+0x%llx%s\n", i,
                     base ? base + 1 : (modname[0] ? modname : "?"), off,
                     m == self_mod ? "   <- editor" : "");
    }
#else
    for (int i = 0; i < nframes; ++i) std::fprintf(f, "  %2d  %p\n", i, frames[i]);
#endif
    // The last thing the console said is usually the last thing that happened.
    if (g_panels_for_log) {
        std::fprintf(f, "\nconsole tail:\n");
        char tail[4096];
        uint32_t n = dai_editor_ui_log_tail(g_panels_for_log, tail, sizeof(tail));
        if (n) std::fwrite(tail, 1, n, f);
    }
    std::fclose(f);
}

#ifdef _WIN32
static LONG WINAPI crash_filter(EXCEPTION_POINTERS *ep) {
    // Armed and not yet hopeless: abandon the frame and carry on. The report
    // is not written for these - the console line is, every time, and that is
    // what turns "it crashed again" into something with a number in it.
    if (g_guard_armed && g_guard_faults < 8) {
        g_guard_last_code = ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionCode : 0;
        g_guard_last_addr = ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionAddress : nullptr;
        ++g_guard_faults;
        longjmp(g_guard_jmp, 1);
    }
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
                  "DAIDALOS stopped unexpectedly.\n\nA report was written to:\n%s\n\n"
                  "Send that file and it can be traced to the line.", g_crash_path);
    MessageBoxA(nullptr, msg, "DAIDALOS", MB_OK | MB_ICONERROR);
    return EXCEPTION_EXECUTE_HANDLER;
}
#else
static void crash_signal(int sig) {
    if (g_guard_armed && g_guard_faults < 8) {
        g_guard_last_code = (unsigned long)sig;
        g_guard_last_addr = nullptr;
        ++g_guard_faults;
        longjmp(g_guard_jmp, 1);
    }
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

// Closing with work that is not on disk. Three answers, because there are
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
                  "The scene has unsaved changes.\n\n%s\n\n"
                  "Yes - save and close\n"
                  "No  - close and lose them\n"
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
    std::printf("closing with unsaved changes (no dialog on this platform)\n");
    return 1;
#endif
}

int main(int argc, char **argv) {
    // Unbuffered: a crash must not swallow the last thing the editor said.
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::set_new_handler(diag_new_failed);
    if (const char *dg = std::getenv("DAIDALOS_DIAG")) {
        g_diag_on = std::atoi(dg);
        if (g_diag_on > 0) g_diag_frames = g_diag_on;   // trace this many frames
    }
    const char *scene_path = argc > 1 ? argv[1] : nullptr;
#ifdef _WIN32
    std::snprintf(g_projects_root, sizeof(g_projects_root), "C:\\daidalos\\projects");
    GetModuleFileNameA(nullptr, g_update.exe_path, (DWORD)sizeof(g_update.exe_path));
#else
    std::snprintf(g_projects_root, sizeof(g_projects_root), "projects");
    if (argc > 0 && argv[0] && realpath(argv[0], g_update.exe_path) == nullptr)
        g_update.exe_path[0] = 0;
#endif
    if (g_update.exe_path[0]) std::thread(update_worker).detach();

    // Unity cannot run without a project, and neither can this: the scene, the
    // assets and half the settings only mean something relative to one. Last
    // one used, else the first one on disk, else a fresh "Untitled".
    dai_prefs prefs = dai_prefs_default();
    dai_prefs_load(&prefs);
    if (prefs.last_project[0]) open_project_path(prefs.last_project);
    if (!g_project) {
        char names[64][DAI_PROJECT_NAME_MAX];
        uint32_t pn = dai_project_list(g_projects_root, names[0], 64, DAI_PROJECT_NAME_MAX);
        if (pn > 0) project_open(names[0], nullptr);
    }
    if (!g_project) project_create("Untitled", nullptr);
    if (g_project) std::printf("project: %s\n", dai_project_path(g_project));

    dai_project_settings psettings = dai_project_settings_default();
    if (g_project) dai_project_settings_load(g_project, &psettings);

    dai_config cfg{};
    cfg.backend = psettings.physics_backend;   // the project chooses, not the binary
    g_active_backend = cfg.backend;
    cfg.tick_hz = psettings.tick_hz > 0 ? (uint32_t)psettings.tick_hz : 60;
    cfg.max_bodies = psettings.max_bodies > 0 ? (uint32_t)psettings.max_bodies : 4096;
    cfg.snapshot_ring = 120; cfg.seed = 1;
    dai_world *w = nullptr;
    if (dai_create(&cfg, &w) != DAI_OK) { std::printf("world failed\n"); return 1; }
    g_world_for_settings = w;
    dai_set_gravity(w, dai_vec3{ psettings.gravity[0], psettings.gravity[1], psettings.gravity[2] });
    if (std::strcmp(dai_backend_name(w), "null") == 0) {
        // "Nothing collides" is a setting, and a setting nobody can see is a
        // bug report. Say it where the user is already looking.
        std::printf("!! PHYSICS: no solver selected (Settings > Project Settings > "
                    "Physics). Colliders are ignored - the only thing that stops "
                    "a falling object is an invisible floor at y=0.\n");
    }
    std::printf("physics: %s, gravity %.2f %.2f %.2f\n", dai_backend_name(w),
                (double)psettings.gravity[0], (double)psettings.gravity[1], (double)psettings.gravity[2]);
    dai_scene *sc = dai_scene_create(w);
    dai_doc *doc = dai_doc_create();
    g_scene_doc = doc;   // the scene host callbacks save through it
    dai_doc_sync *sync = dai_doc_sync_create(doc, sc);

    char err[256] = { 0 };
    if (!scene_path && g_scene_path[0]) scene_path = g_scene_path;
    if (scene_path && dai_doc_load(doc, scene_path, err, sizeof(err)) != DAI_OK) {
        std::printf("could not load %s: %s\n", scene_path, err);
        scene_path = nullptr;
    }
    if (dai_doc_count(doc) == 0) {
        dai_node_desc g = dai_node_desc_default();
        std::snprintf(g.name, sizeof(g.name), "Ground");
        g.motion = DAI_STATIC;
        g.half_extent = { 12, 0.5f, 12 };
        g.position = { 0, -0.5f, 0 };
        g.color = { 0.20f, 0.22f, 0.19f };
        dai_doc_add(doc, &g);
        for (int i = 0; i < 4; ++i) {
            dai_node_desc b = dai_node_desc_default();
            std::snprintf(b.name, sizeof(b.name), "crate%d", i);
            b.motion = DAI_DYNAMIC;
            b.half_extent = { 0.6f, 0.6f, 0.6f };
            b.position = { -2.0f + (float)i * 1.4f, 0.6f + (float)i * 1.3f, 0 };
            dai_doc_add(doc, &b);
        }
    }
    dai_doc_sync_apply(sync);

    const uint32_t W = 1440, H = 810;
    dai_render_desc rd{};
    rd.width = W; rd.height = H; rd.msaa = 4;
    console_capture_begin();
    dai_renderer *r = dai_render_create(&rd, err, sizeof(err));
    if (!r) { std::printf("renderer failed: %s\n", err); return 1; }
    dai_window *win = dai_window_open(r, "Daidalos Editor", W, H, err, sizeof(err));
    if (!win) { std::printf("window failed: %s\n", err); return 1; }
    // THE line that made input.key() work. sp_key() and nv_key() ask this
    // window whether a key is down, and it was declared and never assigned -
    // so every script saw every key as "up" and WASD moved nothing, in every
    // build that has ever shipped. A pointer that is only ever read is not a
    // cache, it is a missing line.
    g_win_for_scripts = win;
    // A line in the log that proves WHICH build is running: two editor.exes
    // on the same machine look identical from the task list.
    std::printf("editor up: %ux%u, %s\n", W, H, dai_version());

    // 13 px, not 17: this is an editor, and the panels are full of numeric
    // fields. On Windows the window is DPI aware now, so 13 px is 13 real
    // pixels instead of 13 stretched to 20 by the desktop scaling.
    // The desktop's scale factor, asked for ONCE the window exists. 13 px of
    // interface is 13 px of interface at every zoom level; what changes is how
    // many real pixels the glyphs are rasterised into.
    g_dpi_auto = dai_window_dpi_scale(win);
    if (!(g_dpi_auto > 0.5f) || g_dpi_auto > 4.0f) g_dpi_auto = 1.0f;
    g_dpi_pref = prefs.dpi_scale;
    g_dpi = g_dpi_pref > 0.05f ? g_dpi_pref : g_dpi_auto;
    g_win_for_scale = win;
    {
        uint32_t rw = 0, rh = 0;
        dai_window_size(win, &rw, &rh);
        std::printf("display scale: %.2f (auto %.2f), window %ux%u real px\n",
                    g_dpi, g_dpi_auto, rw, rh);
    }
    dai_font *font = dai_font_load_ui_scaled(13.0f, g_dpi, err, sizeof(err));
    if (!font) std::printf("no font: %s\n", err);   // the UI would draw blank boxes
    dai_texture font_tex = 0;
    if (font) {
        uint32_t aw = 0, ah = 0;
        const uint8_t *atlas = dai_font_atlas(font, &aw, &ah);
        std::vector<uint8_t> rgba((size_t)aw * ah * 4);
        for (size_t i = 0; i < (size_t)aw * ah; ++i) {
            rgba[i*4+0] = 255; rgba[i*4+1] = 255; rgba[i*4+2] = 255; rgba[i*4+3] = atlas[i];
        }
        font_tex = dai_render_texture_create(r, rgba.data(), aw, ah, 0);
    }
    dai_ui *ui = dai_ui_create(font, font_tex);
    dai_ui_scale_set(ui, g_dpi);
    // The editor's strings are data (see dai_tr.h). The engine's own games
    // are not: translate only what this binary ships.
    dai_ui_translate(ui, 1);
    dai_tr_lang(prefs.language);

    // Vector icons for the toolbar and the inspector's component headers.
    // Rasterised HERE, at 16 px, because that is the size this interface draws
    // them at - the SVG sources are resolution independent, the atlas is not.
    dai_icons *icons = dai_icons_create(16.0f * g_dpi);
    if (icons) dai_icons_display_size(icons, 16.0f);
    g_icons = icons;
    if (icons) {
        uint32_t iw = 0, ih = 0;
        const uint8_t *irgba = dai_icons_atlas_rgba(icons, &iw, &ih);
        if (irgba && iw && ih)
            dai_ui_set_icons(ui, icons, dai_render_texture_create(r, irgba, iw, ih, 0));
    }

    dai_render_sun(r, dai_vec3{ 0.42f, 0.80f, 0.42f }, dai_vec3{ 1.0f, 0.95f, 0.86f }, 1.3f);
    dai_render_ambient(r, dai_vec3{ 0.24f, 0.40f, 0.72f }, dai_vec3{ 0.26f, 0.24f, 0.20f }, 0.38f);
    dai_render_exposure(r, 0.45f);
    dai_render_clear_color(r, 0.078f, 0.078f, 0.078f);   // #141414 - the chrome
    dai_render_shadow_extent(r, 24.0f);
    dai_render_sky(r, 1);

    dai_editor *ed = dai_editor_create(doc, sync);
    dai_editor_camera(ed, dai_vec3{ 6.0f, 4.5f, 9.5f }, dai_vec3{ 0, 1, 0 }, dai_vec3{ 0, 1, 0 },
                      55.0f, 0.1f, 300.0f, (float)W, (float)H);
    dai_editor_ui *panels = dai_editor_ui_create(ed, ui);
    dai_editor_ui_project_host(panels, project_list, project_create, project_open, nullptr);
    dai_editor_ui_mesh_host(panels, mesh_name_of, DAI_MESH_BUILTIN_COUNT, nullptr);
    g_thumb_r = r;
    dai_editor_ui_thumb_host(panels, thumb_for, nullptr);
    dai_editor_ui_script_host(panels, script_create, nullptr);
    dai_editor_ui_rename_host(panels, asset_rename, nullptr);
    dai_editor_ui_import_host(panels, asset_import, nullptr);
    dai_editor_ui_delete_host(panels, asset_delete, nullptr);
    dai_editor_ui_file_host(panels, asset_read_text, asset_write_text, nullptr);
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
    }
    dai_editor_ui_script_editor_pref(panels, prefs.script_editor);
    dai_editor_ui_prefab_host(panels, prefab_save_cb);
    dai_editor_ui_open_asset_host(panels, open_asset_cb, nullptr);
    dai_editor_ui_layout_host(panels, layout_save_file, layout_load_file, nullptr);
    g_prefab_doc = doc;
    g_panels_for_log = panels;
    // The project was opened before the panels existed, so the show - if this
    // is one - gets attached now.
    show_open_for_project();
    dai_editor_ui_log(panels, 0, dai_version());
    dai_editor_ui_scene_host(panels, scene_list, scene_open, scene_save_as, nullptr);
#ifdef DAI_WITH_SCRIPT
    dai_editor_ui_params_host(panels, script_params_of, nullptr);
    g_script_ed = ed;
#endif
    dai_editor_ui_folder_host(panels, folder_create, nullptr);
    dai_editor_ui_material_host(panels, material_create, nullptr);
    dai_editor_ui_scale_host(panels, apply_ui_scale, g_dpi_pref, nullptr);
    g_panels_for_prefs = panels;
    g_psettings = &psettings;
    g_ui_for_settings = ui;
    dai_editor_ui_project_settings_host(panels, draw_project_settings, nullptr);

    // The layout is the user's and the PROJECT's: it lives in the project's
    // settings/ now (see layout_load_for_project), so a show and a game no
    // longer overwrite each other's arrangement. Loaded before the first
    // frame, so the editor opens the way this project was left.
    layout_load_for_project();

    // The asset layer: a mounted folder of glTF/JS, hot reloaded, bound to the
    // document sync so "asset crate.glb" on a node actually resolves.
    dai_assets *assets = dai_assets_create(r, 1);

    // Settings: the UI size is a font reload, and the font is ours.
    g_renderer = r; g_ui = ui; g_font = font;
    // A 150% desktop asks for 150% interface. The renderer already draws in
    // real pixels (that is what makes it sharp); without this the whole editor
    // is 2/3 the size of every other window on the machine, which reads as
    // "the resolution is halved and it looks blurry".
    {
        float dpi = dai_window_dpi_scale(win);
        if (dpi > 1.02f && prefs.dpi_scale < 0.05f) {
            prefs.dpi_scale = dpi;
            std::printf("dpi: display at %.0f%%, ui scaled to match\n", dpi * 100.0f);
        }
    }
    g_ui_scale_out = &prefs.ui_scale;
    g_prefs = &prefs;
    crash_handler_install(g_projects_root);
    dai_editor_ui_settings_host(panels, apply_font, 13.0f * prefs.ui_scale, nullptr);
    dai_editor_ui_loc_host(panels, loc_load_cb, loc_save_cb, panels);
    // THE line that was missing: the inspector's "@key" preview had no way to
    // resolve anything, so it said "no entry" about every key, including the
    // ones sitting right there in the Localisation window.
    dai_editor_ui_tr_host(panels, hud_resolve, nullptr);
    dai_hud_images(hud_image_cb, nullptr);
    dai_hud_vars(hud_var_cb, nullptr);
    // The font is rasterised at a REAL pixel size: 13 px laid out but drawn
    // from a texture made for 13 * 1.5 = 19.5 is sharp at 1.5x zoom, 13 * 1.5
    // = 19.5 rasterised but 13 laid out is the blurry one. Fractional font
    // sizes smear on their own, so the raster size is snapped UP to whole
    // pixels and the layout told to use exactly that - which is why the
    // toolbar icons (rasterised at a whole 16 px) were the one crisp thing
    // on screen.
    {
        float px = 13.0f * prefs.ui_scale;
        float snapped = px < 1.0f ? 1.0f : (float)((int)(px + 0.9999f));
        if (snapped != px && prefs.ui_scale > 0.0f) prefs.ui_scale = snapped / 13.0f;
        apply_font(snapped, nullptr);
    }
    dai_editor_cam_speed(ed, prefs.cam_speed > 0.1f ? prefs.cam_speed : 6.0f);
    if (prefs.gizmo_px > 10.0f) dai_editor_gizmo_size(ed, prefs.gizmo_px);
    if (prefs.snap_translate > 0.0f || prefs.snap_rotate_deg > 0.0f)
        dai_editor_snap(ed, prefs.snap_translate, prefs.snap_rotate_deg, 0.1f);

    auto last = std::chrono::high_resolution_clock::now();
    // One slot per entry in keys[] below - this was 8 while keys grew to 10,
    // and pressed(8)/pressed(9) read past the end of it. That is a heap buffer
    // overflow: on Windows it manifested as a startup crash (std::bad_alloc),
    // on Linux it quietly read whatever followed on the stack.
    int prev_keys[11] = { 0 };
    std::vector<dai_render_instance> inst(4096);
    int prev_f2 = 0, prev_backspace = 0, prev_enter = 0, prev_tab = 0;
    int prev_edit_keys[6] = { 0 };
    int prev_nav_keys[2] = { 0 };
    int prev_ctrl_a = 0;
    int prev_ctrl_p = 0;
    int prev_mouse_x = 0, prev_mouse_y = 0, mouse_seen = 0;

    int update_reported = 0;
    while (dai_window_poll(win) || !quit_is_ok(win, doc)) {
        if (!update_reported && g_update.state != 0) {
            update_reported = 1;
            std::printf("%s\n", g_update.note);
            // ...and into the Console panel, which is the only place a user of
            // a windowed program can actually read it.
            if (g_panels_for_log && g_update.note[0])
                dai_editor_ui_log(g_panels_for_log, (g_update.state >= 2) ? 1 : 0,
                                  g_update.note);
        }
        if (g_update.state == 3) break;   // staged and verified: hand over
        auto now = std::chrono::high_resolution_clock::now();
        float dt = std::chrono::duration<float>(now - last).count();
        last = now;

        int mx = 0, my = 0;
        uint32_t buttons = 0;
        dai_window_mouse(win, &mx, &my, &buttons);
        float wheel = dai_window_wheel(win);

        dai_editor_cam_input ci{};
        // Logical pointer: everything downstream of here - the UI, the gizmo,
        // picking - lays out in logical pixels, and a pointer in real ones
        // would miss every widget by the scale factor.
        ci.mouse_x = (float)mx / g_dpi; ci.mouse_y = (float)my / g_dpi;
        ci.mouse_left = (buttons & (1u << 1)) ? 1 : 0;
        ci.mouse_middle = (buttons & (1u << 2)) ? 1 : 0;
        ci.mouse_right = (buttons & (1u << 3)) ? 1 : 0;
        // The RAW button, kept before the camera's copy is blanked below. The
        // user interface must always know the truth about the pointer; only
        // the camera is allowed to be told a convenient lie.
        int raw_right = ci.mouse_right;
        if (dai_editor_ui_menu_open(panels)) ci.mouse_right = 0;
        ci.wheel = wheel;
        // The camera reads held keys; a field being typed into owns them first.
        int type_lock = dai_ui_text_active(ui);
        ci.key_w = !type_lock && dai_window_key_down(win, DAI_KEY_W);
        ci.key_a = !type_lock && dai_window_key_down(win, DAI_KEY_A);
        ci.key_s = !type_lock && dai_window_key_down(win, DAI_KEY_S);
        ci.key_d = !type_lock && dai_window_key_down(win, DAI_KEY_D);
        ci.key_q = !type_lock && dai_window_key_down(win, DAI_KEY_Q);
        ci.key_e = !type_lock && dai_window_key_down(win, DAI_KEY_E);
        ci.key_shift = dai_window_key_down(win, DAI_KEY_SHIFT_L) || dai_window_key_down(win, DAI_KEY_SHIFT_R);
        ci.key_alt = dai_window_key_down(win, DAI_KEY_ALT_L) || dai_window_key_down(win, DAI_KEY_ALT_R);
        ci.key_ctrl = dai_window_key_down(win, DAI_KEY_CTRL_L) || dai_window_key_down(win, DAI_KEY_CTRL_R);
        ci.key_focus = !type_lock && dai_window_key_down(win, DAI_KEY_F);
        ci.dt = dt;

        // W/E/R switch gizmo mode, but only when the camera is not flying -
        // otherwise pressing W to walk forward would also change the tool.
        // Pointer delta for scripts. Taken here, once, so every script in the
        // frame sees the SAME movement - asking the window per call would give
        // the second caller a delta of zero.
        {
            int mxr = 0, myr = 0; uint32_t mb = 0;
            if (dai_window_mouse(win, &mxr, &myr, &mb)) {
                g_mouse_dx = mouse_seen ? (double)(mxr - prev_mouse_x) : 0.0;
                g_mouse_dy = mouse_seen ? (double)(myr - prev_mouse_y) : 0.0;
                prev_mouse_x = mxr; prev_mouse_y = myr; mouse_seen = 1;
                g_mouse_buttons = (int)mb;
            } else {
                g_mouse_dx = g_mouse_dy = 0.0;
            }
        }
        int ctrl = dai_window_key_down(win, DAI_KEY_CTRL_L) || dai_window_key_down(win, DAI_KEY_CTRL_R);
        int keys[11] = {
            ci.key_w, ci.key_e, dai_window_key_down(win, DAI_KEY_R),
            dai_window_key_down(win, DAI_KEY_Z), dai_window_key_down(win, DAI_KEY_Y),
            dai_window_key_down(win, DAI_KEY_DELETE), dai_window_key_down(win, DAI_KEY_D),
            dai_window_key_down(win, DAI_KEY_SPACE),
            dai_window_key_down(win, DAI_KEY_S),
            dai_window_key_down(win, DAI_KEY_BACKSPACE),
            dai_window_key_down(win, (uint32_t)DAI_KEY_0 + 2),   // the enum names 0 and 9; between them it is ASCII
        };
        auto pressed = [&](int i) { return keys[i] && !prev_keys[i]; };
        // While a field is being typed into, the keyboard belongs to the field.
        // Otherwise renaming an object to "Wide Crate" switches the gizmo to
        // rotate, duplicates the selection and starts play mode on the way.
        int typing = dai_ui_typing(ui);
        if (!ci.mouse_right && !ctrl && !typing) {
            if (pressed(0)) dai_editor_gizmo_mode(ed, DAI_GIZMO_TRANSLATE);
            if (pressed(1)) dai_editor_gizmo_mode(ed, DAI_GIZMO_ROTATE);
            if (pressed(2)) dai_editor_gizmo_mode(ed, DAI_GIZMO_SCALE);
            // 2 toggles 2D, Unity's button on a key. It is a camera mode:
            // the scene does not change, the way you are looking at it does.
            if (pressed(10)) {
                int on = !dai_editor_cam_2d_get(ed);
                dai_editor_cam_2d(ed, on);
                dai_editor_ui_toast(panels, on ? "2D" : "3D", 1.0f);
            }
        }
        if (ctrl && pressed(3) && !typing) dai_editor_undo(ed);
        if (ctrl && pressed(4) && !typing) dai_editor_redo(ed);
        if (ctrl && pressed(6) && !typing) dai_editor_duplicate_selection(ed);
        // Ctrl+C / Ctrl+V: the selection travels as text, so it can leave the
        // editor (into a chat, another scene, another editor window).
        static int prev_copy = 0;
        if (ctrl && dai_window_key_down(win, (uint32_t)0x63 /* c */) && !typing &&
            dai_editor_selection_count(ed) > 0 && !prev_copy) {
            prev_copy = 1;
            dai_node sn = dai_editor_selected(ed, 0);
            dai_node_desc r{};
            if (dai_doc_get(doc, sn, &r) == DAI_OK) {
                std::string line = "node:";
                line += r.name[0] ? r.name : "node";
                line += " shape=" + std::to_string(r.shape);
                line += " pos=" + std::to_string((double)r.position.x) + "," +
                        std::to_string((double)r.position.y) + "," +
                        std::to_string((double)r.position.z);
                if (r.script[0]) { line += " script="; line += r.script; }
                dai_editor_ui_clipboard_set(panels, 1, line.c_str());
                dai_editor_ui_toast(panels, "copied node", 1.0f);
            }
        }
        prev_copy = ctrl && dai_window_key_down(win, (uint32_t)0x63);
        // Ctrl+V: a copied node comes back as a new one, with a unique name.
        static int prev_paste = 0;
        if (ctrl && dai_window_key_down(win, (uint32_t)0x76 /* v */) && !typing && !prev_paste) {
            prev_paste = 1;
            int kind = -1;
            const char *text = dai_editor_ui_clipboard_get(panels, &kind);
            char osbuf[4096];
            if (dai_window_clipboard_get(win, osbuf, sizeof(osbuf)) > 0 &&
                std::strncmp(osbuf, "node:", 5) == 0) {
                text = osbuf;
                kind = 1;   // a node line from another window or a chat
            }
            if (text && kind == 1 && std::strncmp(text, "node:", 5) == 0) {
                dai_node_desc r = dai_node_desc_default();
                // Parse the terse line back out: "node:NAME shape=S pos=x,y,z script=P"
                const char *p2 = text + 5;
                const char *sp = std::strstr(p2, " shape=");
                size_t nl = sp ? (size_t)(sp - p2) : std::strlen(p2);
                if (nl >= sizeof(r.name)) nl = sizeof(r.name) - 1;
                std::memcpy(r.name, p2, nl);
                r.name[nl] = 0;
                if (sp) r.shape = std::atoi(sp + 7);
                const char *pp = std::strstr(p2, " pos=");
                if (pp) std::sscanf(pp + 5, "%f,%f,%f", &r.position.x, &r.position.y, &r.position.z);
                const char *sc2 = std::strstr(p2, " script=");
                if (sc2) {
                    std::string spath = sc2 + 8;
                    if (spath.size() >= sizeof(r.script)) spath.resize(sizeof(r.script) - 1);
                    std::snprintf(r.script, sizeof(r.script), "%s", spath.c_str());
                }
                // Offset a little so the copy does not sit inside the original.
                r.position.x += 0.5f;
                dai_node made = dai_doc_add(doc, &r);
                if (made) { dai_editor_select(ed, made, 0); dai_editor_resync(ed); }
                dai_editor_ui_toast(panels, "pasted node", 1.0f);
            }
        }
        if (!(ctrl && dai_window_key_down(win, (uint32_t)0x76))) prev_paste = 0;
        // Not while a field is being typed into: Delete belongs to the caret
        // then, not to the scene.
        // Delete, and Backspace for the keyboards that have no Delete key.
        // Never while typing: there the two belong to the caret.
        // Delete belongs to whichever window the pointer is over. The Project
        // window takes it for the file that is selected there; otherwise it is
        // the scene's, as it always was.
        if ((pressed(5) || pressed(9)) && !typing) {
            if (!dai_editor_ui_delete_project_pick(panels))
                dai_editor_delete_selection(ed);
        }
        // Play/pause. Ctrl+P always, Space only while EDITING.
        //
        // Space used to toggle play in both states, so the first jump in any
        // game paused the editor: the same key was the game's and the
        // editor's at the same moment, and the editor won. Unity's binding is
        // Ctrl+P for exactly this reason, and while the game is running the
        // keyboard belongs to the game.
        int p_key = dai_window_key_down(win, 'p');
        int ctrl_p = ctrl && p_key && !prev_ctrl_p;
        prev_ctrl_p = p_key;
        int playing_now = dai_editor_state_get(ed) == DAI_EDITOR_PLAY;
        if ((ctrl_p || (pressed(7) && !playing_now)) && !typing) {
            if (playing_now) dai_editor_pause(ed);
            else dai_editor_play(ed);
        }
        // Ctrl+S: once per press, never while a field has the keyboard, and
        // it says so. Held down it used to write the file sixty times a second
        // and tell nobody - a save you cannot see is a save you do not trust.
        // Ctrl+S while a script is being edited saves the SCRIPT. Anything
        // else and it is the scene, as it always was. `typing` is false for
        // the code editor on purpose - Ctrl+S is not a character.
        // ...and it only means the SCRIPT when the code editor actually has the
        // keyboard. It used to try the script first, unconditionally: with any
        // file open in the Script tab, Ctrl+S anywhere in the editor wrote that
        // file and reported "saved playercontroller.js" while the scene - the
        // thing you had just changed - stayed on disk as it was. A save that
        // saves something else is worse than no save.
        int code_has_kb = dai_ui_code_focused(ui);
        if (ctrl && pressed(8) && code_has_kb && dai_editor_ui_script_save(panels)) {
            // handled by the script panel
        } else if (ctrl && pressed(8) && !typing) {
            const char *sp = scene_path ? scene_path : (g_scene_path[0] ? g_scene_path : nullptr);
            char msg[192];
            if (sp && dai_doc_save(doc, sp) == DAI_OK) {
                g_saved_rev = dai_doc_revision(doc);
                const char *base = std::strrchr(sp, '/');
                const char *bs2 = std::strrchr(sp, '\\');
                if (bs2 && (!base || bs2 > base)) base = bs2;
                std::snprintf(msg, sizeof(msg), "saved %s", base ? base + 1 : sp);
                std::printf("%s\n", msg);
            } else {
                std::snprintf(msg, sizeof(msg), "could not save - open a project first");
            }
            dai_editor_ui_toast(panels, msg, 2.0f);
        }
        // Watchdog: once a minute, print how much memory the process holds.
        // A growing count in a crash loop points at the leak; a flat count
        // means the crash is a bug, not a leak.
        {
#ifdef _WIN32
            // Frames are not seconds: at 150 fps a 60-frame counter printed
            // two lines a second into Justin's console. Real clock, and only
            // when DAIDALOS_DIAG asked for it - a healthy editor says nothing.
            if (g_diag_on > 0) {
                static ULONGLONG mem_next = 0;
                ULONGLONG now_ms = GetTickCount64();
                if (now_ms >= mem_next) {
                    mem_next = now_ms + 60000;
                    PROCESS_MEMORY_COUNTERS pmc{};
                    pmc.cb = sizeof(pmc);
                    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc)))
                        std::printf("mem: %.1f MB\n", pmc.WorkingSetSize / 1048576.0);
                }
            }
#endif
        }
        std::memcpy(prev_keys, keys, sizeof(keys));

        // ---- project switching: the callbacks set g_scene_path, the loop
        //      turns it into a loaded scene and a mounted assets folder.
        static char current_scene[512] = { 0 };
        if (std::strcmp(g_scene_path, current_scene) != 0 && g_scene_path[0]) {
            std::snprintf(current_scene, sizeof(current_scene), "%s", g_scene_path);
            scene_path = current_scene;
            dai_doc_clear(doc);
            char lerr[256] = { 0 };
            if (dai_doc_load(doc, current_scene, lerr, sizeof(lerr)) != DAI_OK) {
                // A project with no scene yet is a new project, not a broken one.
                dai_node_desc g2 = dai_node_desc_default();
                std::snprintf(g2.name, sizeof(g2.name), "Ground");
                g2.motion = DAI_STATIC;
                g2.half_extent = { 12, 0.5f, 12 };
                g2.position = { 0, -0.5f, 0 };
                g2.color = { 0.20f, 0.22f, 0.19f };
                dai_doc_add(doc, &g2);
            }
            dai_editor_deselect_all(ed);
            g_saved_rev = dai_doc_revision(doc);   // freshly loaded IS saved
            dai_doc_sync_reset(sync);
            dai_doc_sync_apply(sync);
            // <project>/assets is the mounted folder. Asked of the project,
            // not carved out of the scene path with strstr("/scenes/") - the
            // scene lives INSIDE assets now, so that substring is gone and
            // the old code would have stopped remounting anything at all.
            if (g_project) {
                std::snprintf(g_assets_dir, sizeof(g_assets_dir), "%s",
                              dai_project_asset_dir(g_project));
                if (assets) {
                    dai_assets_destroy(assets);
                    assets = dai_assets_create(r, 1);
                    if (assets) {
                        dai_assets_mount_dir(assets, g_assets_dir, 0);
                        dai_assets_bind(assets, sync);
                    }
                }
            }
            // Prefab instances hold a path, not a subtree - expanding them
            // is what turns the reference back into objects. Nothing did it,
            // so every prefab in a saved scene came back empty.
            if (g_assets_dir[0]) {
                uint32_t nrb = dai_doc_prefab_reload(doc, g_assets_dir);
                if (nrb) {
                    dai_doc_sync_reset(sync);
                    dai_doc_sync_apply(sync);
                    char pm[96];
                    std::snprintf(pm, sizeof(pm), "%u prefab instance%s expanded",
                                  nrb, nrb == 1 ? "" : "s");
                    dai_editor_ui_log(panels, 0, pm);
                }
            }
            {   // the hierarchy's root row says which scene is open
                const char *base = std::strrchr(current_scene, '/');
                const char *bs = std::strrchr(current_scene, '\\');
                if (bs && (!base || bs > base)) base = bs;
                dai_editor_ui_scene_label(panels, base ? base + 1 : current_scene);
            }
            std::printf("project: %s\n", current_scene);
        }

        if (dai_editor_ui_take_save(panels)) {
            // In a droneshow project Ctrl+S means the show. There is no scene
            // to write, and writing an empty one instead would be a save that
            // silently threw the work away.
            if (g_show) show_save();
            const char *sp = scene_path ? scene_path : (g_scene_path[0] ? g_scene_path : nullptr);
            if (!sp) {
                // Nowhere to put it. Silence here is what made "Save first"
                // look like it did nothing: the dialog waited for the scene to
                // stop being dirty, and it never would.
                dai_editor_ui_toast(panels, "this scene has no file yet - use Save As", 3.0f);
                dai_editor_ui_log(panels, 2, "save: the scene has no path");
            } else if (dai_doc_save(doc, sp) == DAI_OK) {
                // THE line that was missing. Without it the document stays
                // "dirty" after a successful save for ever: the title keeps
                // its asterisk, and anything that waits for the save to land -
                // the unsaved changes dialog's "Save first" - waits until it
                // gives up and says the scene did not save.
                g_saved_rev = dai_doc_revision(doc);
                dai_editor_ui_scene_dirty(panels, 0);
                std::printf("saved %s\n", sp);
                dai_editor_ui_toast(panels, "scene saved", 2.0f);
            } else {
                dai_editor_ui_toast(panels, "the scene could not be saved", 3.0f);
                char line[700];
                std::snprintf(line, sizeof(line), "save failed: %s", sp);
                dai_editor_ui_log(panels, 2, line);
            }
        }
        // A refresh means the Project window WROTE files (new script/folder).
        // The list below only re-feeds on a revision change and a new file
        // does not move the revision - so force the re-feed here.
        static uint32_t fed_rev = 0xFFFFFFFFu;
        // One new thumbnail per frame, so opening a folder of models does not
        // stall the frame it was opened in.
        g_thumb_budget = 1;
        if (dai_editor_ui_take_refresh(panels) && assets) {
            thumbs_forget_all();
            dai_assets_poll(assets);
            fed_rev = 0xFFFFFFFFu;
            apply_materials(doc);       // a refresh is also "the files moved"
            dai_doc_sync_apply(sync);
        }
        if (dai_editor_ui_take_material_apply(panels)) {
            apply_materials(doc);
            dai_doc_sync_apply(sync);
        }

        // ---- assets: hot reload, list, and what the Project window clicked
        if (assets) {
            if (dai_assets_poll(assets)) dai_assets_bind(assets, sync);
            uint32_t rev = dai_assets_revision(assets);
            if (rev != fed_rev) {
                fed_rev = rev;
                static const char *ptrs[256];
                uint32_t n2 = dai_assets_list(assets, g_asset_paths[0], 256, 96);
                if (n2 > 256) n2 = 256;
                for (uint32_t i = 0; i < n2; ++i) ptrs[i] = g_asset_paths[i];
                g_asset_count = n2;
                dai_editor_ui_asset_list(panels, ptrs, n2);
                // Folders too - the browser cannot see an empty one otherwise.
                if (g_assets_dir[0]) {
                    static char dirs[256][160];
                    static const char *dptrs[256];
                    uint32_t dn = 0;
                    list_dirs_rec(g_assets_dir, "", dirs, &dn, 256, 0);
                    for (uint32_t i = 0; i < dn; ++i) dptrs[i] = dirs[i];
                    dai_editor_ui_folder_list(panels, dptrs, dn);
                }
            }
            // ---- prefab mode: open a prefab as the scene, and come back --
            {
                const char *popen = nullptr;
                if (dai_editor_ui_take_prefab_open(panels, &popen) && popen && *popen) {
                    // Save what is open first: leaving a scene without writing
                    // it is how an afternoon disappears.
                    if (g_scene_path[0] && dai_doc_save(doc, g_scene_path) == DAI_OK)
                        g_saved_rev = dai_doc_revision(doc);
                    if (!g_prefab_return[0])
                        std::snprintf(g_prefab_return, sizeof(g_prefab_return), "%s", g_scene_path);
                    char full2[700];
                    std::snprintf(full2, sizeof(full2), "%s/%s", g_assets_dir, popen);
                    std::snprintf(g_scene_path, sizeof(g_scene_path), "%s", full2);
                    dai_editor_ui_prefab_mode(panels, popen);
                }
                if (dai_editor_ui_take_prefab_exit(panels)) {
                    // Back to the world, saving the prefab on the way out -
                    // which is what makes "edit the prefab" mean anything.
                    if (g_scene_path[0] && dai_doc_save(doc, g_scene_path) == DAI_OK)
                        g_saved_rev = dai_doc_revision(doc);
                    if (g_prefab_return[0]) {
                        std::snprintf(g_scene_path, sizeof(g_scene_path), "%s", g_prefab_return);
                        g_prefab_return[0] = 0;
                    }
                    dai_editor_ui_prefab_mode(panels, nullptr);
                    // Instances in the scene are expanded from the file, so
                    // the edit shows up everywhere the moment it is reloaded.
                    dai_doc_prefab_reload(doc, g_assets_dir);
                }
            }
            const char *pick = nullptr;
            int as_tree = 0;
            if (dai_editor_ui_take_asset(panels, &pick, &as_tree) && pick) {
                size_t pl = std::strlen(pick);
                bool in_scenes = std::strncmp(pick, "Scenes/", 7) == 0 ||
                                 std::strncmp(pick, "scenes/", 7) == 0;
                if (pl > 9 && std::strcmp(pick + pl - 9, ".daidalos") == 0 && in_scenes) {
                    // A scene file OPENS. Instantiating it as a prefab would
                    // paste the whole level into the level you are standing
                    // in, which is never what a double click on a scene means.
                    if (scene_open(pick, nullptr))
                        dai_editor_ui_toast(panels, "opening scene", 1.5f);
                } else if (pl > 9 && std::strcmp(pick + pl - 9, ".daidalos") == 0) {
                    // A prefab is a scene file: instantiate it under the
                    // current selection, select the copy, done. The model
                    // loader has never heard of one, which is why dropping
                    // a prefab in used to do nothing at all.
                    char full[700];
                    std::snprintf(full, sizeof(full), "%s/%s", g_assets_dir, pick);
                    char perr[256] = { 0 };
                    // The path is stored AS GIVEN, so it goes in relative to
                    // the assets folder and the scene stays portable; the
                    // base dir is how it is found again.
                    (void)full;
                    dai_node made = DAI_INVALID_NODE;
                    if (g_preview_node != DAI_INVALID_NODE && g_preview_path == pick) {
                        made = g_preview_node;            // the drag already placed it
                        g_preview_node = DAI_INVALID_NODE;
                        g_preview_path.clear();
                    } else {
                        made = dai_doc_prefab_instantiate(doc, pick, DAI_INVALID_NODE,
                                                          g_assets_dir, perr, sizeof(perr));
                    }
                    if (made) {
                        // Dropped into the viewport? Then it goes where it was
                        // dropped. The prefab file stores its root at its own
                        // origin, so this is the whole placement.
                        float dx = 0, dy = 0, dz = 0;
                        if (dai_editor_ui_take_asset_at(panels, &dx, &dy, &dz)) {
                            dai_node_desc pr4{};
                            if (dai_doc_get(doc, made, &pr4) == DAI_OK) {
                                dai_doc_begin(doc, "Place prefab");
                                pr4.position = dai_vec3{ dx, dy + pr4.half_extent.y, dz };
                                dai_doc_set(doc, made, &pr4);
                                dai_doc_commit(doc);
                            }
                        }
                        dai_doc_sync_apply(sync);
                        dai_editor_select(ed, made, 0);
                        dai_editor_ui_toast(panels, "prefab placed", 1.5f);
                    } else {
                        char m[400];
                        std::snprintf(m, sizeof(m), "prefab failed: %s",
                                      perr[0] ? perr : "unreadable");
                        dai_editor_ui_log(panels, 2, m);
                    }
                } else if (dai_assets_model_blocking(assets, pick)) {
                    // The drag already put this model in the scene and has
                    // been moving it with the pointer - see the preview block
                    // below. Then it IS the placement: keeping it is one
                    // object and one undo step, instantiating a second one
                    // would leave the first behind at the same spot.
                    dai_node made = DAI_INVALID_NODE;
                    if (g_preview_node != DAI_INVALID_NODE && g_preview_path == pick) {
                        made = g_preview_node;
                        g_preview_node = DAI_INVALID_NODE;   // adopted, not undone
                        g_preview_path.clear();
                    } else {
                        made = dai_assets_instantiate(assets, doc, pick, 0);
                    }
                    if (made) {
                        float dx = 0, dy = 0, dz = 0;
                        if (dai_editor_ui_take_asset_at(panels, &dx, &dy, &dz)) {
                            dai_node_desc mr5{};
                            if (dai_doc_get(doc, made, &mr5) == DAI_OK) {
                                mr5.position = dai_vec3{ dx, dy, dz };
                                dai_doc_set(doc, made, &mr5);
                            }
                        }
                        dai_doc_sync_apply(sync);
                        dai_editor_select(ed, made, 0);
                    }
                }
            }
        }

        // ---- the model under the pointer, actually in the scene ----------
        // Unity's trick, and it is not a trick: while you drag a model over
        // the viewport the model IS in the scene, moving with the pointer.
        // Let go and it stays; drag back out and it is undone. No ghost, no
        // outline, no guessing at the size of a thing you have not seen yet.
        {
            const char *pvp = nullptr;
            float pvx = 0, pvy = 0, pvz = 0;
            if (dai_editor_ui_drag_preview(panels, &pvp, &pvx, &pvy, &pvz) && pvp) {
                if (g_preview_path != pvp) {
                    preview_drop(doc, sync);              // a different file: start over
                    size_t pl2 = std::strlen(pvp);
                    bool is_prefab = pl2 > 9 && std::strcmp(pvp + pl2 - 9, ".daidalos") == 0;
                    dai_node made = DAI_INVALID_NODE;
                    if (is_prefab) {
                        char perr2[256] = { 0 };
                        made = dai_doc_prefab_instantiate(doc, pvp, DAI_INVALID_NODE,
                                                          g_assets_dir, perr2, sizeof(perr2));
                    } else if (dai_assets_model_blocking(assets, pvp)) {
                        made = dai_assets_instantiate(assets, doc, pvp, 0);
                    }
                    if (made) {
                        g_preview_node = made;
                        g_preview_path = pvp;
                        // The undo step the instantiate just pushed is the one
                        // preview_drop() will pop if the drag is abandoned.
                        g_preview_undo = dai_doc_undo_depth(doc);
                    }
                }
                if (g_preview_node != DAI_INVALID_NODE) {
                    dai_node_desc pv{};
                    if (dai_doc_get(doc, g_preview_node, &pv) == DAI_OK) {
                        dai_vec3 want{ pvx, pvy, pvz };
                        if (pv.position.x != want.x || pv.position.y != want.y ||
                            pv.position.z != want.z) {
                            // No dai_doc_begin: sixty positions a second must
                            // not be sixty entries on the undo stack.
                            pv.position = want;
                            dai_doc_set(doc, g_preview_node, &pv);
                            dai_doc_sync_apply(sync);
                        }
                    }
                }
            } else if (g_preview_node != DAI_INVALID_NODE) {
                // The drag left the viewport, or was let go somewhere else.
                // take_asset above already adopted it if it was a real drop.
                preview_drop(doc, sync);
            }
        }

        console_capture_pump(panels);
        diag_step("frame begin");
        uint32_t ww = W, wh = H;
        dai_window_size(win, &ww, &wh);

        // Follow the window. The frame is blitted onto it and stretched, so a
        // fixed render resolution means a picture of the wrong shape (a 16:9
        // frame across a 21:9 monitor) drawn with soft, scaled up text - and
        // the UI, which lays itself out in window pixels, would be drawing into
        // a buffer of a different size. One resolution for the window, the
        // renderer and the interface is the only arrangement where all three
        // agree.
        // Logical size: the renderer owns real pixels, the interface owns
        // logical ones, and the scale is the only bridge between them.
        const float uis = g_dpi;   // Settings can change this between frames
        const float lw = (float)ww / uis, lh = (float)wh / uis;
        if (ww != dai_render_width(r) || wh != dai_render_height(r)) {
            if (dai_render_resize(r, ww, wh) == DAI_OK)
                dai_editor_camera_viewport(ed, lw, lh);
            // The pointer is reported in RENDER pixels, and that scale just
            // changed - so the same physical mouse position is a different
            // number this frame. A resize is done with the button held, so
            // without re-anchoring the camera spins while the window grows.
            int amx = 0, amy = 0;
            dai_window_mouse(win, &amx, &amy, nullptr);
            dai_editor_cam_anchor(ed, (float)amx / uis, (float)amy / uis);
        }

        // What the desktop dropped on us since the last frame. The editor
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
        // panel" is only known once the panels have been laid out this frame.
        // F2 renames the selection, in the hierarchy where the name lives.
        if (dai_window_key_down(win, DAI_KEY_F2) && !prev_f2 && !dai_ui_typing(ui)) {
            // F2 follows THE selection, and there is only one. Selecting an
            // object used to leave the Project window's own highlight
            // standing, and F2 asked the Project window first - so with a
            // crate selected in the hierarchy, F2 renamed a file.
            //
            // The object wins when one is selected, because selecting an
            // object now clears the file pick (see the click handler), so
            // "an object is selected" really does mean the last thing you
            // touched was an object.
            if (dai_editor_selection_count(ed) > 0)
                dai_editor_ui_rename(panels, dai_editor_selected(ed, 0));
            else
                dai_editor_ui_rename_project_pick(panels);
        }
        prev_f2 = dai_window_key_down(win, DAI_KEY_F2);

        // Real text events. dai_window_key_down answers "is it held", which
        // is the camera's question - the inspector's question is "what was
        // typed", and answering that with held keys repeats every letter for
        // as long as the finger is down. The window backend saw the key
        // press events; it hands them over as code points, already
        // shift-resolved. */
        uint32_t typed[8] = { 0 };
        dai_window_text(win, typed, 8);

        // THE bug behind "I can only right click once". This line used to read
        //     in.right_down = menu_open ? 0 : ci.mouse_right;
        // so while a menu was open the editor told the UI, in as many words,
        // that the right button did not exist. Six fixes went in above this
        // line - in the viewport, in the hierarchy, in the popup itself - and
        // every one of them was reasoning about a click that had already been
        // erased one layer down.
        //
        // What that line was actually for is real and stays: the CAMERA must
        // not turn when you dismiss a menu with the same button that opened
        // it. That is a fact about the camera, so it is told to the camera -
        // ci.mouse_right is cleared above for exactly that - and not to the
        // whole user interface.
        dai_ui_input in{};
        in.mouse_x = (float)mx / uis; in.mouse_y = (float)my / uis;
        in.mouse_down = ci.mouse_left;
        in.right_down = raw_right;
        in.wheel = wheel;
        std::memcpy(in.text, typed, sizeof(in.text));
        in.key_backspace = dai_window_key_down(win, DAI_KEY_BACKSPACE) && !prev_backspace;
        prev_backspace = dai_window_key_down(win, DAI_KEY_BACKSPACE);
        in.key_enter = dai_window_key_down(win, DAI_KEY_RETURN) && !prev_enter;
        prev_enter = dai_window_key_down(win, DAI_KEY_RETURN);
        in.key_tab = dai_window_key_down(win, DAI_KEY_TAB) && !prev_tab;
        prev_tab = dai_window_key_down(win, DAI_KEY_TAB);
        // The rest of what a real text field needs. Edge triggered, like the
        // three above: a held arrow key that moves the caret every frame is
        // not "repeat", it is a caret that teleports.
        {
            static const uint32_t EDGE_KEYS[6] = { DAI_KEY_LEFT, DAI_KEY_RIGHT, DAI_KEY_HOME,
                                                   DAI_KEY_END, DAI_KEY_DELETE, DAI_KEY_ESCAPE };
            int *out[6] = { &in.key_left, &in.key_right, &in.key_home,
                            &in.key_end, &in.key_delete, &in.key_escape };
            for (int i = 0; i < 6; ++i) {
                int now = dai_window_key_down(win, EDGE_KEYS[i]);
                *out[i] = now && !prev_edit_keys[i];
                prev_edit_keys[i] = now;
            }
        }
        {
            static const uint32_t NAV[2] = { DAI_KEY_UP, DAI_KEY_DOWN };
            int *nav_out[2] = { &in.key_up_arrow, &in.key_down_arrow };
            for (int i = 0; i < 2; ++i) {
                int now = dai_window_key_down(win, NAV[i]);
                *nav_out[i] = now && !prev_nav_keys[i];
                prev_nav_keys[i] = now;
            }
        }
        in.key_shift = ci.key_shift;
        in.key_ctrl = ctrl;
        {
            int a_now = ctrl && dai_window_key_down(win, DAI_KEY_A);
            in.key_select_all = a_now && !prev_ctrl_a;
            prev_ctrl_a = a_now;
        }
        {
            // The clipboard three, the same way: read off the KEY, edge
            // triggered. The text stream never carries them - see dai_ui.h.
            static int prev_c = 0, prev_x = 0, prev_v = 0;
            int c_now = ctrl && dai_window_key_down(win, (uint32_t)'c');
            int x_now = ctrl && dai_window_key_down(win, (uint32_t)'x');
            int v_now = ctrl && dai_window_key_down(win, (uint32_t)'v');
            in.key_copy  = c_now && !prev_c;
            in.key_cut   = x_now && !prev_x;
            in.key_paste = v_now && !prev_v;
            prev_c = c_now; prev_x = x_now; prev_v = v_now;
        }
        in.double_click = dai_window_double_click(win);
        // The camera step runs BEFORE the UI frame on purpose: the gizmo and
        // the collider lines are generated inside dai_editor_ui_frame, and
        // with the old order they were projected with last frame's camera -
        // visibly trailing the scene by one frame while moving. The "is the
        // pointer over a panel" answer this consumes is then one layout old,
        // the same staleness the menu check above already relies on.
        dai_editor_ui_viewport(panels, &ci);

        // A droneshow preview flies like the scene view: the same struct the
        // scene camera just read, handed to the show while the pointer is
        // over its viewport - and kept coming while a look or a pan is held,
        // because a gesture that ends at the panel edge is a gesture that
        // sticks. dai_ui itself never sees a held key, which is why this is
        // the host's job and not the panel's.
        if (g_show_ui) {
            float vrx = 0, vry = 0, vrw = 0, vrh = 0;
            dai_editor_ui_viewport_rect(panels, &vrx, &vry, &vrw, &vrh);
            static int nav_held = 0;
            int over_vp = ci.mouse_x >= vrx && ci.mouse_x < vrx + vrw &&
                          ci.mouse_y >= vry && ci.mouse_y < vry + vrh;
            int want = (over_vp && (raw_right || ci.mouse_middle || ci.key_focus)) ||
                       (nav_held && (raw_right || ci.mouse_middle));
            if (want) {
                dai_show_nav_input ni{};
                ni.mouse_x      = ci.mouse_x;
                ni.mouse_y      = ci.mouse_y;
                ni.mouse_right  = raw_right;
                ni.mouse_middle = ci.mouse_middle;
                ni.key_w = ci.key_w; ni.key_a = ci.key_a;
                ni.key_s = ci.key_s; ni.key_d = ci.key_d;
                ni.key_q = ci.key_q; ni.key_e = ci.key_e;
                ni.key_shift = ci.key_shift;
                ni.key_focus = ci.key_focus;
                ni.dt = dt;
                dai_show_ui_nav(g_show_ui, &ni);
            }
            nav_held = raw_right || ci.mouse_middle;
        }

        diag_step("ui begin");
        // The OS clipboard, in - and whatever a widget copied, out. One
        // frame's round trip, so Ctrl+C in the script editor lands in the
        // same clipboard everything else on the machine uses.
        {
            char cb[64 * 1024];
            uint32_t cn = dai_window_clipboard_get(win, cb, sizeof(cb));
            dai_ui_clipboard_feed(ui, cn ? cb : "");
        }
        dai_ui_begin(ui, lw, lh, &in);
        diag_step("ui frame (panels)");
        // The frame rate, smoothed over about half a second. Averaged HERE
        // rather than in the editor because this is where the clock is - and
        // an unsmoothed counter is a number nobody can read.
        {
            static float fps_avg = 0.0f;
            if (dt > 0.0001f) {
                float inst = 1.0f / dt;
                float k = dt / (0.5f + dt);        // ~0.5 s time constant
                fps_avg = fps_avg <= 0.0f ? inst : fps_avg + (inst - fps_avg) * k;
            }
            dai_editor_ui_fps(panels, fps_avg);
        }

        if (const char *taken = dai_ui_clipboard_taken(ui))
            dai_window_clipboard_set(win, taken);

        // The About block in Settings: three strings, pushed every frame
        // because they cost nothing and can never then be stale.
        {
            static char upd_status[160] = { 0 };
            if (dai_editor_ui_take_update_check(panels)) {
                // The editor updates itself against the download page's
                // manifest on start-up - that machinery lives in dai_update
                // and needs a URL and an install dir this build does not
                // carry. So this button is honest about what it can do: it
                // opens the page, where the version on offer is written down.
                std::snprintf(upd_status, sizeof(upd_status),
                              "opened the download page - it updates itself on restart");
                dai_editor_ui_log(panels, 0, "opening https://daidalos.fleitec.com");
#ifdef _WIN32
                ShellExecuteA(nullptr, "open", "https://daidalos.fleitec.com",
                              nullptr, nullptr, SW_SHOWNORMAL);
#else
                (void)std::system("xdg-open https://daidalos.fleitec.com >/dev/null 2>&1 &");
#endif
            }
            dai_editor_ui_about(panels, g_projects_root,
                                g_assets_dir[0] ? g_assets_dir : "", upd_status);
        }

        // Unsaved? The asterisk in the hierarchy comes from here.
        dai_editor_ui_scene_dirty(panels, dai_doc_revision(doc) != g_saved_rev);

        // The interface, under the net. A fault here abandons ONE frame and
        // says what it was; the scene, the undo history and everything you
        // have not saved are still in memory afterwards.
        {
            struct FrameArgs { dai_editor_ui *p; float w, h; } fa{ panels, lw, lh };
            auto draw = [](void *u) {
                FrameArgs *a = (FrameArgs *)u;
                dai_editor_ui_frame(a->p, a->w, a->h);
            };
            if (guard_run(draw, &fa)) {
                g_guard_faults = 0;          // a clean frame clears the count
            } else {
                char line[256];
                std::snprintf(line, sizeof(line),
                              "interface fault 0x%lx at %p - frame skipped (%d in a row)",
                              g_guard_last_code, g_guard_last_addr, g_guard_faults);
                dai_editor_ui_log(panels, 2, line);
                std::printf("%s\n", line);
                if (g_guard_faults >= 8) {
                    dai_editor_ui_log(panels, 2,
                        "too many faults in a row - saving a report and stopping");
                    std::printf("giving up after %d consecutive faults\n", g_guard_faults);
                }
            }
        }

        // Copies made anywhere in the UI (console line, component, node)
        // travel to the OS clipboard here: rev bumped means something new was
        // copied this frame, and a copy you cannot paste into a chat is not
        // a copy. Linux returns 0 from the bridge and nothing breaks.
        {
            static unsigned last_clip_rev = 0;
            unsigned crev = dai_editor_ui_clipboard_rev(panels);
            if (crev != last_clip_rev) {
                last_clip_rev = crev;
                const char *ct = dai_editor_ui_clipboard_get(panels, nullptr);
                if (ct && ct[0]) dai_window_clipboard_set(win, ct);
            }
        }
        // What the script GUI needs to answer a button: where the pointer is
        // and whether it was let go this frame, in the view the game is shown
        // in. Taken here, once, before anything is drawn.
        {
            float gx2, gy2, gw2, gh2;
            if (!dai_editor_ui_game_view_rect(panels, &gx2, &gy2, &gw2, &gh2))
                dai_editor_ui_viewport_rect(panels, &gx2, &gy2, &gw2, &gh2);
            g_gui_x = gx2; g_gui_y = gy2; g_gui_w = gw2; g_gui_h = gh2;
            int down = 0;
            dai_ui_mouse(ui, &g_gui_mx, &g_gui_my, &down, nullptr);
            g_gui_released = (!down && g_gui_down) ? 1 : 0;
            g_gui_down = down;
        }

        // ---- dragging the selected UI element ----------------------------
        //
        // A frame around what is selected and EIGHT grips - four corners and
        // four edges, the way every editor that has ever let you resize a
        // rectangle does it. One corner grip meant the only way to change the
        // left edge was to change the right one and then move the whole thing
        // back, twice, until it looked right.
        //
        // Moving writes the OFFSET, not a position: the anchor is what makes
        // a HUD survive a different window size, and a drag that replaced it
        // with absolute pixels would quietly undo that.
        //
        // Resizing writes BOTH, and it has to. Where a box is drawn depends on
        // its size - an element anchored right moves left when it grows - so
        // dragging the right edge alone would drag the left one with it. The
        // offset is corrected by exactly the amount the anchor shifts it:
        // for an anchor column f (0 left, 0.5 centre, 1 right),
        //     right edge by dx  ->  w += dx,  off_x += f*dx
        //     left  edge by dx  ->  w -= dx,  off_x += (1-f)*dx
        // which is the algebra of the layout above, not a fudge factor.
        static dai_node ui_drag_node = DAI_INVALID_NODE;
        static int   ui_drag_kind = 0;         // 0 none, 1 move, 2 resize
        static int   ui_drag_ex = 0, ui_drag_ey = 0;   // -1 left/top, +1 right/bottom
        static float ui_drag_x0 = 0, ui_drag_y0 = 0;
        static float ui_drag_ox = 0, ui_drag_oy = 0, ui_drag_w0 = 0, ui_drag_h0 = 0;
        static int   ui_drag_img = 0;      // 1 = the Image's rectangle, 0 = the Text's
        if (dai_editor_selection_count(ed) > 0) {
            dai_node sel_n = dai_editor_selected(ed, 0);
            float rx, ry, rw, rh;
            int rkind = 0;
            // A node can have BOTH an Image and a Text, and they are two
            // rectangles in two places. Every one of them gets a frame and
            // grips; before this only the first was findable, so the other
            // one could not be selected, moved or resized at all.
            for (int ri = 0; dai_hud_rect_nth(sel_n, ri, &rkind, &rx, &ry, &rw, &rh); ++ri) {
                dai_node_desc sr{};
                int have = dai_doc_get(doc, sel_n, &sr) == DAI_OK;
                float mx2 = 0, my2 = 0;
                int mdown = 0, mpress = 0;
                dai_ui_mouse(ui, &mx2, &my2, &mdown, &mpress);

                // Which of the two this rectangle IS - not "does the node
                // have an image", which is what it used to ask and why a
                // node with both wrote the image's numbers when you dragged
                // its label.
                bool img = have && rkind == 0;
                // A label that is as wide as its words has nothing to resize;
                // giving it a box IS the resize, so the grips are offered and
                // the first drag writes the box the words are in now.
                bool sizable = have && (img || sr.text_on);

                const float G = 9.0f, HALF = G * 0.5f;
                struct Grip { int ex, ey; float cx, cy; };
                Grip grips[8] = {
                    { -1, -1, rx,            ry            },
                    {  0, -1, rx + rw * 0.5f, ry           },
                    { +1, -1, rx + rw,       ry            },
                    { -1,  0, rx,            ry + rh * 0.5f },
                    { +1,  0, rx + rw,       ry + rh * 0.5f },
                    { -1, +1, rx,            ry + rh       },
                    {  0, +1, rx + rw * 0.5f, ry + rh      },
                    { +1, +1, rx + rw,       ry + rh       },
                };

                dai_ui_layer_push(ui, DAI_LAYER_WINDOW + 6);
                dai_ui_rect_outline(ui, rx - 1.0f, ry - 1.0f, rw + 2.0f, rh + 2.0f, 1.0f,
                                    0xFF3D84D8u);
                if (sizable)
                    for (const Grip &g : grips) {
                        dai_ui_rect(ui, g.cx - HALF, g.cy - HALF, G, G, 0xFFFFFFFFu);
                        dai_ui_rect_outline(ui, g.cx - HALF, g.cy - HALF, G, G, 1.0f, 0xFF3D84D8u);
                    }
                dai_ui_layer_pop(ui);

                int hit = -1;
                if (sizable)
                    for (int gi = 0; gi < 8; ++gi) {
                        const Grip &g = grips[gi];
                        if (mx2 >= g.cx - HALF - 2.0f && mx2 < g.cx + HALF + 2.0f &&
                            my2 >= g.cy - HALF - 2.0f && my2 < g.cy + HALF + 2.0f) { hit = gi; break; }
                    }
                bool over_body = mx2 >= rx - 2.0f && mx2 < rx + rw + 2.0f &&
                                 my2 >= ry - 2.0f && my2 < ry + rh + 2.0f;

                if (mpress && have && (hit >= 0 || over_body)) {
                    ui_drag_node = sel_n;
                    ui_drag_img = img ? 1 : 0;
                    ui_drag_x0 = mx2; ui_drag_y0 = my2;
                    ui_drag_ox = img ? sr.image_x : sr.text_x;
                    ui_drag_oy = img ? sr.image_y : sr.text_y;
                    if (hit >= 0) {
                        ui_drag_kind = 2;
                        ui_drag_ex = grips[hit].ex;
                        ui_drag_ey = grips[hit].ey;
                        // 0 means "as big as it needs to be". The honest start
                        // for a drag is the size it HAS on screen right now,
                        // not zero - otherwise the first pixel of movement
                        // collapses the box.
                        float w0 = img ? sr.image_w : sr.text_w;
                        float h0 = img ? sr.image_h : sr.text_h;
                        ui_drag_w0 = w0 > 0.0f ? w0 : rw;
                        ui_drag_h0 = h0 > 0.0f ? h0 : rh;
                    } else {
                        ui_drag_kind = 1;
                    }
                    break;      // this rectangle took the press; the other one must not
                }
                if (hit >= 0) {
                    int ex = grips[hit].ex, ey = grips[hit].ey;
                    dai_ui_cursor_set(ui, ey == 0 ? DAI_CURSOR_SIZE_WE
                                        : ex == 0 ? DAI_CURSOR_SIZE_NS
                                        : (ex == ey ? DAI_CURSOR_SIZE_NWSE : DAI_CURSOR_SIZE_NESW));
                } else if (over_body && ui_drag_node == DAI_INVALID_NODE) {
                    dai_ui_cursor_set(ui, DAI_CURSOR_HAND);
                }
            }
        }
        if (ui_drag_node != DAI_INVALID_NODE) {
            float mx2 = 0, my2 = 0;
            int mdown = 0;
            dai_ui_mouse(ui, &mx2, &my2, &mdown, nullptr);
            dai_node_desc sr{};
            if (dai_doc_get(doc, ui_drag_node, &sr) == DAI_OK) {
                float dx2 = mx2 - ui_drag_x0, dy2 = my2 - ui_drag_y0;
                bool img = ui_drag_img != 0;
                if (ui_drag_kind == 1) {
                    if (img) { sr.image_x = ui_drag_ox + dx2; sr.image_y = ui_drag_oy + dy2; }
                    else     { sr.text_x  = ui_drag_ox + dx2; sr.text_y  = ui_drag_oy + dy2; }
                } else {
                    int anchor = img ? sr.image_anchor : sr.text_anchor;
                    if (anchor < 0) anchor = 0;
                    if (anchor > 8) anchor = 8;
                    float fx = (float)(anchor % 3) * 0.5f;
                    float fy = (float)(anchor / 3) * 0.5f;
                    float nw = ui_drag_w0, nh = ui_drag_h0;
                    float ox = ui_drag_ox, oy = ui_drag_oy;
                    if (ui_drag_ex > 0)      { nw += dx2; ox += fx * dx2; }
                    else if (ui_drag_ex < 0) { nw -= dx2; ox += (1.0f - fx) * dx2; }
                    if (ui_drag_ey > 0)      { nh += dy2; oy += fy * dy2; }
                    else if (ui_drag_ey < 0) { nh -= dy2; oy += (1.0f - fy) * dy2; }
                    if (nw < 8.0f) nw = 8.0f;
                    if (nh < 8.0f) nh = 8.0f;
                    if (img) { sr.image_w = nw; sr.image_h = nh; sr.image_x = ox; sr.image_y = oy; }
                    else     { sr.text_w  = nw; sr.text_h  = nh; sr.text_x  = ox; sr.text_y  = oy; }
                }
                // No transaction while the button is down: a drag is ONE undo
                // step, not one per frame. It is committed on release.
                dai_doc_set(doc, ui_drag_node, &sr);
            }
            if (!mdown) {
                ui_drag_node = DAI_INVALID_NODE;
                ui_drag_kind = 0;
            }
        }

        // ---- the game's own UI ------------------------------------------
        // Drawn into the Game view, clipped to it, in the same draw list as
        // everything else - a HUD in a second pass would sit on top of the
        // panels the Game view is docked next to.
        //
        // Shown while EDITING too, not only during play. A label you cannot
        // see until you press play is a label you place by trial and error.
        {
            dai_hud_frame();
            float vx2, vy2, vw2, vh2;
            dai_editor_ui_viewport_rect(panels, &vx2, &vy2, &vw2, &vh2);
            float hx, hy, hw, hh;
            int has_game = dai_editor_ui_game_view_rect(panels, &hx, &hy, &hw, &hh);

            // The Scene view draws the game's UI too. Unity's canvas lives IN
            // the scene, and a HUD you can only see in the Game tab is a HUD
            // you place by trial and error - which is what it was. Drawn
            // FIRST, so dai_hud_rect_of finds the scene copy and the move and
            // resize grips appear where the editing happens.
            if (dai_editor_ui_view(panels) == DAI_VIEW_SCENE && vw2 > 0.0f && vh2 > 0.0f) {
                dai_hud_editable(1);
                dai_hud_draw(ui, doc, vx2, vy2, vw2, vh2, 1.0f, hud_resolve, nullptr);
                dai_hud_editable(0);
            }

            if (has_game) {
                // Buttons answer the pointer HERE and only here: this is the
                // picture the player is looking at. The copy over the Scene
                // view is for editing, and a button that fired there would go
                // off every time you tried to drag it.
                dai_hud_interactive(dai_editor_state_get(ed) == DAI_EDITOR_PLAY);
                dai_hud_draw(ui, doc, hx, hy, hw, hh, 1.0f, hud_resolve, nullptr);
                dai_hud_interactive(0);
                gui_flush(ui, hx, hy, hw, hh);
            } else if (vw2 > 0.0f && vh2 > 0.0f) {
                // No Game panel: the script's UI still has to go somewhere, or
                // a menu drawn from code is invisible until you dock one.
                gui_flush(ui, vx2, vy2, vw2, vh2);
            }
        }
        diag_step("ui end");
        dai_ui_end(ui);
        diag_step("ui done");

        // The layout, once, a second in: enough frames for the dock to settle,
        // early enough to catch it going wrong.
        static int dump_at = 90;
        if (g_diag_on > 0 && dump_at > 0 && --dump_at == 0) {
            char lay[640];
            dai_editor_ui_layout_dump(panels, lay, sizeof(lay));
            std::printf("layout: %s\n", lay);
        }

        // The pointer says what is under it: an I-beam over a field, a resize
        // arrow on a window edge. The UI knows which widget that is; only the
        // window can set the shape.
        // New rigidbodies inherit the project's friction/bounce defaults.
        dai_editor_ui_physics_defaults(panels, psettings.default_friction, psettings.default_restitution);

        dai_window_cursor(win, dai_ui_cursor(ui));

        // The one strip the engine does not draw: the OS title bar. It follows
        // the theme's chrome colour, checked here so a theme switch repaints it
        // on the same frame instead of after a restart.
        {
            static uint32_t last_chrome = 0;
            uint32_t chrome = dai_ui_style_of(ui)->chrome;
            if (chrome != last_chrome) {
                last_chrome = chrome;
                dai_window_caption_color(win, chrome);
            }
        }

        // The show's clock. It reaches the preview and nothing else: what the
        // solver produced is a function of a time value, never of the frame
        // rate of the machine it is being watched on.
        if (g_show_ui) {
            dai_show_ui_advance(g_show_ui, dt);

            // A click on the export row only records the wish - the writing is
            // done here, where the project's folder is known. The outcome is
            // handed straight back so the status line can say it.
            {
                int fmt = dai_show_ui_take_export(g_show_ui);
                if (fmt) {
                    const dai_show_plan *plan = dai_show_get_plan(g_show);
                    dai_show_settings ss = dai_show_get_settings(g_show);
                    std::string base = std::string(dai_project_path(g_project)) + "/show";
                    const char *ext = (fmt == 1) ? ".skyc" : (fmt == 2) ? ".dsx"
                                    : (fmt == 3) ? ".csv" : ".json";
                    std::string out = base + ext;
                    char eerr[256] = { 0 };
                    dai_result rr = DAI_ERR_INVALID_ARG;
                    if (fmt == 1) rr = dai_show_export_skyc(plan, &ss, out.c_str(), eerr, sizeof(eerr));
                    if (fmt == 2) rr = dai_show_export_dsx (plan, &ss, out.c_str(), eerr, sizeof(eerr));
                    if (fmt == 3) rr = dai_show_export_csv (plan, &ss, out.c_str(), eerr, sizeof(eerr));
                    if (fmt == 4) rr = dai_show_export_json(plan, &ss, out.c_str(), eerr, sizeof(eerr));
                    char note[320];
                    std::snprintf(note, sizeof(note), "%s%s",
                                  rr == DAI_OK ? "wrote " : "export failed: ",
                                  rr == DAI_OK ? out.c_str() : (eerr[0] ? eerr : "unknown"));
                    dai_show_ui_note(g_show_ui, rr != DAI_OK, note);
                    if (g_panels_for_log) dai_editor_ui_log(g_panels_for_log, rr != DAI_OK, note);
                }
            }
            // The selected asset, offered to the storyboard as a figure. A
            // .glb is read into the same plain arrays the pipeline takes, so
            // nothing about the asset layer reaches dai_show.
            int sel = dai_editor_ui_asset_selected(panels);
            const char *rel = (sel >= 0 && (uint32_t)sel < g_asset_count)
                            ? g_asset_paths[sel] : nullptr;
            std::string want = rel ? rel : "";
            size_t dot = want.find_last_of('.');
            std::string ext = (dot == std::string::npos) ? "" : want.substr(dot);
            if (ext != ".glb" && ext != ".gltf") want.clear();
            if (want != g_show_mesh_src) {
                g_show_mesh_src = want;
                g_show_mesh_pos.clear();
                g_show_mesh_idx.clear();
                dai_show_ui_mesh(g_show_ui, nullptr, nullptr);
                if (!want.empty() && g_assets_dir[0]) {
                    std::string full = std::string(g_assets_dir) + "/" + want;
                    std::vector<uint8_t> bytes;
                    if (show_read_file(full.c_str(), bytes)) {
                        dai_mesh_data md[16];
                        char merr[256] = { 0 };
                        uint32_t mn = dai_gltf_read_geometry(bytes.data(), bytes.size(),
                                                             md, 16, merr, sizeof(merr));
                        if (mn > 16) mn = 16;
                        for (uint32_t m = 0; m < mn; ++m) {
                            uint32_t base = (uint32_t)(g_show_mesh_pos.size() / 3);
                            for (uint32_t v = 0; v < md[m].vertex_count; ++v) {
                                g_show_mesh_pos.push_back(md[m].vertices[v].position.x);
                                g_show_mesh_pos.push_back(md[m].vertices[v].position.y);
                                g_show_mesh_pos.push_back(md[m].vertices[v].position.z);
                            }
                            for (uint32_t i2 = 0; i2 < md[m].index_count; ++i2)
                                g_show_mesh_idx.push_back(base + md[m].indices[i2]);
                        }
                        dai_gltf_free_geometry(md, mn);
                    }
                }
                if (!g_show_mesh_idx.empty()) {
                    dai_show_sample_desc d;
                    std::memset(&d, 0, sizeof(d));
                    d.positions    = g_show_mesh_pos.data();
                    d.vertex_count = (uint32_t)(g_show_mesh_pos.size() / 3);
                    d.indices      = g_show_mesh_idx.data();
                    d.index_count  = (uint32_t)g_show_mesh_idx.size();
                    d.base_rgba    = 0xFFFFFFFFu;
                    d.view_dir     = dai_vec3{ 0.0f, 0.0f, 1.0f };
                    dai_show_ui_mesh(g_show_ui, &d, g_show_mesh_src.c_str());
                }
            }
        }

        float alpha = 1.0f;
        dai_editor_advance(ed, dt, &alpha);

#ifdef DAI_WITH_SCRIPT
        // Behaviours: init() on the edge into play, frame() every rendered
        // frame, and Stop tears them down together with the world.
        {
            int st_now = dai_editor_state_get(ed);
            if (st_now == DAI_EDITOR_PLAY && !g_scripts_live) {
                // A run starts with an empty console. Otherwise the first
                // error of THIS run is somewhere below the errors of the last
                // three, and the only way to tell them apart is the clock.
                if (g_panels_for_log) dai_editor_ui_log_clear(g_panels_for_log);
                scripts_start();
            }
            if (st_now != DAI_EDITOR_PLAY && g_scripts_live) scripts_stop();
            g_scripts_live = st_now == DAI_EDITOR_PLAY;
            if (g_scripts_live && g_native) {
                g_native_time += 1.0f / 60.0f;
                for (RunningNative &rn : g_natives)
                    dai_native_frame(g_native, rn.id, &g_native_api,
                                     (dai_nentity)(uint32_t)rn.node, 1.0f / 60.0f);
            }
            if (g_scripts_live)
                for (RunningScript &rs : g_running) {
                    char serr[192] = { 0 };
                    // state.dt, so a script can be frame rate independent
                    // without asking the host for a clock it does not have.
                    dai_script_set_number(rs.s, "dt", 1.0 / 60.0);
                    if (dai_script_call(rs.s, "frame", serr, sizeof(serr)) != DAI_OK && serr[0]) {
                        char line[400];
                        std::snprintf(line, sizeof(line), "%s: %s", rs.path.c_str(), serr);
                        dai_editor_ui_log(panels, 2, line);   // collapses on repeat
                    }
                }

            // ---- what the UI buttons did this frame --------------------
            // After frame(), so a click and the frame it happened in are in
            // the order they read: the world moved, THEN the button fired.
            // The call goes to the script on the clicked node only - a button
            // press is a message to an object, not an announcement.
            if (g_scripts_live) {
                dai_node clicked[16];
                uint32_t nc = dai_hud_take_clicks(clicked, 16);
                for (uint32_t ci = 0; ci < nc; ++ci) {
                    dai_node_desc bd{};
                    const char *fn = "onClick";
                    if (dai_doc_get(doc, clicked[ci], &bd) == DAI_OK && bd.button_action[0])
                        fn = bd.button_action;
                    bool any = false;
                    for (RunningScript &rs : g_running) {
                        if (rs.node != clicked[ci]) continue;
                        any = true;
                        char berr[256] = { 0 };
                        dai_result br = dai_script_call(rs.s, fn, berr, sizeof(berr));
                        if (br == DAI_ERR_NOT_FOUND) continue;   // no handler: fine
                        if (br != DAI_OK && berr[0]) {
                            char line[400];
                            std::snprintf(line, sizeof(line), "%s: %s", rs.path.c_str(), berr);
                            dai_editor_ui_log(panels, 2, line);
                        }
                    }
                    if (!any) {
                        // Worth saying out loud: a button that looks alive and
                        // does nothing is the hardest kind of nothing to debug.
                        char line[256];
                        std::snprintf(line, sizeof(line),
                                      "button '%s' clicked - no script on it defines %s()",
                                      bd.name[0] ? bd.name : "(unnamed)", fn);
                        dai_editor_ui_log(panels, 1, line);
                    }
                }
            }
        }
#endif

        // Nobody has asked for the second view yet this frame. Without this
        // the last rectangle any of them set keeps rendering, so closing the
        // Game panel or deselecting the camera leaves a picture behind.
        dai_render_world_clip2(r, 0.0f, 0.0f, 0.0f, 0.0f);

        // The camera preview, in the corner of the scene view. It uses the
        // renderer's SECOND view, the same one the Game panel uses - and the
        // editor refuses to offer a preview while that panel is open, so the
        // two can never want it at the same time.
        {
            float cx3, cy3, cw3, ch3, cfov = 60.0f, cortho = 0.0f;
            dai_vec3 ceye{}, clook{};
            if (dai_editor_ui_camera_preview(panels, &cx3, &cy3, &cw3, &ch3,
                                             &ceye, &clook, &cfov, &cortho)) {
                dai_render_camera2(r, ceye, clook, dai_vec3{ 0, 1, 0 }, cfov);
                dai_render_ortho2(r, cortho);
                dai_render_world_clip2(r, cx3 * uis, cy3 * uis, cw3 * uis, ch3 * uis);
            }
        }

        // The camera, asked for directly. This used to shoot a ray through the
        // middle of the viewport and call the result "forward" - which works
        // until the viewport is 0x0, and it IS 0x0 whenever the Scene tab is
        // not the visible one. Then the middle is the corner, the corner ray
        // became the camera's direction, and switching to the Script tab and
        // back turned the view by half a field of view.
        dai_vec3 eye{}, look{};
        dai_editor_camera_get(ed, &eye, &look, nullptr);
        // The EDITOR camera is always updated from the editor's own state, in
        // both views: picking, the gizmo and the scene view all project
        // through it, and a game view that overwrote it would leave the scene
        // view pointing wherever the player camera happened to be.
        // The viewport is the scene window's body rect: the camera projects
        // into exactly that rectangle, the renderer clips the world to it, and
        // picking reads clicks in the same pixels. Three sides of one truth.
        float vrx = 0, vry = 0, vrw = lw, vrh = lh;
        dai_editor_ui_viewport_rect(panels, &vrx, &vry, &vrw, &vrh);
        dai_editor_camera_viewport_rect(ed, vrx, vry, vrw, vrh);
        dai_render_world_clip(r, vrx * uis, vry * uis, vrw * uis, vrh * uis);
        {   // The floor grid as world lines: depth tested, so boxes hide it.
            // Bigger buffer than the old fixed 20 m mat needed: the grid now
            // follows the camera and carries a coarse set as well.
            static float grid_xyz[420 * 2 * 3];
            uint32_t gn = dai_editor_ui_grid_lines(panels, grid_xyz, 420 * 2);
            dai_render_lines(r, grid_xyz, gn, 0.35f, 0.38f, 0.42f, 0.75f);
        }
        // ...and it is only written back when there IS a viewport. A frame in
        // which the scene is not on screen has nothing to say about the
        // camera, and saying it anyway is what broke this.
        if (vrw > 0.0f && vrh > 0.0f)
            dai_editor_camera(ed, eye, look, dai_vec3{ 0, 1, 0 }, 55.0f, 0.1f, 300.0f,
                              vrw, vrh);

        // What gets RENDERED is the game camera while the Game tab is up.
        dai_vec3 reye = eye, rlook = look;
        float rfov = 55.0f;
        // The Game tab shows what the GAME camera sees - and with no camera in
        // the scene it must show NOTHING. It used to fall through to the
        // editor camera, so selecting the Game tab still drew the scene view
        // and the "no camera" message sat on top of a picture it was denying.
        int no_world = 0;
        // The Game tab with no camera shows NEUTRAL dark, not the scene's
        // sky-ish clear - an empty Game view must not look like a scene
        // someone forgot to fill.
        if (dai_editor_ui_view(panels) == DAI_VIEW_GAME) {
            dai_vec3 ge{}, gl{};
            float gf = 60.0f;
            if (dai_editor_ui_game_camera(panels, &ge, &gl, &gf)) {
                reye = ge; rlook = gl; rfov = gf;
            } else {
                no_world = 1;
                dai_render_clear_color(r, 0.08f, 0.08f, 0.09f);
            }
        }
        dai_render_sky(r, no_world ? 0 : 1);
        // Orthographic when the GAME camera says so, or when the scene view is
        // in 2D. One call, at the end, because there is one projection for
        // view 0 and whoever writes it last wins - the 2D toggle used to set
        // it thirty lines above this and then have it overwritten with 0 here,
        // which is why pressing 2 turned the camera without flattening it.
        float game_ortho = dai_editor_ui_game_ortho(panels);
        float scene_ortho = dai_editor_cam_2d_get(ed) ? dai_editor_cam_ortho_height(ed) : 0.0f;
        dai_render_ortho(r, dai_editor_ui_view(panels) == DAI_VIEW_GAME ? game_ortho : scene_ortho);
        dai_render_camera(r, reye, rlook, dai_vec3{ 0, 1, 0 }, rfov, 0.1f, 300.0f);

        // Both panels docked open: the Scene panel keeps the editor camera
        // (above) and the Game panel shows the game camera as the SECOND view.
        if (dai_editor_ui_view(panels) == DAI_VIEW_SCENE) {
            float gx, gy, gw, gh;
            if (dai_editor_ui_game_view_rect(panels, &gx, &gy, &gw, &gh)) {
                dai_vec3 ge{}, gl{};
                float gf = 60.0f;
                if (dai_editor_ui_game_camera(panels, &ge, &gl, &gf)) {
                    dai_render_camera2(r, ge, gl, dai_vec3{ 0, 1, 0 }, gf);
                    dai_render_ortho2(r, game_ortho);
                    (void)0;
                    dai_render_world_clip2(r, gx * uis, gy * uis, gw * uis, gh * uis);
                }
            }
        }

        // Light components, straight from the document: the scene layer does
        // not carry them, so the host collects them each frame. Cheap - there
        // are never many lights, and it keeps lights editable while playing.
        {
            static std::vector<dai_light> lights;
            lights.clear();
            uint32_t ln = dai_doc_count(doc);
            static std::vector<dai_node> lids;
            lids.resize(ln);
            if (ln) dai_doc_nodes(doc, lids.data(), ln);
            for (dai_node id : lids) {
                dai_node_desc lr{};
                if (dai_doc_get(doc, id, &lr) != DAI_OK || !lr.light) continue;
                dai_vec3 wp{}, ws{ 1, 1, 1 };
                dai_quat wr2{ 0, 0, 0, 1 };
                if (!dai_editor_live_transform(ed, id, &wp, &wr2, &ws))
                    dai_doc_world_transform(doc, id, &wp, &wr2, &ws);
                dai_vec3 col = (lr.light_color.x || lr.light_color.y || lr.light_color.z)
                             ? lr.light_color : dai_vec3{ 1, 1, 1 };
                float power = lr.light_intensity > 0.0f ? lr.light_intensity : 1.0f;
                float range = lr.light_range > 0.0f ? lr.light_range : 10.0f;
                if (lr.light == 3) {
                    // A directional light is the sun, not a point in the list.
                    dai_vec3 fwd{ 0, 0, -1 };
                    float x = wr2.x, y = wr2.y, z = wr2.z, w2 = wr2.w;
                    dai_vec3 d2{ 2*(x*z + w2*y), 2*(y*z - w2*x), 1 - 2*(x*x + y*y) };
                    (void)fwd;
                    dai_render_sun(r, dai_vec3{ -d2.x, -d2.y, -d2.z }, col, power);
                    continue;
                }
                dai_light L{};
                L.position = wp; L.color = col; L.intensity = power; L.range = range;
                if (lr.light == 2) {
                    float x = wr2.x, y = wr2.y, z = wr2.z, w2 = wr2.w;
                    L.direction = { 2*(x*z + w2*y), 2*(y*z - w2*x), 1 - 2*(x*x + y*y) };
                    L.direction = { -L.direction.x, -L.direction.y, -L.direction.z };
                    L.type = DAI_LIGHT_SPOT;
                    float cone = lr.light_cone > 0.0f ? lr.light_cone : 30.0f;
                    L.inner_deg = cone * 0.7f;
                    L.outer_deg = cone;
                } else {
                    L.type = DAI_LIGHT_POINT;
                }
                lights.push_back(L);
            }
            dai_render_lights(r, lights.empty() ? nullptr : lights.data(), (uint32_t)lights.size());
        }

        diag_step("scene instances");
        uint32_t n = dai_scene_instances(sc, inst.data(), (uint32_t)inst.size(), alpha);
        if (no_world) n = 0;   // Game tab, no camera: an empty frame, not a lie

        diag_step("ui draw list");
        const dai_ui_draw *draws = nullptr;
        uint32_t nb = dai_ui_draws(ui, &draws);
        std::vector<dai_ui_vertex> verts;
        std::vector<uint32_t> counts;
        std::vector<dai_texture> texes;
        if (g_diag_frames > 0) std::printf("[step] batches %u\n", (unsigned)nb);
        for (uint32_t i = 0; i < nb; ++i) {
            // A batch count of millions is corruption, not a busy interface.
            if (draws[i].count > 400000u || !draws[i].vertices) {
                std::printf("!! DIAG: batch %u has count %u vertices=%p - refusing it\n",
                            (unsigned)i, (unsigned)draws[i].count, (const void *)draws[i].vertices);
                std::fflush(stdout);
                diag_stack("bad ui batch");
                continue;
            }
            verts.insert(verts.end(), draws[i].vertices, draws[i].vertices + draws[i].count);
            counts.push_back(draws[i].count);
            texes.push_back(draws[i].texture);
        }
        diag_step("render ui");
        dai_render_ui(r, verts.data(), (uint32_t)verts.size(), counts.data(), texes.data(), (uint32_t)counts.size());
        diag_step("render frame");
        dai_render_frame(r, inst.data(), n);
        diag_step("present");
        dai_window_present(win);
        if (g_diag_frames > 0) { --g_diag_frames; std::printf("[step] frame done\n"); }
    }

    layout_save_for_project();
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
    }
    {   // what this human set on this machine, kept for the next start
        prefs.cam_speed = dai_editor_cam_speed_get(ed);
        save_prefs_now();
    }
    if (g_project) dai_project_close(g_project);
    dai_editor_ui_destroy(panels);
    dai_editor_destroy(ed);
    dai_ui_destroy(ui);
    if (icons) dai_icons_free(icons);
    if (g_font) dai_font_free(g_font);
    if (assets) dai_assets_destroy(assets);
    dai_window_close(win);
    dai_render_destroy(r);
    dai_doc_sync_destroy(sync);
    dai_doc_destroy(doc);
    dai_scene_destroy(sc);
    dai_destroy(w);
    if (g_update.state == 3) {
        // Last thing: the batch file waits for this process to end, swaps the
        // exe, writes the sidecar and starts the new build.
        char uerr[256] = { 0 };
        if (dai_self_update_restart(&g_update.info, g_update.exe_path,
                                    uerr, sizeof(uerr)) != DAI_OK) {
            char line[320];
            std::snprintf(line, sizeof(line), "update: restart failed (%s)", uerr);
            update_log(line);
            std::printf("%s\n", line);
        } else {
            update_log("update: handed over to the swap script");
        }
    }
    return 0;
}
