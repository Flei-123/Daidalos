// runtime_main.cpp - the shipped game. The same engine, without the editor.
//
// This is the OTHER half of the export. tools/build_runtime.sh compiles this
// file into a runtime template; dai_project_export copies that template and
// appends the project archive to the copy. What starts on the other person's
// machine is this program, reading its own tail.
//
//   MyGame.exe                       the export: archive is inside the file
//   daidalos_runtime <project-dir>   development: read the project off disk
//   daidalos_runtime <game.dpk>      a standalone archive next to the runtime
//   daidalos_runtime --headless 120  no window: mount, load, simulate, report
//   ... --shot frame.ppm             render one frame off screen and write it
//
// What is NOT here, on purpose: docking, panels, gizmos, the hierarchy, undo,
// the asset browser, hot reload, the self updater, the project picker. A game
// is a window, a scene, physics and scripts. Everything the editor adds costs
// megabytes and start-up time that a player never gets anything back for -
// which is exactly why Godot ships a separate template binary instead of a
// stripped editor.
//
// Two differences from the editor that matter:
//
//   SCRIPTS ALWAYS RUN. There is no play button to press. init() is called
//   once when the scene is up, frame() on every rendered frame, until the
//   window closes.
//
//   EVERY READ GOES THROUGH THE VFS. Not one fopen on a project path: in the
//   export there is no project directory to open. The asset resolver below
//   loads .glb bytes out of the archive and hands them to dai_gltf_load_memory
//   rather than going through the editor's asset layer, which mounts folders.

#include "dai_render.h"
#include "dai_scene.h"
#include "dai_doc.h"
// The game's UI. dai_hud_draw is the SAME function the editor's Game view
// calls - what you place is what ships, or the Game view is a rehearsal for a
// different play. dai_ui is only here to own the font atlas and build the
// vertex batches; no editor panel comes with it.
#include "dai_editor_ui.h"
#include "dai_ui.h"
#include "dai_font.h"
#include "dai_strings.h"
#include "dai_gltf.h"
#include "dai_vfs.h"
#ifdef DAI_WITH_SCRIPT
#include "dai_script.h"
#include "dai_audio.h"
#include "dai_material.h"
// The JS object model the editor installs before every behaviour. Without it
// `self` does not exist in an exported game and every behaviour that ever ran
// in the editor dies on its first line.
#include "dai_prelude.h"
// The same seam the editor uses to turn a .daimat into a real dai_material.
// Without it a blockout scene reaches the runtime grey: the document stores a
// PATH, and nothing on this side used to read it.
#include "dai_ext.h"
#include "dai_material_host.inl"
// Module 1: the blockout meshes themselves. Without it every box, cylinder
// and CSG cut in the document reaches the runtime as the default cube - the
// document stores the SHAPE, and building it is the host's job in the editor
// and here alike.
#include "dai_blockout_host.inl"
#endif

#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>
#include <map>

#ifdef _WIN32
#include <windows.h>
#endif

// ---------------------------------------------------------------- logging
//
// A game built with -mwindows has no console: printf goes to a handle that is
// not there, and the first thing anyone asks when it does not start is "what
// did it say". So it says it into a file next to the executable, always, on
// every platform - the same log whether it was run from a terminal or double
// clicked.

static FILE *g_log = nullptr;
static int   g_have_console = 1;

static void log_open(const char *exe_path) {
    std::string p = exe_path && *exe_path ? exe_path : "daidalos_runtime";
    size_t dot = p.find_last_of('.');
    size_t sl = p.find_last_of("/\\");
    if (dot != std::string::npos && (sl == std::string::npos || dot > sl)) p.resize(dot);
    p += ".log";
    g_log = std::fopen(p.c_str(), "wb");
#ifdef _WIN32
    // No console when linked with -mwindows. Nothing to mirror to.
    g_have_console = GetConsoleWindow() != nullptr;
#endif
}

static void rt_log(const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (g_have_console) { std::fputs(buf, stdout); std::fputc('\n', stdout); std::fflush(stdout); }
    if (g_log) { std::fputs(buf, g_log); std::fputc('\n', g_log); std::fflush(g_log); }
}

// ------------------------------------------------------------- scene loading

static std::string dir_of(const std::string &p) {
    size_t sl = p.find_last_of('/');
    return sl == std::string::npos ? std::string() : p.substr(0, sl);
}

static std::string join_rel(const std::string &base, const std::string &rel) {
    if (rel.empty() || base.empty()) return rel;
    if (rel[0] == '/') return rel.substr(1);
    return base + "/" + rel;
}

// Copies every node of `src` under `parent` in `dst`. The same rule the
// editor's loader uses: ids belong to the document they live in, and the
// copies must NOT keep the prefab reference or a reload would expand them
// again, forever.
static uint32_t graft(dai_doc *dst, const dai_doc *src, dai_node parent) {
    std::vector<dai_node> ids((size_t)dai_doc_count(src));
    if (ids.empty()) return 0;
    dai_doc_nodes(src, ids.data(), (uint32_t)ids.size());
    std::unordered_map<dai_node, dai_node> map;
    uint32_t made = 0;
    for (size_t i = 0; i < ids.size(); ++i) {         // parents come first
        dai_node_desc rec{};
        if (dai_doc_get(src, ids[i], &rec) != DAI_OK) continue;
        rec.prefab[0] = 0;
        dai_node p = parent;
        if (rec.parent) {
            auto it = map.find(rec.parent);
            if (it != map.end()) p = it->second;
        }
        rec.parent = p;
        dai_node made_id = dai_doc_add(dst, &rec);
        if (!made_id) continue;
        map[ids[i]] = made_id;
        ++made;
    }
    return made;
}

// Loads a scene THROUGH THE VFS, expanding prefab instances the way
// dai_doc_load does on disk. dai_doc_from_text deliberately does not expand
// them - only a load knows what directory the references are relative to - so
// the runtime has to do that part itself against the archive.
static bool load_scene_vfs(dai_doc *doc, const std::string &vpath, int depth,
                           std::vector<std::string> &open_files, char *err, size_t err_len) {
    if (depth > 8) {
        std::snprintf(err, err_len, "prefabs nested more than 8 deep");
        return false;
    }
    for (size_t i = 0; i < open_files.size(); ++i)
        if (open_files[i] == vpath) {
            std::snprintf(err, err_len, "'%s' contains itself", vpath.c_str());
            return false;
        }

    size_t n = 0;
    void *bytes = dai_vfs_read(vpath.c_str(), &n);
    if (!bytes) {
        std::snprintf(err, err_len, "%s", dai_vfs_last_error());
        return false;
    }
    char perr[256] = { 0 };
    dai_result r = dai_doc_from_text(doc, (const char *)bytes, n, perr, sizeof(perr));
    dai_vfs_free(bytes);
    if (r != DAI_OK) {
        std::snprintf(err, err_len, "%s: %s", vpath.c_str(), perr);
        return false;
    }

    std::string base = dir_of(vpath);
    std::vector<dai_node> ids((size_t)dai_doc_count(doc));
    if (!ids.empty()) dai_doc_nodes(doc, ids.data(), (uint32_t)ids.size());
    open_files.push_back(vpath);
    for (size_t i = 0; i < ids.size(); ++i) {
        dai_node_desc rec{};
        if (dai_doc_get(doc, ids[i], &rec) != DAI_OK || !rec.prefab[0]) continue;
        std::string full = join_rel(base, rec.prefab);
        dai_doc *sub = dai_doc_create();
        if (!sub) continue;
        char serr[256] = { 0 };
        // A missing prefab is not fatal: the instance node stays, obviously
        // empty, rather than taking the whole game down at the player's.
        if (load_scene_vfs(sub, full, depth + 1, open_files, serr, sizeof(serr)))
            graft(doc, sub, ids[i]);
        else
            rt_log("prefab '%s': %s", full.c_str(), serr);
        dai_doc_destroy(sub);
    }
    open_files.pop_back();
    return true;
}

// ------------------------------------------------------- assets, from the VFS
//
// The editor's asset layer (Mnemosyne) mounts folders and watches mtimes. A
// shipped game has neither: its files are inside its own executable and none
// of them will ever change. So the runtime keeps its own two-line cache and
// hands the bytes to dai_gltf_load_memory, which is exactly the entry point
// that exists for "I already have the bytes, there is no path left".

struct AssetCache {
    dai_renderer *r = nullptr;
    std::string   base;                     // where relative URIs resolve from
    std::unordered_map<std::string, dai_model *> models;
    std::vector<std::string> failed;
};
static AssetCache g_assets;

// glTF buffers and images that live beside the .gltf. The bytes have to stay
// alive until the import returns, which is what the little holder is for.
static std::vector<void *> g_sidecar_hold;
static int sidecar_read(const char *uri, const void **out_bytes, size_t *out_size, void *user) {
    const std::string *base = (const std::string *)user;
    std::string p = join_rel(*base, uri ? uri : "");
    size_t n = 0;
    void *b = dai_vfs_read(p.c_str(), &n);
    if (!b) return 0;
    g_sidecar_hold.push_back(b);
    *out_bytes = b;
    *out_size = n;
    return 1;
}

static std::string strip_selector(const std::string &path, std::string *node_name) {
    size_t h = path.find('#');
    if (h == std::string::npos) { if (node_name) node_name->clear(); return path; }
    if (node_name) *node_name = path.substr(h + 1);
    return path.substr(0, h);
}

static dai_model *asset_model(const std::string &file) {
    auto it = g_assets.models.find(file);
    if (it != g_assets.models.end()) return it->second;
    if (!g_assets.r) return nullptr;
    for (size_t i = 0; i < g_assets.failed.size(); ++i)
        if (g_assets.failed[i] == file) return nullptr;

    size_t n = 0;
    void *bytes = dai_vfs_read(file.c_str(), &n);
    if (!bytes) {
        rt_log("asset '%s': %s", file.c_str(), dai_vfs_last_error());
        g_assets.failed.push_back(file);
        return nullptr;
    }
    char err[256] = { 0 };
    std::string base = dir_of(file);
    g_sidecar_hold.clear();
    dai_model *m = dai_gltf_load_memory(g_assets.r, bytes, n, sidecar_read, &base, err, sizeof(err));
    for (size_t i = 0; i < g_sidecar_hold.size(); ++i) dai_vfs_free(g_sidecar_hold[i]);
    g_sidecar_hold.clear();
    dai_vfs_free(bytes);
    if (!m) {
        rt_log("asset '%s': %s", file.c_str(), err);
        g_assets.failed.push_back(file);
        return nullptr;
    }
    rt_log("asset '%s': %u pieces", file.c_str(), dai_model_node_count(m));
    g_assets.models[file] = m;
    return m;
}

// Matches dai_asset_resolve_fn. Same contract as dai_assets_resolve: fill up
// to `max`, return how many the asset HAS, 0 means "fall back to the shape".
static uint32_t runtime_resolve(const char *path, dai_render_part *out, uint32_t max, void *) {
    if (!path || !*path) return 0;
    std::string sel;
    std::string file = strip_selector(path, &sel);
    dai_model *m = asset_model(file);
    if (!m) return 0;

    auto write = [&](const dai_model_node *nd, uint32_t at) {
        if (!out || at >= max) return;
        out[at].mesh = nd->mesh;
        out[at].material = nd->material;
        out[at].position = nd->position;
        out[at].rotation = nd->rotation;
        out[at].scale = nd->scale;
    };
    if (!sel.empty()) {
        const dai_model_node *nd = dai_model_find(m, sel.c_str());
        if (!nd) return 0;                 // a typo must not draw the wrong object
        write(nd, 0);
        return 1;
    }
    uint32_t count = dai_model_node_count(m);
    for (uint32_t i = 0; i < count; ++i) {
        const dai_model_node *nd = dai_model_node_at(m, i);
        if (nd) write(nd, i);
    }
    return count;
}

// --------------------------------------------------------------- the world

static dai_world     *g_world = nullptr;
static dai_scene     *g_scene = nullptr;
// ---- the HUD ------------------------------------------------------------
static dai_ui      *g_ui = nullptr;
static dai_font    *g_hud_font = nullptr;
static dai_strings *g_hud_strings = nullptr;

// The exported game reads its string table out of the pack, like everything
// else: dai_vfs, not the file system. A game that looks for Assets/Strings on
// the player's disk is a game that has no strings on the player's disk.
static void hud_strings_load(const char *lang) {
    if (!g_hud_strings) g_hud_strings = dai_strings_create();
    if (!lang || !lang[0]) return;
    char vpath[128];
    // Under assets/, where the exporter puts everything from the project's
    // asset folder - the same prefix the script loader uses.
    std::snprintf(vpath, sizeof(vpath), "assets/Strings/%s.daistr", lang);
    size_t n = 0;
    const void *bytes = dai_vfs_read(vpath, &n);
    if (!bytes || !n) { rt_log("no string table %s", vpath); return; }
    char err[256] = { 0 };
    if (dai_strings_parse(g_hud_strings, (const char *)bytes, n, err, sizeof(err)) != DAI_OK)
        rt_log("string table %s: %s", vpath, err);
    else
        rt_log("language %s: %u entries", lang, (unsigned)dai_strings_count(g_hud_strings));
}

static const char *hud_resolve(const char *text, void *) {
    static char buf[256];
    return dai_strings_resolve(g_hud_strings, text, buf, sizeof(buf));
}

static dai_doc       *g_doc = nullptr;
static dai_doc_sync  *g_sync = nullptr;

// ---- sound --------------------------------------------------------------
//
// The bank is named in boot.cfg ("audio_bank assets/audio/game.json"). It is
// opened from the FILE SYSTEM, not the VFS: Aulos reads its bank and its
// samples itself, which is fine for a project folder and is the one thing a
// .dpk cannot do yet - the log says so rather than failing silently.
static std::string        g_project_dir;      /* "" when we run from an archive */
static dai_audio_backend *g_audio = nullptr;
static std::string        g_audio_root;

static double rt_audio_play(const char *event, const double *pos, double volume,
                            double pitch, void *) {
    if (!g_audio || !event || !*event) return 0;
    dai_audio_event ev{};
    std::snprintf(ev.name, sizeof(ev.name), "%s", event);
    ev.volume = (float)(volume > 0 ? volume : 1.0);
    ev.pitch  = (float)(pitch  > 0 ? pitch  : 1.0);
    if (pos) {
        ev.is_3d = 1;
        ev.position = dai_vec3{ (float)pos[0], (float)pos[1], (float)pos[2] };
    }
    uint32_t inst = dai_audio_play_ex(g_audio, &ev, DAI_AUDIO_BUS_EVENT);
    // THE LOG HAS TO SAY WHETHER SOUND IS ACTUALLY COMING OUT. Until this was
    // here, "audio: bank ... out of the archive" was the last word on the
    // subject - and that line only means the BANK parsed. A game that loaded
    // its bank, bound audio to the script and then played into a closed device
    // or a missing event looked identical in the log to one you can hear. So:
    // report the first event that plays and the first one that does not, with
    // its name. Two lines, once each, and silence stops being a mystery.
    static int said_ok = 0, said_bad = 0;
    if (inst && !said_ok) {
        said_ok = 1;
        rt_log("audio: first event '%s' started (voice %u) - the mixer is running",
               event, inst);
    } else if (!inst && !said_bad) {
        said_bad = 1;
        rt_log("audio: event '%s' did NOT start - no voice. Wrong event name, "
               "no free voice, or the device never opened. THE GAME IS SILENT.",
               event);
    }
    return (double)inst;
}
static void rt_audio_stop(double handle, void *) {
    // Stopping is "move it nowhere and let it finish" until Aulos grows a
    // stop-by-handle; a one shot is over in under a second anyway.
    (void)handle;
}
static void rt_audio_listener(const double *pos, const double *fwd, void *) {
    if (!g_audio || !pos || !fwd) return;
    dai_audio_listener(g_audio,
                       dai_vec3{ (float)pos[0], (float)pos[1], (float)pos[2] },
                       dai_vec3{ (float)fwd[0], (float)fwd[1], (float)fwd[2] },
                       dai_vec3{ 0, 1, 0 }, dai_vec3{ 0, 0, 0 });
}
/* Aulos asks for a path, dai_vfs hands over the bytes. The archive keeps the
 * entry compressed, so this is a real read and a real allocation - which is
 * why the pair has a release. */
static int rt_audio_vfs_read(const char *path, void **out, size_t *len, void *) {
    if (!path || !out || !len) return 0;
    size_t n = 0;
    void *bytes = dai_vfs_read(path, &n);
    if (!bytes) return 0;
    *out = bytes; *len = n;
    return 1;
}
static void rt_audio_vfs_release(void *bytes, void *) { if (bytes) dai_vfs_free(bytes); }

static dai_script_audio_host g_audio_host = { rt_audio_play, rt_audio_stop,
                                              rt_audio_listener, nullptr };

// .daimat -> dai_material, once per frame after the sync, exactly like the
// editor does it. Needs real paths, so it only runs from a project folder.
static void rt_apply_materials(dai_renderer *r, bool entities_changed) {
    if (!r) return;
    // Two ways to find a .daimat and its maps. A dev run has a folder; the
    // shipped game has the archive appended to its own executable, and until
    // this had the second case an exported build drew every blockout in the
    // renderer's placeholder colour - the screenshot everybody read as
    // "missing textures".
    static std::string assets = g_project_dir.empty() ? std::string("assets")
                                                      : g_project_dir + "/assets";
    dai_ext_host h{};
    h.doc = g_doc;
    h.sync = g_sync;
    h.scene = g_scene;
    h.renderer = r;
    h.assets_dir = assets.c_str();
    if (g_project_dir.empty()) {
        h.read = rt_audio_vfs_read;          /* same archive, same bytes */
        h.release = rt_audio_vfs_release;
    }
    // Same order as the editor: the material first, then the mesh it is drawn
    // on (see examples/editor_demo.cpp).
    // A SHIPPED GAME IS NOT AN EDITOR. Nobody is repainting a material while
    // the game runs, so resolving all of them from scratch every frame is pure
    // waste - and it was expensive waste: dai_material_host_apply walked all
    // 569 nodes, copied a dai_node_desc out of the document and built a
    // std::string per node EVERY FRAME, which measured 6.8 ms. Together with
    // the rest of the per-frame work that put the CPU over the 16.7 ms a 60 Hz
    // frame has before a single triangle is drawn - the stutter that got
    // reported as "laggt sehr im Vergleich zum Browser".
    //
    // So: do the expensive resolve rarely, remember what came out of it, and
    // spend the other frames on the only part that actually has to repeat.
    // That part is real - the sync layer throws an entity away and respawns it
    // when a node's mesh or scale changes, and a respawned entity wears the
    // default material - but re-applying a REMEMBERED handle is two cheap
    // lookups, not a filesystem walk.
    static std::vector<std::pair<dai_node, dai_material>> mat_cache;
    static uint64_t mat_frame = 0;
    static uint32_t mat_nodes = 0;
    static bool blockout_dirty = true;

    // dai_doc_count() WALKS THE WHOLE NODE MAP - it counts the living nodes
    // one by one. Calling it every frame to ask "did the scene change size?"
    // cost more than the work it was guarding (3.4 ms of the 9 ms this
    // function used to take). The answer changes when a node is spawned or
    // destroyed, so asking twice a second is plenty - and 29 out of 30 frames
    // pay nothing at all.
    static uint32_t cached_count = 0;
    static uint64_t count_frame = 0;
    if ((count_frame++ % 30) == 0 || cached_count == 0) cached_count = dai_doc_count(g_doc);
    uint32_t now_nodes = cached_count;
    // When to pay for a full rebuild. Not on a plain timer: a periodic
    // rebuild costs a ~9 ms spike, and a hitch every single second is worse
    // to play than a slightly higher steady cost - that hitch IS the lag.
    // And not on "the count changed" alone either: a running game spawns and
    // retires wings and horses, so that condition is true forever rather than
    // once. So both, with a floor of two seconds between rebuilds. A node
    // that appears without a material waits a few frames for it, which is
    // invisible; a shipped game has no editor repainting materials underneath
    // it, so nothing else can make this stale.
    if (mat_cache.empty() || (now_nodes != mat_nodes && (mat_frame % 120) == 0)) {
        // dai_material_host_apply() is NOT called here any more. It resolves
        // every node's material and assigns it - which is exactly what the
        // loop below already does while it fills the cache. Calling both meant
        // doing the whole expensive pass TWICE per rebuild.
        mat_nodes = now_nodes;
        blockout_dirty = true;
        mat_cache.clear();
        std::vector<dai_node> ids(now_nodes);
        if (now_nodes) dai_doc_nodes(g_doc, ids.data(), now_nodes);
        // One resolve per material PATH. dai_material_host_for() stat()s the
        // .daimat and all three of its maps, so asking it once per NODE means
        // ~2000 syscalls for the dozen materials a scene actually has.
        std::map<std::string, dai_material> seen;
        for (dai_node n : ids) {
            dai_node_desc d{};
            if (dai_doc_get(g_doc, n, &d) != DAI_OK || !d.materials[0]) continue;
            std::string first = d.materials;
            size_t semi = first.find(';');
            if (semi != std::string::npos) first = first.substr(0, semi);
            if (!dai_matfile_is_file(first.c_str())) continue;
            dai_material m;
            std::map<std::string, dai_material>::iterator sit = seen.find(first);
            if (sit != seen.end()) m = sit->second;
            else { m = dai_material_host_for(&h, first); seen[first] = m; }
            if (!m) continue;
            mat_cache.push_back(std::make_pair(n, m));
            dai_entity e = dai_doc_sync_entity(g_sync, n);
            if (e) dai_scene_set_material(g_scene, e, m);
        }
        // Said once, on the first build: the number that tells a missing
        // texture apart from a missing material. 0 painted here is what the
        // "everything is placeholder colours" bug looks like from the inside.
        static int said = 0;
        if (!said) {
            said = 1;
            rt_log("materials: %zu objects painted from %zu distinct materials (%s)",
                   mat_cache.size(), seen.size(),
                   g_project_dir.empty() ? "archive" : "folder");
        }
    } else if (entities_changed) {
        // Re-applying costs 569 sync lookups, so it only happens on the frames
        // where the sync layer actually touched something. dai_doc_sync_apply
        // returning 0 means no entity was created, updated or destroyed - and
        // an entity that was never respawned is still wearing its material.
        for (size_t i = 0; i < mat_cache.size(); ++i) {
            dai_entity e = dai_doc_sync_entity(g_sync, mat_cache[i].first);
            if (e) dai_scene_set_material(g_scene, e, mat_cache[i].second);
        }
    }
    ++mat_frame;

    // The blockout meshes are built from a hash of each node's SHAPE, and a
    // game moves things without reshaping them - so this only ever finds work
    // on the frames where the scene itself changed. Same rebuild rule.
    if (blockout_dirty) { dai_blockout_host_sync(&h); blockout_dirty = false; }
}

// A blockout node carries its SHAPE (blockout.size), but its collision/cull
// box stays at the document default of 0.5 m unless an editor widget touched
// it. The scene layer culls by that box, so a 13x26 m floor whose centre sits
// outside the frustum vanishes - which is exactly what happened here: walls
// (centre in view) drew, the floor did not. Fixing it up once at load is the
// same thing the inspector does when the size is dragged.
static void rt_fix_blockout_extents(void) {
    if (!g_doc) return;
    uint32_t n = dai_doc_count(g_doc);
    std::vector<dai_node> ids(n);
    if (n) dai_doc_nodes(g_doc, ids.data(), n);
    uint32_t fixed = 0;
    for (dai_node id : ids) {
        dai_node_desc r{};
        if (dai_doc_get(g_doc, id, &r) != DAI_OK) continue;
        if (!r.blockout) continue;
        dai_vec3 half{ r.blockout_size.x * 0.5f, r.blockout_size.y * 0.5f,
                       r.blockout_size.z * 0.5f };
        if (half.x <= 0 || half.y <= 0 || half.z <= 0) continue;
        if (std::fabs(half.x - r.half_extent.x) < 1e-4f &&
            std::fabs(half.y - r.half_extent.y) < 1e-4f &&
            std::fabs(half.z - r.half_extent.z) < 1e-4f) continue;
        r.half_extent = half;
        dai_doc_set(g_doc, id, &r);
        ++fixed;
    }
    if (fixed) rt_log("blockout: %u cull boxes taken from blockout.size", fixed);
}

// Light components, straight from the document - the scene layer does not
// carry them, so the host collects them every frame. The editor does exactly
// this; without it a scene lit by its own lamps arrives here lit by nothing
// but the default sun, which is why a neon hall came out grey.
static void rt_apply_lights(dai_renderer *r) {
    if (!r || !g_doc) return;
    static std::vector<dai_light> lights;
    static std::vector<dai_node> ids;
    lights.clear();
    // A scene has a handful of lights and hundreds of everything else, so
    // reading every node's whole descriptor each frame to find them was the
    // wrong way round - dai_doc_count plus dai_doc_nodes plus 569 dai_doc_get
    // measured 1.4 ms per frame for what is usually three lamps. The LIST of
    // light nodes is what changes rarely; where those lights point can still
    // change every frame, and does, because that is read below as before.
    static std::vector<dai_node> light_ids;
    static uint64_t light_frame = 0;
    if ((light_frame++ % 30) == 0 || light_ids.empty()) {
        uint32_t n = dai_doc_count(g_doc);
        ids.resize(n);
        if (n) dai_doc_nodes(g_doc, ids.data(), n);
        light_ids.clear();
        for (dai_node id : ids) {
            dai_node_desc lr{};
            if (dai_doc_get(g_doc, id, &lr) == DAI_OK && lr.light) light_ids.push_back(id);
        }
    }
    int sun_seen = 0;
    for (dai_node id : light_ids) {
        dai_node_desc lr{};
        if (dai_doc_get(g_doc, id, &lr) != DAI_OK || !lr.light) continue;
        dai_vec3 wp{}, ws{ 1, 1, 1 };
        dai_quat wr{ 0, 0, 0, 1 };
        dai_doc_world_transform(g_doc, id, &wp, &wr, &ws);
        dai_vec3 col = (lr.light_color.x || lr.light_color.y || lr.light_color.z)
                     ? lr.light_color : dai_vec3{ 1, 1, 1 };
        float power = lr.light_intensity > 0.0f ? lr.light_intensity : 1.0f;
        float range = lr.light_range > 0.0f ? lr.light_range : 10.0f;
        float x = wr.x, y = wr.y, z = wr.z, w = wr.w;
        dai_vec3 dir{ 2*(x*z + w*y), 2*(y*z - w*x), 1 - 2*(x*x + y*y) };
        if (lr.light == 3) {                     /* a sun, not a point */
            dai_render_sun(r, dai_vec3{ -dir.x, -dir.y, -dir.z }, col, power);
            sun_seen = 1;
            continue;
        }
        dai_light L{};
        L.position = wp; L.color = col; L.intensity = power; L.range = range;
        if (lr.light == 2) {
            L.direction = dai_vec3{ -dir.x, -dir.y, -dir.z };
            L.type = DAI_LIGHT_SPOT;
            float cone = lr.light_cone > 0.0f ? lr.light_cone : 30.0f;
            L.inner_deg = cone * 0.7f;
            L.outer_deg = cone;
        } else {
            L.type = DAI_LIGHT_POINT;
        }
        lights.push_back(L);
    }
    (void)sun_seen;
    dai_render_lights(r, lights.empty() ? nullptr : lights.data(), (uint32_t)lights.size());
}

// One place both the window loop and --shot go through: a screenshot that
// does not contain the HUD cannot be used to check the HUD.

// ---- gui: what a behaviour asked to be drawn this frame --------------------
//
// The editor has had this since behaviours could draw (examples/editor_demo.cpp,
// gui_text_cb and friends); the shipped game did not, so every game that puts
// its score on the screen with gui.text() ran with NO HUD once it was
// exported - the one difference between Ctrl+P and the thing you hand to a
// player. Same shape as the editor's: the calls land in a list during the
// frame and the list is played back in one place, after the world is drawn.
struct RtGuiCmd {
    int kind;                       // 0 text, 1 rect, 2 image, 3 button
    float x, y, w, h, size;
    uint32_t color;
    std::string text;
};
static std::vector<RtGuiCmd> g_gui_cmds;
static float g_gui_w = 0, g_gui_h = 0;

static uint32_t rt_gui_col(double v) {
    if (!(v >= 0.0)) return 0xFFFFFFFFu;
    if (v > 4294967295.0) return 0xFFFFFFFFu;
    return (uint32_t)v;
}
static void rt_gui_text(double x, double y, const char *t, double size, double rgba, void *) {
    RtGuiCmd c{}; c.kind = 0; c.x = (float)x; c.y = (float)y;
    c.size = size > 0 ? (float)size : 24.0f; c.color = rt_gui_col(rgba); c.text = t ? t : "";
    g_gui_cmds.push_back(c);
}
static void rt_gui_rect(double x, double y, double w, double h, double rgba, void *) {
    RtGuiCmd c{}; c.kind = 1; c.x = (float)x; c.y = (float)y; c.w = (float)w; c.h = (float)h;
    c.color = rt_gui_col(rgba);
    g_gui_cmds.push_back(c);
}
static void rt_gui_image(double x, double y, double w, double h, const char *path,
                         double rgba, void *) {
    RtGuiCmd c{}; c.kind = 2; c.x = (float)x; c.y = (float)y; c.w = (float)w; c.h = (float)h;
    c.color = rt_gui_col(rgba); c.text = path ? path : "";
    g_gui_cmds.push_back(c);
}
static int rt_gui_button(double, double, double, double, const char *, void *) {
    return 0;   /* a game reads the mouse itself; no pointer UI in the runtime */
}
static void rt_gui_size(double *w, double *h, void *) {
    if (w) *w = g_gui_w;
    if (h) *h = g_gui_h;
}
static dai_script_gui_host g_gui_host = {
    rt_gui_text, rt_gui_rect, rt_gui_image, rt_gui_button, rt_gui_size, nullptr
};

static void draw_hud(dai_renderer *r, float w, float h) {
    if (!g_ui || !r || !g_doc) return;
    dai_ui_input uin{};      // no pointer, no keys: a HUD is drawn, not operated
    g_gui_w = w; g_gui_h = h;
    dai_ui_begin(g_ui, w, h, &uin);
    dai_hud_draw(g_ui, g_doc, 0.0f, 0.0f, w, h, 1.0f, hud_resolve, nullptr);
    // ...and what the behaviours asked for, on top of it.
    if (!g_gui_cmds.empty()) {
        const float base = dai_ui_text_height(g_ui) > 0.0f ? dai_ui_text_height(g_ui) : 13.0f;
        for (const RtGuiCmd &c : g_gui_cmds) {
            if (c.kind == 1) {
                dai_ui_rect(g_ui, c.x, c.y, c.w, c.h, c.color);
            } else if (c.kind == 0) {
                float k = c.size / base;
                /* the shadow first: white text on a bright wall is unreadable
                 * without it, and a HUD that cannot be read is not a HUD */
                dai_ui_text_scaled(g_ui, c.x + 1.0f, c.y + 1.0f, c.text.c_str(), 0xB0000000u, k);
                dai_ui_text_scaled(g_ui, c.x, c.y, c.text.c_str(), c.color, k);
            }
        }
        g_gui_cmds.clear();
    }
    dai_ui_end(g_ui);
    const dai_ui_draw *draws = nullptr;
    uint32_t nb = dai_ui_draws(g_ui, &draws);
    std::vector<dai_ui_vertex> verts;
    std::vector<uint32_t> counts;
    std::vector<dai_texture> texes;
    for (uint32_t i = 0; i < nb; ++i) {
        if (!draws[i].vertices || !draws[i].count) continue;
        verts.insert(verts.end(), draws[i].vertices, draws[i].vertices + draws[i].count);
        counts.push_back(draws[i].count);
        texes.push_back(draws[i].texture);
    }
    if (!counts.empty())
        dai_render_ui(r, verts.data(), (uint32_t)verts.size(),
                      counts.data(), texes.data(), (uint32_t)counts.size());
}


static dai_vec3 qrot(dai_quat q, dai_vec3 v) {
    float x = q.x, y = q.y, z = q.z, w = q.w;
    dai_vec3 u{ x, y, z };
    float uv = u.x * v.x + u.y * v.y + u.z * v.z;
    float uu = x * x + y * y + z * z;
    dai_vec3 c{ u.y * v.z - u.z * v.y, u.z * v.x - u.x * v.z, u.x * v.y - u.y * v.x };
    return dai_vec3{ 2.0f * uv * u.x + (w * w - uu) * v.x + 2.0f * w * c.x,
                     2.0f * uv * u.y + (w * w - uu) * v.y + 2.0f * w * c.y,
                     2.0f * uv * u.z + (w * w - uu) * v.z + 2.0f * w * c.z };
}

// "Where is this really", the runtime's copy of dai_editor_live_transform's
// play branch: the body is the truth for anything simulated, the document for
// anything that is not (a camera, a marker, a pure graphics node).
static int live_transform(dai_node n, dai_vec3 *pos, dai_quat *rot) {
    dai_node_desc rec{};
    if (!g_doc || dai_doc_get(g_doc, n, &rec) != DAI_OK) return 0;
    dai_vec3 wp{}, ws{ 1, 1, 1 };
    dai_quat wr{ 0, 0, 0, 1 };
    dai_doc_world_transform(g_doc, n, &wp, &wr, &ws);

    dai_entity ent = g_sync ? dai_doc_sync_entity(g_sync, n) : 0;
    dai_body b = (ent && g_scene) ? dai_scene_body(g_scene, ent) : 0;
    dai_transform t{};
    if (!b || dai_body_get(g_world, b, &t) != DAI_OK) {
        if (pos) *pos = wp;
        if (rot) *rot = wr;
        return 1;
    }
    if (rot) *rot = t.rotation;
    if (pos) {
        *pos = t.position;
        // A collider centre offset puts the BODY somewhere the object is not.
        // Undone with the body's rotation, not the document's - the same fix
        // the editor needed.
        if (rec.collider_center.x || rec.collider_center.y || rec.collider_center.z) {
            dai_vec3 off = qrot(t.rotation, dai_vec3{ rec.collider_center.x * ws.x,
                                                      rec.collider_center.y * ws.y,
                                                      rec.collider_center.z * ws.z });
            pos->x -= off.x; pos->y -= off.y; pos->z -= off.z;
        }
    }
    return 1;
}

static void live_set_transform(dai_node n, const dai_vec3 *pos, const dai_quat *rot) {
    if (!g_doc || (!pos && !rot)) return;
    dai_entity ent = g_sync ? dai_doc_sync_entity(g_sync, n) : 0;
    dai_body b = (ent && g_scene) ? dai_scene_body(g_scene, ent) : 0;
    if (!b) {
        // Nothing simulates it: the document is where it lives. This is how a
        // script moves a camera.
        dai_node_desc rec{};
        if (dai_doc_get(g_doc, n, &rec) != DAI_OK) return;
        dai_doc_begin(g_doc, "script");
        if (pos) rec.position = *pos;
        if (rot) rec.rotation = *rot;
        dai_doc_set(g_doc, n, &rec);
        dai_doc_commit(g_doc);
        return;
    }
    // A node with children is a pivot: writing only its body would leave the
    // children standing where they were, because their own revision never
    // moved. The document is the only place the hierarchy is resolved from,
    // so a parent goes there as well.
    dai_node kids[1];
    if (dai_doc_children(g_doc, n, kids, 1) > 0) {
        dai_node_desc rec{};
        if (dai_doc_get(g_doc, n, &rec) == DAI_OK) {
            dai_doc_begin(g_doc, "script");
            if (pos) rec.position = *pos;
            if (rot) rec.rotation = *rot;
            dai_doc_set(g_doc, n, &rec);
            dai_doc_commit(g_doc);
        }
    }
    dai_transform t{};
    if (dai_body_get(g_world, b, &t) != DAI_OK) return;
    if (rot) t.rotation = *rot;
    if (pos) {
        dai_node_desc rec{};
        dai_vec3 wp{}, ws{ 1, 1, 1 };
        dai_quat wrq{ 0, 0, 0, 1 };
        dai_doc_world_transform(g_doc, n, &wp, &wrq, &ws);
        t.position = *pos;
        if (dai_doc_get(g_doc, n, &rec) == DAI_OK &&
            (rec.collider_center.x || rec.collider_center.y || rec.collider_center.z)) {
            dai_vec3 off = qrot(t.rotation, dai_vec3{ rec.collider_center.x * ws.x,
                                                      rec.collider_center.y * ws.y,
                                                      rec.collider_center.z * ws.z });
            t.position.x += off.x; t.position.y += off.y; t.position.z += off.z;
        }
    }
    dai_body_set_transform(g_world, b, t.position, t.rotation);
}

// --------------------------------------------------------------- behaviours

#ifdef DAI_WITH_SCRIPT
static double sh_find(const char *name, void *) {
    if (!name || !*name || !g_doc) return -1.0;
    uint32_t n = dai_doc_count(g_doc);
    std::vector<dai_node> all(n);
    if (n) dai_doc_nodes(g_doc, all.data(), n);
    for (size_t i = 0; i < all.size(); ++i) {
        dai_node_desc r{};
        if (dai_doc_get(g_doc, all[i], &r) == DAI_OK && std::strcmp(r.name, name) == 0)
            return (double)(uint32_t)all[i];
    }
    return -1.0;
}
static int sh_get_pos(double id, double *xyz, void *) {
    dai_vec3 p{};
    if (!live_transform((dai_node)(uint32_t)id, &p, nullptr)) return 0;
    xyz[0] = p.x; xyz[1] = p.y; xyz[2] = p.z;
    return 1;
}
static void sh_set_pos(double id, const double *xyz, void *) {
    dai_vec3 p{ (float)xyz[0], (float)xyz[1], (float)xyz[2] };
    live_set_transform((dai_node)(uint32_t)id, &p, nullptr);
}
static int sh_get_rot(double id, double *xyzw, void *) {
    dai_quat q{};
    if (!live_transform((dai_node)(uint32_t)id, nullptr, &q)) return 0;
    xyzw[0] = q.x; xyzw[1] = q.y; xyzw[2] = q.z; xyzw[3] = q.w;
    return 1;
}
static void sh_set_rot(double id, const double *xyzw, void *) {
    dai_quat q{ (float)xyzw[0], (float)xyzw[1], (float)xyzw[2], (float)xyzw[3] };
    live_set_transform((dai_node)(uint32_t)id, nullptr, &q);
}
// The node's Text component. The document is the truth for it: nothing
// simulates a label, and draw_hud reads the document.
static void sh_set_text(double id, const char *str, void *) {
    dai_node_desc r{};
    if (!g_doc || dai_doc_get(g_doc, (dai_node)(uint32_t)id, &r) != DAI_OK) return;
    std::snprintf(r.text, sizeof(r.text), "%s", str ? str : "");
    if (!r.text_on) r.text_on = 1;
    dai_doc_set(g_doc, (dai_node)(uint32_t)id, &r);
}

// Seam: the component table - "light.intensity", "camera.fov", "blockout.size",
// "script" - shared with the editor and the modelling host.
// include/dai_props_host.inl. Until this was here the shipped game bound the
// FIRST FIVE fields of dai_script_node_host and left the component half null,
// so node.setNum() in an exported game silently did nothing while the same
// behaviour worked in the editor.
#define DAI_PROPS_DOC g_doc
#include "dai_props_host.inl"

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


// Seam: spawning - what scene.spawn()/scene.destroy() mean, shared by every
// host that runs behaviours. include/dai_spawn_host.inl.
#include "dai_spawn_host.inl"

static double sh_spawn(double src, double parent, const char *name, void *) {
    return comp_spawn((dai_node)(uint32_t)src, (dai_node)(uint32_t)parent, name);
}
static int sh_destroy(double id, void *) {
    return comp_destroy((dai_node)(uint32_t)id);
}
static double sh_child_count(double id, void *) {
    return comp_child_count((dai_node)(uint32_t)id);
}
static double sh_child_at(double id, double index, void *) {
    return comp_child_at((dai_node)(uint32_t)id, index);
}
static double sh_parent_of(double id, void *) {
    return comp_parent_of((dai_node)(uint32_t)id);
}

static dai_script_node_host g_node_host = { sh_find, sh_get_pos, sh_set_pos,
                                            sh_get_rot, sh_set_rot, sh_set_text,
                                            sh_get_num, sh_set_num,
                                            sh_get_vec, sh_set_vec,
                                            sh_get_str, sh_set_str,
                                            sh_spawn, sh_destroy,
                                            sh_child_count, sh_child_at, sh_parent_of,
                                            nullptr };

// ---- the PLAY half: keys, the mouse, and the body ------------------------
// The editor binds this from its play state; the runtime IS the play state.
// Without it `input.key("w")` and `body.setVel()` do not exist in a shipped
// game, which means every behaviour that moves anything is decoration.
//
// The window may be null - --headless has none - and then every key is up and
// the mouse never moves. That is a game running with nobody at the controls,
// which is exactly what a headless test wants.
static dai_window *g_win_for_scripts = nullptr;
static double g_mouse_dx = 0, g_mouse_dy = 0;
static int    g_mouse_buttons = 0, g_mouse_have = 0, g_mouse_lx = 0, g_mouse_ly = 0;

static void poll_mouse(void) {
    g_mouse_dx = g_mouse_dy = 0;
    if (!g_win_for_scripts) return;
    int x = 0, y = 0; uint32_t b = 0;
    if (!dai_window_mouse(g_win_for_scripts, &x, &y, &b)) return;
    if (dai_window_mouse_captured(g_win_for_scripts)) {
        // Mouse look: the backend already measured the movement, and the
        // pointer itself never leaves the middle. Subtracting positions here
        // would give zero for every frame and the player could not turn.
        int mdx = 0, mdy = 0;
        dai_window_mouse_delta(g_win_for_scripts, &mdx, &mdy);
        g_mouse_dx = mdx; g_mouse_dy = mdy;
        g_mouse_have = 0;              /* the absolute history means nothing now */
    } else {
        if (g_mouse_have) { g_mouse_dx = x - g_mouse_lx; g_mouse_dy = y - g_mouse_ly; }
        g_mouse_lx = x; g_mouse_ly = y; g_mouse_have = 1;
    }
    g_mouse_buttons = (int)b;
}

static dai_body body_of(double id) {
    dai_entity ent = g_sync ? dai_doc_sync_entity(g_sync, (dai_node)(uint32_t)id) : 0;
    return (ent && g_scene) ? dai_scene_body(g_scene, ent) : 0;
}

// The same spelling the editor answers - one name table, or "w" would mean
// something else in the game than it does in Play.
static int sp_key(const char *name, void *) {
    if (!name || !*name || !g_win_for_scripts) return 0;
    uint32_t code = 0;
    if (!name[1]) {
        // LOWER case. Every window backend stores a letter key as its lower
        // case code point (see dai_key_from_vk: "A".."Z" + 0x20), and the
        // editor asks the same way. This asked for 'W' and got a permanent 0,
        // which is why WASD did nothing in an exported game while it worked
        // fine under Ctrl+P - the one bug that made the shipped build
        // unplayable.
        char c = name[0];
        if (c >= 'A' && c <= 'Z') code = (uint32_t)(c - 'A' + 'a');
        else code = (uint32_t)(unsigned char)c;
    } else {
        std::string n(name);
        for (char &c : n) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if (n == "space")      code = DAI_KEY_SPACE;
        else if (n == "enter" || n == "return") code = DAI_KEY_RETURN;
        else if (n == "escape" || n == "esc")   code = DAI_KEY_ESCAPE;
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
    if (!code) return 0;
    return dai_window_key_down(g_win_for_scripts, code) ? 1 : 0;
}

static int sp_get_vel(double id, double *xyz, void *) {
    if (!xyz) return 0;
    xyz[0] = xyz[1] = xyz[2] = 0.0;
    dai_body b = body_of(id);
    if (!b) return 0;
    dai_vec3 l{}, a{};
    if (dai_body_get_velocity(g_world, b, &l, &a) != DAI_OK) return 0;
    xyz[0] = l.x; xyz[1] = l.y; xyz[2] = l.z;
    return 1;
}

static void sp_set_vel(double id, const double *xyz, void *) {
    dai_body b = body_of(id);
    if (!b || !xyz) return;
    dai_vec3 l{}, a{};
    dai_body_get_velocity(g_world, b, &l, &a);        // keep the spin
    dai_body_set_velocity(g_world, b,
                          dai_vec3{ (float)xyz[0], (float)xyz[1], (float)xyz[2] }, a);
}

static void sp_impulse(double id, const double *xyz, void *) {
    dai_body b = body_of(id);
    if (!b || !xyz) return;
    dai_body_add_impulse(g_world, b,
                         dai_vec3{ (float)xyz[0], (float)xyz[1], (float)xyz[2] });
}

// "Is there floor under me" - the same measurement the editor makes: a body
// that is neither rising nor sinking is standing on something.
static int sp_grounded(double id, void *) {
    double v[3] = { 0, 0, 0 };
    if (!sp_get_vel(id, v, nullptr)) return 0;
    return (v[1] > -0.35 && v[1] < 0.35) ? 1 : 0;
}

static void sp_mouse(double *dx, double *dy, int *buttons, void *) {
    if (dx) *dx = g_mouse_dx;
    if (dy) *dy = g_mouse_dy;
    if (buttons) *buttons = g_mouse_buttons;
}

static dai_script_play_host g_play_host = {
    sp_key, sp_get_vel, sp_set_vel, sp_impulse, sp_grounded, sp_mouse, nullptr
};

struct RunningScript { dai_script *s; std::string path; };
static std::vector<RunningScript> g_running;

// The node's `script` field, in the editor's own format:
//   "a.js{target=Box};b.js"   path plus the references assigned in the
//                             inspector, which arrive as `params`.

// ---- an inspector value, with the TYPE the script declared ----------------
//
// The document stores a behaviour's fields as text: "walkSpeed=2.2,autoWalk=true".
// The editor turns each one back into a number, a boolean or a string using the
// type the script declared, and hands the script `params.walkSpeed = 2.2`.
// The runtime used to hand it `params.walkSpeed = "2.2"` - EVERY field a
// string - so a behaviour that checks `typeof v === "number"` fell back to its
// default in the shipped game and worked in the editor. That is the worst kind
// of bug: the game is not broken, it is subtly different.
//
// The types come out of the script itself, which is the only place they exist:
//   // @param float speed = 6      an explicit declaration
//   let sprinting = false;         a literal declaration (docs/SCRIPTING.md)
// and anything the file does not declare falls back to what the text looks
// like, because a value nobody declared still has to arrive as something.
static std::string script_param_type(const char *code, size_t len,
                                     const std::string &name) {
    if (!code || !len || name.empty()) return std::string();
    std::string src(code, len);
    size_t pos = 0;
    while ((pos = src.find("@param", pos)) != std::string::npos) {
        size_t eol = src.find('\n', pos);
        std::string line = src.substr(pos + 6, eol == std::string::npos ? eol : eol - pos - 6);
        std::vector<std::string> tok;
        size_t i = 0;
        while (i < line.size()) {
            while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
            size_t st = i;
            while (i < line.size() && line[i] != ' ' && line[i] != '\t' && line[i] != '=') ++i;
            if (i > st) tok.push_back(line.substr(st, i - st));
            if (i < line.size() && line[i] == '=') break;
        }
        pos = (eol == std::string::npos) ? src.size() : eol;
        if (tok.size() >= 2 && tok[1] == name) return tok[0];
        if (tok.size() == 1 && tok[0] == name) return std::string();
    }
    // "let name = <literal>" - the declaration IS the field.
    std::string want = "let " + name;
    size_t d = src.find(want);
    if (d != std::string::npos) {
        size_t eq = src.find('=', d);
        size_t eol = src.find('\n', d);
        if (eq != std::string::npos && (eol == std::string::npos || eq < eol)) {
            size_t v = eq + 1;
            while (v < src.size() && (src[v] == ' ' || src[v] == '\t')) ++v;
            if (v < src.size()) {
                if (src[v] == '"' || src[v] == '\'') return "string";
                if (!src.compare(v, 4, "true") || !src.compare(v, 5, "false")) return "bool";
                if ((src[v] >= '0' && src[v] <= '9') || src[v] == '-' || src[v] == '.') return "float";
            }
        }
    }
    return std::string();
}

// The value, spelled as JavaScript. A string is quoted (and its quotes and
// backslashes escaped, or a Windows path in a text field would end the literal
// and the whole params object with it).
static std::string script_param_js(const std::string &val, const std::string &type) {
    auto quoted = [&]() {
        std::string out = "\"";
        for (char c : val) {
            if (c == '"' || c == '\\') out += '\\';
            out += c;
        }
        out += '"';
        return out;
    };
    auto looks_number = [&]() {
        if (val.empty()) return false;
        char *end = nullptr;
        std::strtod(val.c_str(), &end);
        return end && *end == 0;
    };
    if (type == "float" || type == "int")
        return looks_number() ? val : std::string("0");
    if (type == "bool")
        return (val == "true" || val == "1") ? "true" : "false";
    if (type == "string" || type == "node" || type == "camera" || type == "light" ||
        type == "rigidbody" || type == "collider" || type == "sprite" ||
        type == "audio" || type == "mesh" || type == "transform" || type == "object")
        return quoted();
    // Undeclared: what it looks like. A node reference is a name, so it stays
    // a string - which is what scene.find() wants anyway.
    if (val == "true" || val == "false") return val;
    if (looks_number()) return val;
    return quoted();
}

static void scripts_start(void) {
    if (!g_doc) return;
    uint32_t n = dai_doc_count(g_doc);
    std::vector<dai_node> all(n);
    if (n) dai_doc_nodes(g_doc, all.data(), n);
    char err[256];
    for (size_t k = 0; k < all.size(); ++k) {
        dai_node_desc r{};
        if (dai_doc_get(g_doc, all[k], &r) != DAI_OK || !r.script[0]) continue;
        std::string cur;
        std::vector<std::string> entries;
        for (const char *c = r.script; ; ++c) {
            if (*c == ';' || !*c) {
                if (!cur.empty()) entries.push_back(cur);
                cur.clear();
                if (!*c) break;
            } else cur += *c;
        }
        for (size_t ei = 0; ei < entries.size(); ++ei) {
            const std::string &entry = entries[ei];
            std::string path = entry, inner;
            size_t b = entry.find('{');
            if (b != std::string::npos) {
                path = entry.substr(0, b);
                size_t en = entry.find('}', b);
                inner = entry.substr(b + 1, en == std::string::npos ? en : en - b - 1);
            }
            // Scripts live under assets/ in the project, and the field holds
            // the path RELATIVE to that - the same string the editor writes.
            std::string vpath = "assets/" + path;
            size_t len = 0;
            void *code = dai_vfs_read(vpath.c_str(), &len);
            if (!code) {
                rt_log("script '%s': %s", vpath.c_str(), dai_vfs_last_error());
                continue;
            }
            dai_script *s = dai_script_create(err, sizeof(err));
            if (!s) { rt_log("script: %s", err); dai_vfs_free(code); continue; }
            dai_script_bind_nodes(s, &g_node_host);
            dai_script_bind_play(s, &g_play_host);
            dai_script_bind_gui(s, &g_gui_host);
            if (g_audio) dai_script_bind_audio(s, &g_audio_host);
            // The node the behaviour is ON, so a script can move itself
            // without looking its own name up.
            dai_script_set_number(s, "self", (double)(uint32_t)all[k]);
            // The fields, typed the way the script declared them - see
            // script_param_type above for why this is not "everything is a
            // string".
            if (!inner.empty()) {
                std::string params_js = "var params = {";
                size_t pos = 0;
                while (pos < inner.size()) {
                    size_t comma = inner.find(',', pos);
                    std::string kv = inner.substr(pos, comma == std::string::npos ? comma : comma - pos);
                    size_t eq = kv.find('=');
                    if (eq != std::string::npos && eq > 0) {
                        std::string key = kv.substr(0, eq), val = kv.substr(eq + 1);
                        std::string ty = script_param_type((const char *)code, len, key);
                        params_js += "\"" + key + "\":" + script_param_js(val, ty) + ",";
                    }
                    if (comma == std::string::npos) break;
                    pos = comma + 1;
                }
                params_js += "};";
                dai_script_eval(s, params_js.c_str(), "params", err, sizeof(err));
            }
            if (dai_script_eval(s, (const char *)code, path.c_str(), err, sizeof(err)) != DAI_OK) {
                rt_log("script %s: %s", path.c_str(), err);
                dai_script_destroy(s);
                dai_vfs_free(code);
                continue;
            }
            dai_vfs_free(code);
            // The object model, then `self` as one of its Nodes - the same two
            // evals, in the same order, the editor does. state.self alone was
            // not enough: every behaviour ever written says `self`, and in a
            // shipped game it was not defined.
            err[0] = 0;
            if (dai_script_eval(s, DAI_JS_PRELUDE, "prelude", err, sizeof(err)) != DAI_OK && err[0])
                rt_log("prelude: %s", err);
            {
                char selfjs[96];
                std::snprintf(selfjs, sizeof(selfjs),
                              "var self = __wrapSelf(%u);", (unsigned)(uint32_t)all[k]);
                err[0] = 0;
                dai_script_eval(s, selfjs, "self", err, sizeof(err));
            }
            err[0] = 0;
            dai_script_call(s, "init", err, sizeof(err));
            if (err[0]) rt_log("script %s init: %s", path.c_str(), err);
            RunningScript rs;
            rs.s = s;
            rs.path = path;
            g_running.push_back(rs);
        }
    }
    rt_log("scripts: %u running", (unsigned)g_running.size());
}

static void scripts_frame(double dt) {
    poll_mouse();
    for (size_t i = 0; i < g_running.size(); ++i) {
        char err[192] = { 0 };
        // state.dt, so a behaviour can be frame rate independent without
        // asking the host for a clock it does not have. The editor sets the
        // same name; a script must not have to tell the two apart.
        dai_script_set_number(g_running[i].s, "dt", dt);
        if (dai_script_call(g_running[i].s, "frame", err, sizeof(err)) != DAI_OK && err[0])
            rt_log("script %s: %s", g_running[i].path.c_str(), err);
    }
}

static void scripts_stop(void) {
    for (size_t i = 0; i < g_running.size(); ++i) dai_script_destroy(g_running[i].s);
    g_running.clear();
}
#else
static void scripts_start(void) { rt_log("scripts: not compiled in"); }
static void scripts_frame(double) {}
static void scripts_stop(void) {}
#endif

// ----------------------------------------------------------------- camera

static const char *CAMERA_TAG = "MainCamera";

static dai_node find_camera(void) {
    if (!g_doc) return DAI_INVALID_NODE;
    uint32_t n = dai_doc_count(g_doc);
    std::vector<dai_node> all(n);
    if (n) dai_doc_nodes(g_doc, all.data(), n);
    for (size_t i = 0; i < all.size(); ++i) {
        dai_node_desc r{};
        if (dai_doc_get(g_doc, all[i], &r) == DAI_OK && std::strcmp(r.tag, CAMERA_TAG) == 0)
            return all[i];
    }
    // Scenes written before the format carried `tag` at all lost it on save,
    // so the camera the editor made comes back untagged. Its NAME survived.
    // Recognising that is the difference between an old project shipping and
    // an old project shipping a black window.
    for (size_t i = 0; i < all.size(); ++i) {
        dai_node_desc r{};
        if (dai_doc_get(g_doc, all[i], &r) != DAI_OK) continue;
        if (r.no_body && std::strstr(r.name, "Camera")) {
            rt_log("no '%s' tag; using the node named '%s'", CAMERA_TAG, r.name);
            return all[i];
        }
    }
    return DAI_INVALID_NODE;
}

// With no camera in the scene the editor draws "No cameras rendering" and
// shows nothing, which is right for an editor: it is telling the author about
// a mistake. A shipped game showing a black window tells the PLAYER nothing,
// so it frames the scene instead and says so in the log.
static void fallback_camera(dai_vec3 *eye, dai_vec3 *look) {
    dai_vec3 lo{ 1e9f, 1e9f, 1e9f }, hi{ -1e9f, -1e9f, -1e9f };
    uint32_t n = dai_doc_count(g_doc);
    std::vector<dai_node> all(n);
    if (n) dai_doc_nodes(g_doc, all.data(), n);
    int any = 0;
    for (size_t i = 0; i < all.size(); ++i) {
        dai_vec3 p{};
        if (!live_transform(all[i], &p, nullptr)) continue;
        lo.x = p.x < lo.x ? p.x : lo.x; lo.y = p.y < lo.y ? p.y : lo.y; lo.z = p.z < lo.z ? p.z : lo.z;
        hi.x = p.x > hi.x ? p.x : hi.x; hi.y = p.y > hi.y ? p.y : hi.y; hi.z = p.z > hi.z ? p.z : hi.z;
        any = 1;
    }
    if (!any) { lo = dai_vec3{ -1, -1, -1 }; hi = dai_vec3{ 1, 1, 1 }; }
    dai_vec3 c{ (lo.x + hi.x) * 0.5f, (lo.y + hi.y) * 0.5f, (lo.z + hi.z) * 0.5f };
    float rx = hi.x - lo.x, ry = hi.y - lo.y, rz = hi.z - lo.z;
    float rad = std::sqrt(rx * rx + ry * ry + rz * rz) * 0.5f + 2.0f;
    *look = c;
    *eye = dai_vec3{ c.x + rad * 1.1f, c.y + rad * 0.75f, c.z + rad * 1.6f };
}

// --------------------------------------------------------------------- main

int main(int argc, char **argv) {
    char self[1024] = { 0 };
    dai_vfs_self_path(self, sizeof(self));
    log_open(self);
    rt_log("daidalos runtime %s", dai_version());
    if (self[0]) rt_log("exe: %s", self);

    // ---- what to run
    const char *source = nullptr;   // a project dir or a .dpk, when given
    const char *shot_path = nullptr;
    int headless_frames = 0;
    // Offline mix: every frame the same number of samples the tick is worth is
    // pulled out of the mixer and written to a WAV at the end. That is how a
    // build server hears a game - the pictures already have --shot.
    const char *audio_dump = nullptr;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--headless") { headless_frames = (i + 1 < argc) ? std::atoi(argv[++i]) : 120; }
        else if (a == "--shot") { shot_path = (i + 1 < argc) ? argv[++i] : nullptr; }
        else if (a == "--audio-dump") { audio_dump = (i + 1 < argc) ? argv[++i] : nullptr; }
        else if (a == "--help" || a == "-h") {
            rt_log("usage: %s [project-dir | archive.dpk] [--headless <frames>] "
                   "[--shot <file.ppm>] [--audio-dump <file.wav>]", argv[0]);
            return 0;
        } else if (!source) source = argv[i];
    }
    if (headless_frames < 0) headless_frames = 0;
    // A screenshot needs a renderer but no window, which is exactly what a
    // machine with a GPU and no display can do - and what proves the shipped
    // archive reaches actual pixels rather than merely parsing.

    // ---- mount. The export case first: the archive is our own tail.
    char err[512] = { 0 };
    int mounted = 0;
    if (dai_vfs_mount_archive(nullptr, 0, err, sizeof(err)) == DAI_OK) {
        rt_log("mounted: %s", dai_vfs_mount_name(0));
        mounted = 1;
    } else if (self[0]) {
        rt_log("no archive in this executable (%s)", err);
    }
    if (!mounted && source) {
        if (dai_vfs_mount_dir(source, 0) == DAI_OK) {
            rt_log("mounted: %s", dai_vfs_mount_name(0));
            g_project_dir = source;
            mounted = 1;
        } else if (dai_vfs_mount_archive(source, 0, err, sizeof(err)) == DAI_OK) {
            rt_log("mounted: %s", dai_vfs_mount_name(0));
            mounted = 1;
        } else rt_log("cannot mount '%s': %s", source, err);
    }
    if (!mounted && self[0]) {
        // Last resort: an archive lying beside the runtime. Handy while
        // developing the exporter - the same bytes, without the copy.
        std::string beside = dir_of(std::string(self)) + "/game.dpk";
        if (dai_vfs_mount_archive(beside.c_str(), 0, err, sizeof(err)) == DAI_OK) {
            rt_log("mounted: %s", dai_vfs_mount_name(0));
            mounted = 1;
        }
    }
    if (!mounted) {
        rt_log("nothing to run: no archive appended, no project directory given.");
        rt_log("usage: %s [project-dir | archive.dpk] [--headless <frames>]", argv[0]);
        return 1;
    }

    // ---- how to start
    dai_boot_config cfg = dai_boot_config_default();
    size_t blen = 0;
    void *btext = dai_vfs_read(DAI_PACK_BOOT_PATH, &blen);
    if (btext) {
        if (dai_boot_config_parse((const char *)btext, blen, &cfg) != DAI_OK)
            rt_log("%s is not a boot config - using the defaults", DAI_PACK_BOOT_PATH);
        dai_vfs_free(btext);
    } else {
        // A plain project directory has no boot.cfg. That is not an error: it
        // is what running the runtime straight out of the editor's project
        // folder looks like.
        rt_log("no %s - starting %s at %dx%d", DAI_PACK_BOOT_PATH, cfg.scene, cfg.width, cfg.height);
    }
    rt_log("boot: scene='%s' title='%s' %dx%d msaa=%d tick=%dHz physics=%d",
           cfg.scene, cfg.title, cfg.width, cfg.height, cfg.msaa, cfg.tick_hz, cfg.physics_backend);
    if (cfg.fullscreen)
        rt_log("note: fullscreen was requested; the window backend has no full screen mode yet");

    // ---- sound. Opened before the world so a script that plays something in
    // init() already has a backend behind it.
    if (cfg.audio_bank[0]) {
        char aerr[256] = { 0 };
        if (g_project_dir.empty()) {
            /* Shipped build: the bank and every wav live in the archive that is
             * appended to this executable. The mixer gets a reader instead of a
             * folder - see dai_audio_open_reader. This used to print "skipped"
             * and the exported game was silent, which made every sound the
             * editor plays a lie about the thing you hand to a player. */
            std::string bank = cfg.audio_bank;
            std::string root = bank.substr(0, bank.find_last_of('/') + 1);
            g_audio = dai_audio_open_reader(bank.c_str(), root.c_str(),
                                            headless_frames ? 0 : 1,
                                            rt_audio_vfs_read, rt_audio_vfs_release, nullptr,
                                            aerr, sizeof(aerr));
            if (!g_audio) rt_log("audio: %s (the game runs silent)", aerr);
            else          rt_log("audio: bank '%s' out of the archive", bank.c_str());
            g_audio_root = root;
        } else {
            std::string bank = g_project_dir + "/" + cfg.audio_bank;
            std::string root = bank.substr(0, bank.find_last_of('/') + 1);
            g_audio = dai_audio_open(bank.c_str(), root.c_str(), headless_frames ? 0 : 1,
                                     aerr, sizeof(aerr));
            if (!g_audio) rt_log("audio: %s (the game runs silent)", aerr);
            else          rt_log("audio: bank '%s', samples from '%s'", bank.c_str(), root.c_str());
            g_audio_root = root;
        }
    }

    // ---- the world
    dai_config wc{};
    wc.backend = cfg.physics_backend;
    wc.tick_hz = (uint32_t)cfg.tick_hz;
    wc.max_bodies = (uint32_t)cfg.max_bodies;
    wc.snapshot_ring = 8;      // no timeline scrubbing in a game
    wc.seed = 1;
    if (dai_create(&wc, &g_world) != DAI_OK) {
        rt_log("physics world failed to start");
        return 1;
    }
    dai_set_gravity(g_world, dai_vec3{ cfg.gravity[0], cfg.gravity[1], cfg.gravity[2] });
    rt_log("physics: %s", dai_backend_name(g_world));

    g_scene = dai_scene_create(g_world);
    g_doc = dai_doc_create();
    g_sync = dai_doc_sync_create(g_doc, g_scene);

    char lerr[512] = { 0 };
    std::vector<std::string> open_files;
    if (!load_scene_vfs(g_doc, cfg.scene, 0, open_files, lerr, sizeof(lerr))) {
        rt_log("cannot load the startup scene: %s", lerr);
        return 1;
    }
    rt_log("scene '%s': %u nodes", cfg.scene, dai_doc_count(g_doc));

    // ---- the renderer, unless we were told not to open a window
    dai_renderer *r = nullptr;
    dai_window *win = nullptr;
    if (!headless_frames || shot_path) {
        dai_render_desc rd{};
        rd.width = (uint32_t)cfg.width;
        rd.height = (uint32_t)cfg.height;
        rd.msaa = cfg.msaa;
        r = dai_render_create(&rd, err, sizeof(err));
        if (!r) { rt_log("renderer failed: %s", err); return 1; }
        rt_log("gpu: %s", dai_render_device_name(r));
        if (!headless_frames) {
            win = dai_window_open(r, cfg.title, (uint32_t)cfg.width, (uint32_t)cfg.height,
                                  err, sizeof(err));
            if (!win) { rt_log("window failed: %s", err); return 1; }
        }
        // The keyboard and the mouse the behaviours read. Null in --headless,
        // which is a game running with nobody at the controls.
#ifdef DAI_WITH_SCRIPT
        g_win_for_scripts = win;
#endif

        g_assets.r = r;
        dai_doc_sync_resolver(g_sync, runtime_resolve, &g_assets);

        // The look of the world. The editor sets these from its own panels; a
        // game has no panels, so they are the engine's defaults until the
        // scene format grows a place to put them.
        dai_render_sun(r, dai_vec3{ 0.42f, 0.80f, 0.42f }, dai_vec3{ 1.0f, 0.95f, 0.86f }, 1.3f);
        dai_render_ambient(r, dai_vec3{ 0.24f, 0.40f, 0.72f }, dai_vec3{ 0.26f, 0.24f, 0.20f }, 0.38f);
        dai_render_exposure(r, 0.45f);
        dai_render_clear_color(r, 0.05f, 0.06f, 0.08f);
        dai_render_shadow_extent(r, 24.0f);
        dai_render_sky(r, 1);

        // The HUD's font. A system font, like the editor's - the game ships
        // no typeface of its own yet, and one that fails to load must cost
        // the labels, not the game.
        char ferr[256] = { 0 };
        g_hud_font = dai_font_load_ui(16.0f, ferr, sizeof(ferr));
        if (!g_hud_font) {
            rt_log("no UI font (%s) - text components will not draw", ferr);
        } else {
            uint32_t aw = 0, ah = 0;
            const uint8_t *atlas = dai_font_atlas(g_hud_font, &aw, &ah);
            std::vector<uint8_t> rgba((size_t)aw * ah * 4);
            for (size_t i = 0; i < (size_t)aw * ah; ++i) {
                rgba[i*4+0] = 255; rgba[i*4+1] = 255; rgba[i*4+2] = 255; rgba[i*4+3] = atlas[i];
            }
            dai_texture tex = dai_render_texture_create(r, rgba.data(), aw, ah, 0);
            g_ui = dai_ui_create(g_hud_font, tex);
        }
        hud_strings_load(cfg.language);
    }

    rt_fix_blockout_extents();
    dai_doc_sync_apply(g_sync);
    rt_log("live: %u entities", dai_scene_count(g_scene));

    dai_node cam = find_camera();
    if (cam == DAI_INVALID_NODE)
        rt_log("no node tagged '%s' - framing the scene instead", CAMERA_TAG);

    // Behaviours run from here on. There is no play button: this IS play.
    scripts_start();

    std::vector<dai_render_instance> inst(4096);
    auto last = std::chrono::high_resolution_clock::now();

    if (headless_frames) {
        // No window, no GPU: mount, load, simulate, report. This is what runs
        // on a build server, and it is a real end to end test of everything
        // except the pixels.
        double dt = 1.0 / (double)cfg.tick_hz;
        std::vector<float> mix;                       /* interleaved stereo */
        const uint32_t mix_rate = 48000;
        const uint32_t mix_per_frame = (uint32_t)(mix_rate / (cfg.tick_hz > 0 ? cfg.tick_hz : 60));
        if (audio_dump && g_audio) mix.reserve((size_t)headless_frames * mix_per_frame * 2);
        for (int f = 0; f < headless_frames; ++f) {
            float alpha = 1.0f;
            dai_advance(g_world, dt, &alpha);
            scripts_frame(dt);
            dai_doc_sync_apply(g_sync);
            if (g_audio) dai_audio_update(g_audio);
            if (audio_dump && g_audio) {
                size_t at = mix.size();
                mix.resize(at + (size_t)mix_per_frame * 2, 0.0f);
                dai_audio_render(g_audio, mix.data() + at, mix_per_frame);
            }
            uint32_t n = dai_scene_instances(g_scene, inst.data(), (uint32_t)inst.size(), alpha);
            if (f == 0 || f == headless_frames - 1) {
                rt_log("frame %d: tick %llu, %u instances", f,
                       (unsigned long long)dai_current_tick(g_world), n);
                uint32_t shown = n < 16 ? n : 16;
                for (uint32_t i = 0; i < shown; ++i)
                    rt_log("   inst %u  pos %.3f %.3f %.3f", i,
                           (double)inst[i].position.x, (double)inst[i].position.y,
                           (double)inst[i].position.z);
            }
        }
        dai_vec3 eye{}, look{};
        if (cam != DAI_INVALID_NODE) {
            dai_quat q{ 0, 0, 0, 1 };
            live_transform(cam, &eye, &q);
            dai_vec3 d = qrot(q, dai_vec3{ 0, 0, -1 });
            look = dai_vec3{ eye.x + d.x, eye.y + d.y, eye.z + d.z };
        } else fallback_camera(&eye, &look);
        rt_log("camera: eye %.2f %.2f %.2f -> %.2f %.2f %.2f",
               (double)eye.x, (double)eye.y, (double)eye.z,
               (double)look.x, (double)look.y, (double)look.z);
        if (shot_path && r) {
            dai_render_camera(r, eye, look, dai_vec3{ 0, 1, 0 }, 60.0f, 0.1f, 500.0f);
            float alpha = 1.0f;
            rt_apply_materials(r, true);
            rt_apply_lights(r);
            uint32_t n = dai_scene_instances(g_scene, inst.data(), (uint32_t)inst.size(), alpha);
            // The HUD belongs in the screenshot too. A --shot that leaves it
            // out is a --shot that cannot be used to check it, which is the
            // one job a headless screenshot has.
            draw_hud(r, (float)cfg.width, (float)cfg.height);
            if (dai_render_frame(r, inst.data(), n) != DAI_OK)
                rt_log("render failed: %s", dai_render_last_error(r));
            size_t sl = std::strlen(shot_path);
            dai_result wr = (sl > 4 && std::strcmp(shot_path + sl - 4, ".png") == 0)
                            ? dai_render_write_png(r, shot_path)
                            : dai_render_write_ppm(r, shot_path);
            rt_log("shot: %s (%u instances) -> %s", shot_path, n, wr == DAI_OK ? "ok" : "FAILED");
        }
        if (audio_dump && !mix.empty()) {
            // A 16 bit PCM WAV, written by hand: the engine has no sound file
            // writer and this is eleven lines.
            FILE *w = std::fopen(audio_dump, "wb");
            if (!w) rt_log("audio dump: cannot write %s", audio_dump);
            else {
                uint32_t frames_n = (uint32_t)(mix.size() / 2);
                uint32_t data_bytes = frames_n * 2 * 2;
                uint32_t riff = 36 + data_bytes;
                uint16_t ch = 2, bits = 16, fmt = 1;
                uint32_t rate = mix_rate, byte_rate = rate * ch * bits / 8;
                uint16_t align = (uint16_t)(ch * bits / 8);
                std::fwrite("RIFF", 1, 4, w); std::fwrite(&riff, 4, 1, w);
                std::fwrite("WAVEfmt ", 1, 8, w);
                uint32_t sub1 = 16; std::fwrite(&sub1, 4, 1, w);
                std::fwrite(&fmt, 2, 1, w); std::fwrite(&ch, 2, 1, w);
                std::fwrite(&rate, 4, 1, w); std::fwrite(&byte_rate, 4, 1, w);
                std::fwrite(&align, 2, 1, w); std::fwrite(&bits, 2, 1, w);
                std::fwrite("data", 1, 4, w); std::fwrite(&data_bytes, 4, 1, w);
                double peak = 0;
                for (float v : mix) { double a2 = v < 0 ? -v : v; if (a2 > peak) peak = a2; }
                for (float v : mix) {
                    float c = v > 1.0f ? 1.0f : (v < -1.0f ? -1.0f : v);
                    int16_t q = (int16_t)(c * 32767.0f);
                    std::fwrite(&q, 2, 1, w);
                }
                std::fclose(w);
                rt_log("audio dump: %s, %.2f s, peak %.3f", audio_dump,
                       (double)frames_n / (double)mix_rate, peak);
            }
        }
        rt_log("headless run finished after %d frames", headless_frames);
    } else {
        // A first person game owns the mouse: hidden, held in the middle, and
        // reported as movement. Without this the player turns until the cursor
        // reaches the edge of the screen and the world simply stops turning -
        // and the desktop cursor sits on top of the game the whole time.
        dai_window_mouse_capture(win, 1);
        int esc_was_down = 0;
        while (dai_window_poll(win)) {
            auto now = std::chrono::high_resolution_clock::now();
            double dt = std::chrono::duration<double>(now - last).count();
            last = now;
            if (dt > 0.25) dt = 0.25;          // a dragged window is not a jump

            // Escape lets go of the mouse first and only quits when the
            // pointer is already free: a game that closes on the key people
            // press to get their cursor back is a game nobody can alt-tab out
            // of. Clicking back into the picture takes the mouse again.
            int esc = dai_window_key_down(win, DAI_KEY_ESCAPE);
            if (esc && !esc_was_down) {
                if (dai_window_mouse_captured(win)) dai_window_mouse_capture(win, 0);
                else break;
            }
            esc_was_down = esc;
            if (!dai_window_mouse_captured(win)) {
                uint32_t mb = 0;
                dai_window_mouse(win, nullptr, nullptr, &mb);
                if (mb & (1u << 1)) dai_window_mouse_capture(win, 1);
            }

            float alpha = 1.0f;
            /*PROFILE*/
            static int prof_on = getenv("DAI_PROFILE") != nullptr;
            static double pa=0,pb=0,pc=0,pd=0,pe=0,pf=0,pg=0; static int pn=0;
            auto T0 = std::chrono::high_resolution_clock::now();
            dai_advance(g_world, dt, &alpha);
            auto T1 = std::chrono::high_resolution_clock::now();
            scripts_frame(dt);
            auto T2 = std::chrono::high_resolution_clock::now();
            uint32_t sync_changed = dai_doc_sync_apply(g_sync);
            auto T3 = std::chrono::high_resolution_clock::now();
            rt_apply_materials(r, sync_changed != 0);
            auto T4 = std::chrono::high_resolution_clock::now();
            rt_apply_lights(r);
            auto T5 = std::chrono::high_resolution_clock::now();
            if (g_audio) dai_audio_update(g_audio);
            auto T6 = std::chrono::high_resolution_clock::now();
            if (prof_on) {
                pa += std::chrono::duration<double,std::milli>(T1-T0).count();
                pb += std::chrono::duration<double,std::milli>(T2-T1).count();
                pc += std::chrono::duration<double,std::milli>(T3-T2).count();
                pd += std::chrono::duration<double,std::milli>(T4-T3).count();
                pe += std::chrono::duration<double,std::milli>(T5-T4).count();
                pf += std::chrono::duration<double,std::milli>(T6-T5).count();
            }

            uint32_t ww = (uint32_t)cfg.width, wh = (uint32_t)cfg.height;
            dai_window_size(win, &ww, &wh);
            if (ww && wh && (ww != dai_render_width(r) || wh != dai_render_height(r)))
                dai_render_resize(r, ww, wh);

            dai_vec3 eye{}, look{};
            float fov = 60.0f;
            if (cam != DAI_INVALID_NODE && dai_doc_valid(g_doc, cam)) {
                dai_quat q{ 0, 0, 0, 1 };
                live_transform(cam, &eye, &q);
                dai_vec3 d = qrot(q, dai_vec3{ 0, 0, -1 });
                look = dai_vec3{ eye.x + d.x, eye.y + d.y, eye.z + d.z };
            } else fallback_camera(&eye, &look);
            dai_render_camera(r, eye, look, dai_vec3{ 0, 1, 0 }, fov, 0.1f, 500.0f);

            uint32_t n = dai_scene_instances(g_scene, inst.data(), (uint32_t)inst.size(), alpha);

            draw_hud(r, (float)ww, (float)wh);

            auto T7 = std::chrono::high_resolution_clock::now();
            dai_render_frame(r, inst.data(), n);
            dai_window_present(win);
            if (prof_on) {
                auto T8 = std::chrono::high_resolution_clock::now();
                pg += std::chrono::duration<double,std::milli>(T8-T7).count();
                double fms = std::chrono::duration<double,std::milli>(T8-T0).count();
                static double worst = 0; if (fms > worst) worst = fms;
                if (++pn >= 60) {
                    rt_log("PROF max-frame-ms(cpu+render)=%.2f", worst); worst = 0;
                    rt_log("PROF/frame ms: advance=%.3f scripts=%.3f sync=%.3f "
                           "materials=%.3f lights=%.3f audio=%.3f render=%.3f  SUM=%.3f",
                           pa/pn, pb/pn, pc/pn, pd/pn, pe/pn, pf/pn, pg/pn,
                           (pa+pb+pc+pd+pe+pf+pg)/pn);
                    pa=pb=pc=pd=pe=pf=pg=0; pn=0;
                }
            }
        }
    }

    scripts_stop();
    if (g_ui) dai_ui_destroy(g_ui);
    if (g_hud_font) dai_font_free(g_hud_font);
    if (g_hud_strings) dai_strings_destroy(g_hud_strings);
    for (auto it = g_assets.models.begin(); it != g_assets.models.end(); ++it)
        if (it->second) { if (r) dai_model_release(r, it->second); else dai_model_free(it->second); }
    g_assets.models.clear();
    if (win) dai_window_close(win);
    if (r) dai_render_destroy(r);
    if (g_audio) { dai_audio_close(g_audio); g_audio = nullptr; }
    dai_doc_sync_destroy(g_sync);
    dai_doc_destroy(g_doc);
    dai_scene_destroy(g_scene);
    dai_destroy(g_world);
    dai_vfs_unmount_all();
    rt_log("bye");
    if (g_log) std::fclose(g_log);
    return 0;
}
