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

// One place both the window loop and --shot go through: a screenshot that
// does not contain the HUD cannot be used to check the HUD.
static void draw_hud(dai_renderer *r, float w, float h) {
    if (!g_ui || !r || !g_doc) return;
    dai_ui_input uin{};      // no pointer, no keys: a HUD is drawn, not operated
    dai_ui_begin(g_ui, w, h, &uin);
    dai_hud_draw(g_ui, g_doc, 0.0f, 0.0f, w, h, 1.0f, hud_resolve, nullptr);
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
static dai_script_node_host g_node_host = { sh_find, sh_get_pos, sh_set_pos,
                                            sh_get_rot, sh_set_rot, nullptr };

struct RunningScript { dai_script *s; std::string path; };
static std::vector<RunningScript> g_running;

// The node's `script` field, in the editor's own format:
//   "a.js{target=Box};b.js"   path plus the references assigned in the
//                             inspector, which arrive as `params`.
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
            std::string path = entry, params_js;
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
                    if (eq != std::string::npos && eq > 0)
                        params_js += "\"" + kv.substr(0, eq) + "\":\"" + kv.substr(eq + 1) + "\",";
                    if (comma == std::string::npos) break;
                    pos = comma + 1;
                }
                params_js += "};";
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
            // The node the behaviour is ON, so a script can move itself
            // without looking its own name up.
            dai_script_set_number(s, "self", (double)(uint32_t)all[k]);
            if (!params_js.empty()) dai_script_eval(s, params_js.c_str(), "params", err, sizeof(err));
            if (dai_script_eval(s, (const char *)code, path.c_str(), err, sizeof(err)) != DAI_OK) {
                rt_log("script %s: %s", path.c_str(), err);
                dai_script_destroy(s);
                dai_vfs_free(code);
                continue;
            }
            dai_vfs_free(code);
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

static void scripts_frame(void) {
    for (size_t i = 0; i < g_running.size(); ++i) {
        char err[192] = { 0 };
        dai_script_call(g_running[i].s, "frame", err, sizeof(err));
    }
}

static void scripts_stop(void) {
    for (size_t i = 0; i < g_running.size(); ++i) dai_script_destroy(g_running[i].s);
    g_running.clear();
}
#else
static void scripts_start(void) { rt_log("scripts: not compiled in"); }
static void scripts_frame(void) {}
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
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--headless") { headless_frames = (i + 1 < argc) ? std::atoi(argv[++i]) : 120; }
        else if (a == "--shot") { shot_path = (i + 1 < argc) ? argv[++i] : nullptr; }
        else if (a == "--help" || a == "-h") {
            rt_log("usage: %s [project-dir | archive.dpk] [--headless <frames>] [--shot <file.ppm>]",
                   argv[0]);
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
        for (int f = 0; f < headless_frames; ++f) {
            float alpha = 1.0f;
            dai_advance(g_world, dt, &alpha);
            scripts_frame();
            dai_doc_sync_apply(g_sync);
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
        rt_log("headless run finished after %d frames", headless_frames);
    } else {
        while (dai_window_poll(win)) {
            auto now = std::chrono::high_resolution_clock::now();
            double dt = std::chrono::duration<double>(now - last).count();
            last = now;
            if (dt > 0.25) dt = 0.25;          // a dragged window is not a jump

            if (dai_window_key_down(win, DAI_KEY_ESCAPE)) break;

            float alpha = 1.0f;
            dai_advance(g_world, dt, &alpha);
            scripts_frame();
            dai_doc_sync_apply(g_sync);

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

            dai_render_frame(r, inst.data(), n);
            dai_window_present(win);
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
    dai_doc_sync_destroy(g_sync);
    dai_doc_destroy(g_doc);
    dai_scene_destroy(g_scene);
    dai_destroy(g_world);
    dai_vfs_unmount_all();
    rt_log("bye");
    if (g_log) std::fclose(g_log);
    return 0;
}
