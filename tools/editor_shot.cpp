// Renders the whole editor - scene, gizmo, hierarchy, inspector, toolbar,
// timeline - to PNGs, so the panels can be looked at instead of only asserted
// about.
//
//   DAI_SHADER_DIR=shaders ./build/editor_shot /tmp/out [W] [H] [GAUNTLET_DIR]
//
// The working shots are `editor_*.png` in the output folder. With a fifth
// argument it also writes `09-game-mode-editor.png` into that folder - the
// game-mode picture the review shows next to the drone show panels.

#include "dai_editor_ui.h"
#include "dai_render.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

int main(int argc, char **argv) {
    std::string outdir = argc > 1 ? argv[1] : "/tmp";
    // Where the one picture the review looks at goes, under the name the
    // review knows it by: `09-game-mode-editor.png`. Empty means "only the
    // editor_*.png working shots".
    std::string gauntlet = argc > 4 ? argv[4] : "";

    dai_config cfg{};
    cfg.tick_hz = 60; cfg.max_bodies = 256; cfg.physics_threads = 1;
    cfg.snapshot_ring = 64; cfg.seed = 9;
    dai_world *w = nullptr;
    if (dai_create(&cfg, &w) != DAI_OK) { std::printf("world failed\n"); return 1; }
    dai_scene *sc = dai_scene_create(w);
    dai_doc *doc = dai_doc_create();
    dai_doc_sync *sync = dai_doc_sync_create(doc, sc);

    dai_node_desc g = dai_node_desc_default();
    std::snprintf(g.name, sizeof(g.name), "Ground");
    g.motion = DAI_STATIC;
    g.half_extent = { 9, 0.5f, 9 };
    g.position = { 0, -0.5f, 0 };
    g.color = { 0.20f, 0.22f, 0.19f };
    dai_doc_add(doc, &g);

    dai_node_desc grp = dai_node_desc_default();
    std::snprintf(grp.name, sizeof(grp.name), "Stack");
    grp.no_body = 1;
    grp.position = { -2.2f, 0, 0 };
    dai_node stack = dai_doc_add(doc, &grp);

    dai_node target = 0;
    for (int i = 0; i < 3; ++i) {
        dai_node_desc b = dai_node_desc_default();
        std::snprintf(b.name, sizeof(b.name), "crate%d", i);
        b.motion = DAI_DYNAMIC;
        b.parent = stack;
        b.half_extent = { 0.6f, 0.6f, 0.6f };
        b.position = { 0, 0.6f + (float)i * 1.25f, 0 };
        dai_doc_add(doc, &b);
    }
    for (int i = 0; i < 2; ++i) {
        dai_node_desc b = dai_node_desc_default();
        std::snprintf(b.name, sizeof(b.name), "ball%d", i);
        b.motion = DAI_DYNAMIC;
        b.shape = DAI_SHAPE_SPHERE;
        b.half_extent = { 0.7f, 0.7f, 0.7f };
        b.position = { 2.4f, 0.7f + (float)i * 1.6f, 0.4f };
        dai_node n = dai_doc_add(doc, &b);
        if (i == 0) target = n;
    }
    dai_doc_sync_apply(sync);
    dai_step(w);

    // Overridable, so the ultrawide case Justin actually hit can be rendered
    // here instead of only on his desk.
    const uint32_t W = argc > 2 ? (uint32_t)atoi(argv[2]) : 1280;
    const uint32_t H = argc > 3 ? (uint32_t)atoi(argv[3]) : 720;
    dai_render_desc rd{};
    rd.width = W; rd.height = H; rd.msaa = 4;
    char err[256] = { 0 };
    dai_renderer *r = dai_render_create(&rd, err, sizeof(err));
    if (!r) { std::printf("renderer failed: %s\n", err); return 1; }

    dai_font *font = dai_font_load_ui(13.0f, err, sizeof(err));
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

    dai_vec3 eye{ 5.4f, 4.0f, 8.6f }, look{ 0, 1.1f, 0 }, up{ 0, 1, 0 };
    dai_render_camera(r, eye, look, up, 55.0f, 0.1f, 200.0f);
    // Direction *towards* the sun, not the way the light travels.
    dai_render_sun(r, dai_vec3{ 0.42f, 0.80f, 0.42f }, dai_vec3{ 1.0f, 0.95f, 0.86f }, 1.3f);
    dai_render_ambient(r, dai_vec3{ 0.24f, 0.40f, 0.72f }, dai_vec3{ 0.26f, 0.24f, 0.20f }, 0.38f);
    dai_render_exposure(r, 0.45f);
    dai_render_shadow_extent(r, 20.0f);
    dai_render_sky(r, 1);

    dai_editor *ed = dai_editor_create(doc, sync);
    dai_editor_camera(ed, eye, look, up, 55.0f, 0.1f, 200.0f, (float)W, (float)H);
    dai_editor_select(ed, target, 0);
    dai_editor_ui *panels = dai_editor_ui_create(ed, ui);

    // `file` is the picture's name; the ordinary shots write editor_<name>.png
    // into outdir, and the one the gauntlet collects writes its own name.
    auto shot_to = [&](const char *name, const std::string &path, float mx, float my) {
        std::vector<dai_render_instance> inst(256);
        uint32_t n = dai_scene_instances(sc, inst.data(), (uint32_t)inst.size(), 1.0f);

        dai_ui_input in{};
        in.mouse_x = mx; in.mouse_y = my;

        // The world is not drawn over the whole frame - it is drawn into the
        // Scene panel's body rect, with that rect's aspect. The gizmo is UI,
        // in surface pixels, so the editor's camera has to know the same
        // rectangle or the handles land next to the body they move (measured
        // at 92 px here before this pass existed; tests/test_editor_ui.cpp
        // holds the < 5 px check). The rect only exists once the dock has laid
        // itself out, so the layout frame comes first and the photographed
        // frame second - which is what examples/editor_demo.cpp does too.
        dai_ui_begin(ui, (float)W, (float)H, &in);
        dai_editor_ui_frame(panels, (float)W, (float)H);
        dai_ui_end(ui);
        {
            float lx = 0, ly = 0, lw = (float)W, lh = (float)H;
            dai_editor_ui_viewport_rect(panels, &lx, &ly, &lw, &lh);
            dai_editor_camera_viewport_rect(ed, lx, ly, lw, lh);
        }

        dai_ui_begin(ui, (float)W, (float)H, &in);
        dai_editor_ui_frame(panels, (float)W, (float)H);
        dai_ui_end(ui);

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
        // Mirror the real host: clip the world to the Scene panel's body rect.
        float vrx = 0, vry = 0, vrw = (float)W, vrh = (float)H;
        dai_editor_ui_viewport_rect(panels, &vrx, &vry, &vrw, &vrh);
        std::printf("   viewport rect: %.1f %.1f %.1fx%.1f (frame %dx%d)\n",
                    (double)vrx, (double)vry, (double)vrw, (double)vrh, W, H);
        dai_render_world_clip(r, vrx, vry, vrw, vrh);
        {   static float grid_xyz[84 * 2 * 3];
            uint32_t gn = dai_editor_ui_grid_lines(panels, grid_xyz, 84 * 2);
            dai_render_lines(r, grid_xyz, gn, 0.35f, 0.38f, 0.42f, 0.75f);
        }
        dai_render_frame(r, inst.data(), n);
        dai_render_write_png(r, path.c_str());
        std::printf("%-10s %u instances, %u ui verts -> %s\n", name, n,
                    (uint32_t)verts.size(), path.c_str());
    };
    auto shot = [&](const char *name, float mx, float my) {
        shot_to(name, outdir + "/editor_" + std::string(name) + ".png", mx, my);
    };

    // The Project panel gets the files a game project of this checkout really
    // has, so the bottom of the picture shows a browser with content instead
    // of the empty "nothing mounted" placeholder.
    {
        // Leaf names, not paths: the browser turns a path into a folder to
        // open, and a photograph of a closed folder shows nothing. These are
        // the files of assets/test in this checkout.
        static const char *const ASSETS[] = {
            "blender_scene.glb", "parented.gltf", "skinned.glb",
            "Grid.png",          "ui.png",        "bindings.cfg"
        };
        dai_editor_ui_asset_list(panels, ASSETS, (uint32_t)(sizeof(ASSETS) / sizeof(ASSETS[0])));
    }

    shot("edit", 640, 360);

    // hover the gizmo so the highlight shows
    dai_vec3 c = dai_editor_selection_center(ed);
    float len = dai_editor_gizmo_scale(ed);
    float hx = 0, hy = 0;
    dai_editor_project(ed, dai_vec3{ c.x + len * 0.7f, c.y, c.z }, &hx, &hy);
    dai_editor_gizmo_hover(ed, hx, hy);
    shot("hover", hx, hy);

    dai_editor_gizmo_mode(ed, DAI_GIZMO_ROTATE);
    shot("rotate", 640, 360);
    dai_editor_gizmo_mode(ed, DAI_GIZMO_TRANSLATE);

    dai_editor_play(ed);
    float alpha = 0.0f;
    for (int i = 0; i < 45; ++i) dai_editor_advance(ed, 1.0 / 60.0, &alpha);
    dai_editor_pause(ed);
    shot("playing", 640, 360);

    // Camera bindings, so the shots also prove the viewport camera moves.
    dai_editor_stop(ed);
    dai_editor_cam_input ci{};
    ci.dt = 1.0f / 60.0f;
    ci.mouse_x = 640; ci.mouse_y = 360; ci.mouse_right = 1;
    dai_editor_cam_update(ed, &ci);
    for (int i = 0; i < 22; ++i) { ci.key_w = 1; dai_editor_cam_update(ed, &ci); }
    ci.mouse_x = 760;                       // look right while flying
    dai_editor_cam_update(ed, &ci);
    {
        dai_vec3 o, d;
        dai_editor_ray(ed, (float)W * 0.5f, (float)H * 0.5f, &o, &d);
        dai_render_camera(r, o, dai_vec3{ o.x + d.x, o.y + d.y, o.z + d.z },
                          dai_vec3{ 0, 1, 0 }, 55.0f, 0.1f, 200.0f);
    }
    shot("flythrough", 640, 360);

    dai_editor_select(ed, target, 0);
    ci.mouse_right = 0; ci.key_w = 0;
    dai_editor_cam_update(ed, &ci);
    ci.key_focus = 1;
    dai_editor_cam_update(ed, &ci);         // F
    {
        dai_vec3 o, d;
        dai_editor_ray(ed, (float)W * 0.5f, (float)H * 0.5f, &o, &d);
        dai_render_camera(r, o, dai_vec3{ o.x + d.x, o.y + d.y, o.z + d.z },
                          dai_vec3{ 0, 1, 0 }, 55.0f, 0.1f, 200.0f);
    }
    shot("focus", 640, 360);

    // The picture the gauntlet collects: the SAME binary in game mode, so the
    // drone show set next to it is visibly one editor with two panel sets and
    // not two programs. It is written from here rather than by hand, because a
    // screenshot no tool produces is a screenshot nobody can reproduce.
    if (!gauntlet.empty()) {
        dai_editor_camera(ed, eye, look, up, 55.0f, 0.1f, 200.0f, (float)W, (float)H);
        dai_render_camera(r, eye, look, up, 55.0f, 0.1f, 200.0f);
        dai_editor_select(ed, target, 0);
        dai_editor_gizmo_mode(ed, DAI_GIZMO_TRANSLATE);
        dai_editor_gizmo_hover(ed, -1000.0f, -1000.0f);
        shot_to("game-mode", gauntlet + "/09-game-mode-editor.png", (float)W * 0.5f, (float)H * 0.5f);
    }

    dai_editor_ui_destroy(panels);
    dai_editor_destroy(ed);
    dai_ui_destroy(ui);
    if (icons) dai_icons_free(icons);
    if (font) dai_font_free(font);
    dai_render_destroy(r);
    dai_doc_sync_destroy(sync);
    dai_doc_destroy(doc);
    dai_scene_destroy(sc);
    dai_destroy(w);
    return 0;
}
