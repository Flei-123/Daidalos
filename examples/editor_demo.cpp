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

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#endif

// ---- the project host: the editor's "New Project" needs a disk ------------
// A project is a folder: scenes and assets under one root. That is the entire
// definition - a Unity project is also just a folder with opinions.
static char g_projects_root[512] = { 0 };
static char g_scene_path[512] = { 0 };

static const char *project_list(uint32_t index, void *) {
    static std::vector<std::string> names;
#ifdef _WIN32
    if (index == 0) {
        names.clear();
        std::string pat = std::string(g_projects_root) + "\\*";
        WIN32_FIND_DATAA fd{};
        HANDLE h = FindFirstFileA(pat.c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
                if (fd.cFileName[0] == '.') continue;
                names.push_back(fd.cFileName);
            } while (FindNextFileA(h, &fd));
            FindClose(h);
        }
    }
#endif
    if (index >= names.size()) return nullptr;
    return names[index].c_str();
}

static int project_path(const char *name, char *out, size_t n) {
    if (!name || !*name) return 0;
    for (const char *c = name; *c; ++c) {
        // A project name is a folder name. Anything else is how "my project"
        // becomes ../somewhere.
        bool ok = (*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') ||
                  (*c >= '0' && *c <= '9') || *c == '-' || *c == '_' || *c == ' ';
        if (!ok) return 0;
    }
    size_t rl = std::strlen(g_projects_root), nl = std::strlen(name);
    if (rl + 1 + nl + 1 > n) return 0;
    std::memcpy(out, g_projects_root, rl);
    out[rl] = '/';
    std::memcpy(out + rl + 1, name, nl + 1);
    return 1;
}

static int project_create(const char *name, void *) {
    char path[512];
    if (!project_path(name, path, sizeof(path))) return 0;
#ifdef _WIN32
    CreateDirectoryA(g_projects_root, nullptr);
    if (!CreateDirectoryA(path, nullptr) && GetLastError() != ERROR_ALREADY_EXISTS)
        return 0;
    std::string scenes = std::string(path) + "/scenes";
    std::string assets = std::string(path) + "/assets";
    CreateDirectoryA(scenes.c_str(), nullptr);
    CreateDirectoryA(assets.c_str(), nullptr);
#endif
    if (std::strlen(path) + 22 <= sizeof(g_scene_path)) {
        std::snprintf(g_scene_path, sizeof(g_scene_path), "%s/scenes/main.daidalos", path);
    }
    return 1;
}

static int project_open(const char *name, void *) {
    char path[512];
    if (!project_path(name, path, sizeof(path))) return 0;
    if (std::strlen(path) + 22 <= sizeof(g_scene_path)) {
        std::snprintf(g_scene_path, sizeof(g_scene_path), "%s/scenes/main.daidalos", path);
    }
    return 1;
}

// The renderer's inventory, so the inspector's mesh picker shows names
// instead of a number nobody chose.
static const char *mesh_name_of(uint32_t mesh, void *) {
    switch (mesh) {
    case DAI_MESH_BOX:     return "Box (builtin)";
    case DAI_MESH_SPHERE:  return "Sphere (builtin)";
    case DAI_MESH_CAPSULE: return "Capsule (builtin)";
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

    dai_config cfg{};
    cfg.tick_hz = 60; cfg.max_bodies = 4096; cfg.snapshot_ring = 120; cfg.seed = 1;
    dai_world *w = nullptr;
    if (dai_create(&cfg, &w) != DAI_OK) { std::printf("world failed\n"); return 1; }
    dai_scene *sc = dai_scene_create(w);
    dai_doc *doc = dai_doc_create();
    dai_doc_sync *sync = dai_doc_sync_create(doc, sc);

    char err[256] = { 0 };
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
    dai_render_shadow_extent(r, 24.0f);
    dai_render_sky(r, 1);

    dai_editor *ed = dai_editor_create(doc, sync);
    dai_editor_camera(ed, dai_vec3{ 6.0f, 4.5f, 9.5f }, dai_vec3{ 0, 1, 0 }, dai_vec3{ 0, 1, 0 },
                      55.0f, 0.1f, 300.0f, (float)W, (float)H);
    dai_editor_ui *panels = dai_editor_ui_create(ed, ui);
    dai_editor_ui_project_host(panels, project_list, project_create, project_open, nullptr);
    dai_editor_ui_mesh_host(panels, mesh_name_of, 4, nullptr);   // the builtins

    auto last = std::chrono::high_resolution_clock::now();
    int prev_keys[8] = { 0 };
    std::vector<dai_render_instance> inst(4096);
    int prev_f2 = 0, prev_backspace = 0, prev_enter = 0, prev_tab = 0;

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
        ci.key_w = dai_window_key_down(win, DAI_KEY_W);
        ci.key_a = dai_window_key_down(win, DAI_KEY_A);
        ci.key_s = dai_window_key_down(win, DAI_KEY_S);
        ci.key_d = dai_window_key_down(win, DAI_KEY_D);
        ci.key_q = dai_window_key_down(win, DAI_KEY_Q);
        ci.key_e = dai_window_key_down(win, DAI_KEY_E);
        ci.key_shift = dai_window_key_down(win, DAI_KEY_SHIFT_L) || dai_window_key_down(win, DAI_KEY_SHIFT_R);
        ci.key_alt = dai_window_key_down(win, DAI_KEY_ALT_L) || dai_window_key_down(win, DAI_KEY_ALT_R);
        ci.key_focus = dai_window_key_down(win, DAI_KEY_F);
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
        if (!ci.mouse_right && !ctrl) {
            if (pressed(0)) dai_editor_gizmo_mode(ed, DAI_GIZMO_TRANSLATE);
            if (pressed(1)) dai_editor_gizmo_mode(ed, DAI_GIZMO_ROTATE);
            if (pressed(2)) dai_editor_gizmo_mode(ed, DAI_GIZMO_SCALE);
        }
        if (ctrl && pressed(3)) dai_editor_undo(ed);
        if (ctrl && pressed(4)) dai_editor_redo(ed);
        if (ctrl && pressed(6)) dai_editor_duplicate_selection(ed);
        if (pressed(5)) dai_editor_delete_selection(ed);
        if (pressed(7)) {
            if (dai_editor_state_get(ed) == DAI_EDITOR_PLAY) dai_editor_pause(ed);
            else dai_editor_play(ed);
        }
        if (ctrl && dai_window_key_down(win, DAI_KEY_S) && scene_path) {
            if (dai_doc_save(doc, scene_path) == DAI_OK) std::printf("saved %s\n", scene_path);
        }
        std::memcpy(prev_keys, keys, sizeof(keys));

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
        if (dai_window_key_down(win, DAI_KEY_F2) && !prev_f2 &&
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
        uint32_t ntyped = dai_window_text(win, typed, 8);

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
        dai_ui_begin(ui, (float)ww, (float)wh, &in);
        dai_editor_ui_frame(panels, (float)ww, (float)wh);
        dai_ui_end(ui);

        dai_editor_ui_viewport(panels, &ci);

        float alpha = 1.0f;
        dai_editor_advance(ed, dt, &alpha);

        dai_vec3 eye, look;
        {   // read the camera back out of the editor so both agree exactly
            dai_vec3 o, d;
            dai_editor_ray(ed, (float)ww * 0.5f, (float)wh * 0.5f, &o, &d);
            eye = o;
            look = dai_vec3{ o.x + d.x, o.y + d.y, o.z + d.z };
        }
        dai_editor_camera(ed, eye, look, dai_vec3{ 0, 1, 0 }, 55.0f, 0.1f, 300.0f,
                          (float)ww, (float)wh);
        dai_render_camera(r, eye, look, dai_vec3{ 0, 1, 0 }, 55.0f, 0.1f, 300.0f);

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

    dai_editor_ui_destroy(panels);
    dai_editor_destroy(ed);
    dai_ui_destroy(ui);
    if (icons) dai_icons_free(icons);
    if (font) dai_font_free(font);
    dai_window_close(win);
    dai_render_destroy(r);
    dai_doc_sync_destroy(sync);
    dai_doc_destroy(doc);
    dai_scene_destroy(sc);
    dai_destroy(w);
    return 0;
}
