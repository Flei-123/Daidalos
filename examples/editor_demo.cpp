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
#include "dai_render.h"
#include "dai_assets.h"
#include "dai_project.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#ifndef _WIN32
#include <sys/stat.h>
#endif
#ifdef _WIN32
#include <windows.h>
#endif

// ---- the project host: the editor's "New Project" needs a disk ------------
// A project is a folder: scenes and assets under one root. That is the entire
// definition - a Unity project is also just a folder with opinions.
static char g_projects_root[512] = { 0 };
static char g_scene_path[512] = { 0 };

static dai_project *g_project = nullptr;
static char g_assets_dir[512] = { 0 };

// The project list, from disk, through the project layer - it knows what a
// project is (assets/, scenes/, settings/) so this does not have to.
static const char *project_list(uint32_t index, void *) {
    static std::vector<std::string> names;
    if (index == 0) {
        names.clear();
        char buf[64][DAI_PROJECT_NAME_MAX];
        uint32_t n = dai_project_list(g_projects_root, buf[0], 64, DAI_PROJECT_NAME_MAX);
        if (n > 64) n = 64;
        for (uint32_t i = 0; i < n; ++i) names.push_back(buf[i]);
    }
    if (index >= names.size()) return nullptr;
    return names[index].c_str();
}

// Opening a project is the ONE thing that decides where everything else comes
// from: the scene, the assets, the settings. The editor never runs without
// one - that is why this is called before the first frame as well as from the
// project window.
static int open_project_path(const char *path) {
    char err[256] = { 0 };
    dai_project *np = dai_project_open(path, err, sizeof(err));
    if (!np) { std::printf("project: %s\n", err); return 0; }
    if (g_project) dai_project_close(g_project);
    g_project = np;
    std::snprintf(g_scene_path, sizeof(g_scene_path), "%s", dai_project_scene_path(g_project));
    std::snprintf(g_assets_dir, sizeof(g_assets_dir), "%s", dai_project_asset_dir(g_project));
    dai_prefs pr = dai_prefs_default();
    dai_prefs_load(&pr);
    std::snprintf(pr.last_project, sizeof(pr.last_project), "%s", dai_project_path(g_project));
    dai_prefs_save(&pr);
    return 1;
}

static int project_create(const char *name, void *) {
    char err[256] = { 0 };
    dai_project *np = dai_project_create(g_projects_root, name, err, sizeof(err));
    if (!np) { std::printf("project: %s\n", err); return 0; }
    std::string path = dai_project_path(np);
    dai_project_close(np);
    return open_project_path(path.c_str());
}

static int project_open(const char *name, void *) {
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

// "New Script" writes a template, because the first script anyone writes
// should not start with guessing what the entry point is called.
static int script_create(const char *name, void *) {
    if (!name || !*name || !g_assets_dir[0]) return 0;
    for (const char *c = name; *c; ++c) {
        bool ok = (*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') ||
                  (*c >= '0' && *c <= '9') || *c == '-' || *c == '_' || *c == '/';
        if (!ok) return 0;
    }
    if (std::strstr(name, "..")) return 0;   // never escape the assets folder
    char path[640];
    std::snprintf(path, sizeof(path), "%s/%s.js", g_assets_dir, name);
    FILE *f = std::fopen(path, "wb");
    if (!f) return 0;
    std::fputs("// A Daidalos behaviour. The engine calls the globals it finds:\n"
               "//   init()   once when play starts\n"
               "//   frame()  every rendered frame\n"
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
static dai_project_settings *g_psettings = nullptr;
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
    static const char *const BACKENDS[] = { "Jolt", "Talos", "None" };
    dai_ui_option(ui, "Physics", &ps.physics_backend, BACKENDS, 3);
    dai_ui_num_field(ui, "Friction", &ps.default_friction, 0.01f, 0.0f, 10.0f, "psfric");
    dai_ui_num_field(ui, "Bounce", &ps.default_restitution, 0.01f, 0.0f, 1.0f, "psrest");
    dai_ui_input_text(ui, "App name", ps.app_name, sizeof(ps.app_name));
    dai_ui_separator(ui);
    dai_ui_label(ui, "Tags");
    for (int i = 0; i < 4; ++i) {
        char lbl[16];
        std::snprintf(lbl, sizeof(lbl), "Tag %d", i);
        dai_ui_input_text(ui, lbl, ps.tags[i], DAI_PROJECT_TAG_MAX);
    }
    if (std::memcmp(&before, &ps, sizeof(ps)) != 0)
        dai_project_settings_save(g_project, &ps);
    dai_ui_label(ui, "Changes are saved immediately.");
}

// The settings window's font swap needs what main() owns, so main() publishes
// it here. One editor, one font - this is not a place that needs generality.
static dai_renderer *g_renderer = nullptr;
static dai_ui       *g_ui = nullptr;
static dai_font     *g_font = nullptr;

static void apply_font(float px, void *) {
    if (!g_renderer || !g_ui) return;
    char err[256] = { 0 };
    dai_font *nf = dai_font_load_ui(px, err, sizeof(err));
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

int main(int argc, char **argv) {
    const char *scene_path = argc > 1 ? argv[1] : nullptr;
#ifdef _WIN32
    std::snprintf(g_projects_root, sizeof(g_projects_root), "C:\\daidalos\\projects");
#else
    std::snprintf(g_projects_root, sizeof(g_projects_root), "projects");
#endif

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
    cfg.tick_hz = psettings.tick_hz > 0 ? (uint32_t)psettings.tick_hz : 60;
    cfg.max_bodies = psettings.max_bodies > 0 ? (uint32_t)psettings.max_bodies : 4096;
    cfg.snapshot_ring = 120; cfg.seed = 1;
    dai_world *w = nullptr;
    if (dai_create(&cfg, &w) != DAI_OK) { std::printf("world failed\n"); return 1; }
    dai_scene *sc = dai_scene_create(w);
    dai_doc *doc = dai_doc_create();
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
    dai_renderer *r = dai_render_create(&rd, err, sizeof(err));
    if (!r) { std::printf("renderer failed: %s\n", err); return 1; }
    dai_window *win = dai_window_open(r, "Daidalos Editor", W, H, err, sizeof(err));
    if (!win) { std::printf("window failed: %s\n", err); return 1; }
    // A line in the log that proves WHICH build is running: two editor.exes
    // on the same machine look identical from the task list.
    std::printf("editor up: %ux%u, %s\n", W, H, dai_version());

    // 13 px, not 17: this is an editor, and the panels are full of numeric
    // fields. On Windows the window is DPI aware now, so 13 px is 13 real
    // pixels instead of 13 stretched to 20 by the desktop scaling.
    dai_font *font = dai_font_load_ui(13.0f, err, sizeof(err));
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

    // Vector icons for the toolbar and the inspector's component headers.
    // Rasterised HERE, at 16 px, because that is the size this interface draws
    // them at - the SVG sources are resolution independent, the atlas is not.
    dai_icons *icons = dai_icons_create(16.0f);
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
    dai_editor_ui_script_host(panels, script_create, nullptr);
    dai_editor_ui_folder_host(panels, folder_create, nullptr);
    g_psettings = &psettings;
    g_ui_for_settings = ui;
    dai_editor_ui_project_settings_host(panels, draw_project_settings, nullptr);

    // The layout is the user's; it belongs on disk next to the projects, not
    // in the binary. Loading it before the first frame means the editor opens
    // the way it was left.
    {
        char lpath[512];
        std::snprintf(lpath, sizeof(lpath), "%s/layout.txt", g_projects_root);
        FILE *lf = std::fopen(lpath, "rb");
        if (lf) {
            std::string txt;
            char chunk[1024];
            size_t got;
            while ((got = std::fread(chunk, 1, sizeof(chunk), lf)) > 0) txt.append(chunk, got);
            std::fclose(lf);
            dai_editor_ui_layout_load(panels, txt.c_str());
        }
    }

    // The asset layer: a mounted folder of glTF/JS, hot reloaded, bound to the
    // document sync so "asset crate.glb" on a node actually resolves.
    dai_assets *assets = dai_assets_create(r, 1);

    // Settings: the UI size is a font reload, and the font is ours.
    g_renderer = r; g_ui = ui; g_font = font;
    dai_editor_ui_settings_host(panels, apply_font, 13.0f * prefs.ui_scale, nullptr);
    if (prefs.ui_scale > 1.02f || prefs.ui_scale < 0.98f) apply_font(13.0f * prefs.ui_scale, nullptr);
    dai_editor_cam_speed(ed, prefs.cam_speed > 0.1f ? prefs.cam_speed : 6.0f);
    if (prefs.gizmo_px > 10.0f) dai_editor_gizmo_size(ed, prefs.gizmo_px);
    if (prefs.snap_translate > 0.0f || prefs.snap_rotate_deg > 0.0f)
        dai_editor_snap(ed, prefs.snap_translate, prefs.snap_rotate_deg, 0.1f);

    auto last = std::chrono::high_resolution_clock::now();
    int prev_keys[8] = { 0 };
    std::vector<dai_render_instance> inst(4096);
    int prev_f2 = 0, prev_backspace = 0, prev_enter = 0, prev_tab = 0;
    int prev_edit_keys[6] = { 0 };
    int prev_nav_keys[2] = { 0 };
    int prev_ctrl_a = 0;

    while (dai_window_poll(win)) {
        auto now = std::chrono::high_resolution_clock::now();
        float dt = std::chrono::duration<float>(now - last).count();
        last = now;

        int mx = 0, my = 0;
        uint32_t buttons = 0;
        dai_window_mouse(win, &mx, &my, &buttons);
        float wheel = dai_window_wheel(win);

        dai_editor_cam_input ci{};
        ci.mouse_x = (float)mx; ci.mouse_y = (float)my;
        ci.mouse_left = (buttons & (1u << 1)) ? 1 : 0;
        ci.mouse_middle = (buttons & (1u << 2)) ? 1 : 0;
        ci.mouse_right = (buttons & (1u << 3)) ? 1 : 0;
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
        ci.key_focus = !type_lock && dai_window_key_down(win, DAI_KEY_F);
        ci.dt = dt;

        // W/E/R switch gizmo mode, but only when the camera is not flying -
        // otherwise pressing W to walk forward would also change the tool.
        int ctrl = dai_window_key_down(win, DAI_KEY_CTRL_L) || dai_window_key_down(win, DAI_KEY_CTRL_R);
        int keys[8] = {
            ci.key_w, ci.key_e, dai_window_key_down(win, DAI_KEY_R),
            dai_window_key_down(win, DAI_KEY_Z), dai_window_key_down(win, DAI_KEY_Y),
            dai_window_key_down(win, DAI_KEY_DELETE), dai_window_key_down(win, DAI_KEY_D),
            dai_window_key_down(win, DAI_KEY_SPACE),
        };
        auto pressed = [&](int i) { return keys[i] && !prev_keys[i]; };
        // While a field is being typed into, the keyboard belongs to the field.
        // Otherwise renaming an object to "Wide Crate" switches the gizmo to
        // rotate, duplicates the selection and starts play mode on the way.
        int typing = dai_ui_text_active(ui);
        if (!ci.mouse_right && !ctrl && !typing) {
            if (pressed(0)) dai_editor_gizmo_mode(ed, DAI_GIZMO_TRANSLATE);
            if (pressed(1)) dai_editor_gizmo_mode(ed, DAI_GIZMO_ROTATE);
            if (pressed(2)) dai_editor_gizmo_mode(ed, DAI_GIZMO_SCALE);
        }
        if (ctrl && pressed(3) && !typing) dai_editor_undo(ed);
        if (ctrl && pressed(4) && !typing) dai_editor_redo(ed);
        if (ctrl && pressed(6) && !typing) dai_editor_duplicate_selection(ed);
        // Not while a field is being typed into: Delete belongs to the caret
        // then, not to the scene.
        if (pressed(5) && !dai_ui_text_active(ui)) dai_editor_delete_selection(ed);
        if (pressed(7) && !typing) {
            if (dai_editor_state_get(ed) == DAI_EDITOR_PLAY) dai_editor_pause(ed);
            else dai_editor_play(ed);
        }
        if (ctrl && dai_window_key_down(win, DAI_KEY_S) && scene_path) {
            if (dai_doc_save(doc, scene_path) == DAI_OK) std::printf("saved %s\n", scene_path);
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
            dai_doc_sync_reset(sync);
            dai_doc_sync_apply(sync);
            // <project>/assets is the mounted folder.
            std::snprintf(g_assets_dir, sizeof(g_assets_dir), "%s", current_scene);
            char *slash = std::strstr(g_assets_dir, "/scenes/");
            if (slash) {
                *slash = 0;
                std::strncat(g_assets_dir, "/assets", sizeof(g_assets_dir) - std::strlen(g_assets_dir) - 1);
                if (assets) {
                    dai_assets_destroy(assets);
                    assets = dai_assets_create(r, 1);
                    if (assets) {
                        dai_assets_mount_dir(assets, g_assets_dir, 0);
                        dai_assets_bind(assets, sync);
                    }
                }
            }
            std::printf("project: %s\n", current_scene);
        }

        if (dai_editor_ui_take_save(panels) && scene_path) {
            if (dai_doc_save(doc, scene_path) == DAI_OK) std::printf("saved %s\n", scene_path);
        }
        if (dai_editor_ui_take_refresh(panels) && assets) dai_assets_poll(assets);

        // ---- assets: hot reload, list, and what the Project window clicked
        if (assets) {
            if (dai_assets_poll(assets)) dai_assets_bind(assets, sync);
            static uint32_t fed_rev = 0xFFFFFFFFu;
            uint32_t rev = dai_assets_revision(assets);
            if (rev != fed_rev) {
                fed_rev = rev;
                static char paths[256][96];
                static const char *ptrs[256];
                uint32_t n2 = dai_assets_list(assets, paths[0], 256, 96);
                if (n2 > 256) n2 = 256;
                for (uint32_t i = 0; i < n2; ++i) ptrs[i] = paths[i];
                dai_editor_ui_asset_list(panels, ptrs, n2);
            }
            const char *pick = nullptr;
            int as_tree = 0;
            if (dai_editor_ui_take_asset(panels, &pick, &as_tree) && pick) {
                if (dai_assets_model_blocking(assets, pick)) {
                    dai_node made = dai_assets_instantiate(assets, doc, pick, 0);
                    if (made) {
                        dai_doc_sync_apply(sync);
                        dai_editor_select(ed, made, 0);
                    }
                }
            }
        }

        uint32_t ww = W, wh = H;
        dai_window_size(win, &ww, &wh);

        // Follow the window. The frame is blitted onto it and stretched, so a
        // fixed render resolution means a picture of the wrong shape (a 16:9
        // frame across a 21:9 monitor) drawn with soft, scaled up text - and
        // the UI, which lays itself out in window pixels, would be drawing into
        // a buffer of a different size. One resolution for the window, the
        // renderer and the interface is the only arrangement where all three
        // agree.
        if (ww != dai_render_width(r) || wh != dai_render_height(r)) {
            if (dai_render_resize(r, ww, wh) == DAI_OK)
                dai_editor_camera_viewport(ed, (float)ww, (float)wh);
        }

        // The UI has to run before the viewport, because "is the pointer over a
        // panel" is only known once the panels have been laid out this frame.
        // F2 renames the selection, in the hierarchy where the name lives.
        if (dai_window_key_down(win, DAI_KEY_F2) && !prev_f2 && !dai_ui_text_active(ui) &&
            dai_editor_selection_count(ed) > 0)
            dai_editor_ui_rename(panels, dai_editor_selected(ed, 0));
        prev_f2 = dai_window_key_down(win, DAI_KEY_F2);

        // Real text events. dai_window_key_down answers "is it held", which
        // is the camera's question - the inspector's question is "what was
        // typed", and answering that with held keys repeats every letter for
        // as long as the finger is down. The window backend saw the key
        // press events; it hands them over as code points, already
        // shift-resolved. */
        uint32_t typed[8] = { 0 };
        dai_window_text(win, typed, 8);

        // The menu swallows the right button: while one is open, right is not
        // the camera's look button - otherwise dismissing a menu with the same
        // button that summoned it also turns the world.
        dai_ui_input in{};
        in.mouse_x = (float)mx; in.mouse_y = (float)my;
        in.mouse_down = ci.mouse_left;
        in.right_down = dai_editor_ui_menu_open(panels) ? 0 : ci.mouse_right;
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
        in.double_click = dai_window_double_click(win);
        dai_ui_begin(ui, (float)ww, (float)wh, &in);
        dai_editor_ui_frame(panels, (float)ww, (float)wh);
        dai_ui_end(ui);

        dai_editor_ui_viewport(panels, &ci);

        // The layout, once, a second in: enough frames for the dock to settle,
        // early enough to catch it going wrong.
        static int dump_at = 90;
        if (dump_at > 0 && --dump_at == 0) {
            char lay[640];
            dai_editor_ui_layout_dump(panels, lay, sizeof(lay));
            std::printf("layout: %s\n", lay);
        }

        // The pointer says what is under it: an I-beam over a field, a resize
        // arrow on a window edge. The UI knows which widget that is; only the
        // window can set the shape.
        dai_window_cursor(win, dai_ui_cursor(ui));

        float alpha = 1.0f;
        dai_editor_advance(ed, dt, &alpha);

        dai_vec3 eye, look;
        {   // read the camera back out of the editor so both agree exactly
            float cx, cy, cw2, ch2;
            dai_editor_ui_viewport_rect(panels, &cx, &cy, &cw2, &ch2);
            dai_vec3 o, d;
            dai_editor_ray(ed, cx + cw2 * 0.5f, cy + ch2 * 0.5f, &o, &d);
            eye = o;
            look = dai_vec3{ o.x + d.x, o.y + d.y, o.z + d.z };
        }
        // The EDITOR camera is always updated from the editor's own state, in
        // both views: picking, the gizmo and the scene view all project
        // through it, and a game view that overwrote it would leave the scene
        // view pointing wherever the player camera happened to be.
        // The viewport is the scene window's body rect: the camera projects
        // into exactly that rectangle, the renderer clips the world to it, and
        // picking reads clicks in the same pixels. Three sides of one truth.
        float vrx = 0, vry = 0, vrw = (float)ww, vrh = (float)wh;
        dai_editor_ui_viewport_rect(panels, &vrx, &vry, &vrw, &vrh);
        dai_editor_camera_viewport_rect(ed, vrx, vry, vrw, vrh);
        dai_render_world_clip(r, vrx, vry, vrw, vrh);
        dai_editor_camera(ed, eye, look, dai_vec3{ 0, 1, 0 }, 55.0f, 0.1f, 300.0f,
                          vrw, vrh);

        // What gets RENDERED is the game camera while the Game tab is up.
        dai_vec3 reye = eye, rlook = look;
        float rfov = 55.0f;
        if (dai_editor_ui_view(panels) == DAI_VIEW_GAME) {
            dai_vec3 ge{}, gl{};
            float gf = 60.0f;
            if (dai_editor_ui_game_camera(panels, &ge, &gl, &gf)) {
                reye = ge; rlook = gl; rfov = gf;
            }
        }
        dai_render_camera(r, reye, rlook, dai_vec3{ 0, 1, 0 }, rfov, 0.1f, 300.0f);

        uint32_t n = dai_scene_instances(sc, inst.data(), (uint32_t)inst.size(), alpha);

        const dai_ui_draw *draws = nullptr;
        uint32_t nb = dai_ui_draws(ui, &draws);
        std::vector<dai_ui_vertex> verts;
        std::vector<uint32_t> counts;
        std::vector<dai_texture> texes;
        for (uint32_t i = 0; i < nb; ++i) {
            verts.insert(verts.end(), draws[i].vertices, draws[i].vertices + draws[i].count);
            counts.push_back(draws[i].count);
            texes.push_back(draws[i].texture);
        }
        dai_render_ui(r, verts.data(), (uint32_t)verts.size(), counts.data(), texes.data(), nb);
        dai_render_frame(r, inst.data(), n);
        dai_window_present(win);
    }

    {
        char lpath[512];
        std::snprintf(lpath, sizeof(lpath), "%s/layout.txt", g_projects_root);
        size_t need = dai_editor_ui_layout_save(panels, nullptr, 0);
        std::string txt(need + 1, '\0');
        dai_editor_ui_layout_save(panels, &txt[0], txt.size());
        FILE *lf = std::fopen(lpath, "wb");
        if (lf) { std::fwrite(txt.c_str(), 1, need, lf); std::fclose(lf); }
    }
    {   // what this human set on this machine, kept for the next start
        prefs.cam_speed = dai_editor_cam_speed_get(ed);
        dai_prefs_save(&prefs);
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
    return 0;
}
