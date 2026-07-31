// Renders the editor viewport - scene plus gizmo overlay - to a PNG, so the
// gizmo can actually be looked at instead of only asserted about.
//
//   DAI_SHADER_DIR=shaders ./build/gizmo_shot /tmp/out
//
// Writes gizmo_translate.png, gizmo_rotate.png, gizmo_scale.png.

#include "dai_doc.h"
#include "dai_editor.h"
#include "dai_render.h"
#include "dai_ui.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static uint32_t pack(dai_vec3 c, float a = 1.0f) {
    auto b = [](float v) { return (uint32_t)(v < 0 ? 0 : (v > 1 ? 255 : v * 255.0f + 0.5f)); };
    return b(c.x) | (b(c.y) << 8) | (b(c.z) << 16) | (b(a) << 24);
}

int main(int argc, char **argv) {
    std::string outdir = argc > 1 ? argv[1] : "/tmp";

    dai_config cfg{};
    cfg.tick_hz = 60; cfg.max_bodies = 128; cfg.physics_threads = 1; cfg.seed = 5;
    dai_world *w = nullptr;
    if (dai_create(&cfg, &w) != DAI_OK) { std::printf("world failed\n"); return 1; }
    dai_scene *sc = dai_scene_create(w);
    dai_doc *doc = dai_doc_create();
    dai_doc_sync *sync = dai_doc_sync_create(doc, sc);

    // ground plus three boxes
    dai_node_desc g = dai_node_desc_default();
    snprintf(g.name, sizeof(g.name), "Ground");
    g.motion = DAI_STATIC;
    g.half_extent = { 9, 0.5f, 9 };
    g.position = { 0, -0.5f, 0 };
    g.color = { 0.20f, 0.22f, 0.19f };
    dai_doc_add(doc, &g);

    dai_node target = 0;
    for (int i = 0; i < 3; ++i) {
        dai_node_desc b = dai_node_desc_default();
        snprintf(b.name, sizeof(b.name), "box%d", i);
        b.motion = DAI_KINEMATIC;
        b.half_extent = { 0.75f, 0.75f, 0.75f };
        b.position = { -3.5f + (float)i * 3.5f, 0.75f, 0 };
        dai_node n = dai_doc_add(doc, &b);
        if (i == 1) target = n;
    }
    dai_doc_sync_apply(sync);
    dai_step(w);

    const uint32_t W = 960, H = 600;
    dai_render_desc rd{};
    rd.width = W; rd.height = H; rd.msaa = 4;
    char err[256] = { 0 };
    dai_renderer *r = dai_render_create(&rd, err, sizeof(err));
    if (!r) { std::printf("renderer failed: %s\n", err); return 1; }

    dai_font *font = dai_font_load("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", 18.0f,
                                   nullptr, 0, err, sizeof(err));
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

    dai_vec3 eye{ 4.6f, 3.4f, 7.6f }, look{ 0, 0.7f, 0 }, up{ 0, 1, 0 };
    dai_render_camera(r, eye, look, up, 55.0f, 0.1f, 200.0f);
    // dai_render_sun takes the direction *towards* the sun. Passing the
    // direction the light travels leaves every visible face in shadow and the
    // scene comes out flat - which is exactly what the first render looked like.
    dai_render_sun(r, dai_vec3{ 0.42f, 0.80f, 0.42f }, dai_vec3{ 1.0f, 0.95f, 0.86f }, 1.3f);
    dai_render_ambient(r, dai_vec3{ 0.24f, 0.40f, 0.72f }, dai_vec3{ 0.26f, 0.24f, 0.20f }, 0.38f);
    dai_render_exposure(r, 0.45f);
    dai_render_shadow_extent(r, 20.0f);
    dai_render_sky(r, 1);

    dai_editor *ed = dai_editor_create(doc, sync);
    dai_editor_camera(ed, eye, look, up, 55.0f, 0.1f, 200.0f, (float)W, (float)H);
    dai_editor_select(ed, target, 0);

    const char *names[3] = { "translate", "rotate", "scale" };
    const int modes[3] = { DAI_GIZMO_TRANSLATE, DAI_GIZMO_ROTATE, DAI_GIZMO_SCALE };

    for (int m = 0; m < 3; ++m) {
        dai_editor_gizmo_mode(ed, modes[m]);

        std::vector<dai_render_instance> inst(64);
        uint32_t n = dai_scene_instances(sc, inst.data(), (uint32_t)inst.size(), 1.0f);

        // gizmo -> screen space lines -> UI geometry
        uint32_t gl = dai_editor_gizmo_lines(ed, nullptr, 0);
        std::vector<dai_gizmo_line> lines(gl);
        dai_editor_gizmo_lines(ed, lines.data(), gl);

        dai_ui_input in{};
        dai_ui_begin(ui, (float)W, (float)H, &in);
        for (const dai_gizmo_line &l : lines) {
            float ax, ay, bx, by;
            if (!dai_editor_project(ed, l.a, &ax, &ay)) continue;
            if (!dai_editor_project(ed, l.b, &bx, &by)) continue;
            dai_ui_line(ui, ax, ay, bx, by, l.highlighted ? 4.0f : 2.5f, pack(l.color));
        }
        char label[64];
        snprintf(label, sizeof(label), "gizmo: %s", names[m]);
        dai_ui_panel_begin(ui, 16, 16, 200, 52, nullptr);
        dai_ui_label(ui, label);
        dai_ui_panel_end(ui);
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
        dai_render_frame(r, inst.data(), n);

        std::string path = outdir + "/gizmo_" + names[m] + ".png";
        dai_render_write_png(r, path.c_str());
        std::printf("%-10s %u instances, %u gizmo lines, %u ui verts -> %s\n",
                    names[m], n, gl, (uint32_t)verts.size(), path.c_str());
    }

    dai_editor_destroy(ed);
    dai_ui_destroy(ui);
    if (font) dai_font_free(font);
    dai_render_destroy(r);
    dai_doc_sync_destroy(sync);
    dai_doc_destroy(doc);
    dai_scene_destroy(sc);
    dai_destroy(w);
    return 0;
}
