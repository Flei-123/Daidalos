// Renders the drone show panels - storyboard, parameters, validation, the
// preview with its timeline and the status line - to PNGs, so the show mode
// can be LOOKED at rather than only asserted about. The sibling of
// tools/editor_shot.cpp, and it exists for the same reason: a panel that is
// green in a test and unreadable on screen is still broken.
//
//   DAI_SHADER_DIR=shaders ./build/droneshow_shot .gauntlet-shots [W] [H]
//
// The show it builds is a real one: three figures sampled off triangle soup
// through the ordinary pipeline, solved, validated - plus one figure with a
// pair of drones deliberately placed inside the minimum distance, because a
// validation panel photographed with an empty list proves nothing about the
// panel that has to show a conflict.

#include "dai_editor_ui.h"
#include "dai_render.h"
#include "dai_show.h"
#include "dai_show_ui.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// The same triangle soup a glTF import hands over - generated here so the tool
// needs no asset on disk.
struct Soup {
    std::vector<float>    pos;
    std::vector<uint32_t> idx;
    void tri(const float *a, const float *b, const float *c) {
        const float *v[3] = { a, b, c };
        for (int i = 0; i < 3; ++i) {
            idx.push_back((uint32_t)(pos.size() / 3));
            pos.push_back(v[i][0]); pos.push_back(v[i][1]); pos.push_back(v[i][2]);
        }
    }
};

static void figure_sphere(Soup &s, int rings, int segs) {
    for (int i = 0; i < rings; ++i) {
        float p0 = (float)M_PI * (float)i / (float)rings;
        float p1 = (float)M_PI * (float)(i + 1) / (float)rings;
        for (int j = 0; j < segs; ++j) {
            float t0 = 2.0f * (float)M_PI * (float)j / (float)segs;
            float t1 = 2.0f * (float)M_PI * (float)(j + 1) / (float)segs;
            float a[3] = { std::sin(p0) * std::cos(t0), std::cos(p0), std::sin(p0) * std::sin(t0) };
            float b[3] = { std::sin(p1) * std::cos(t0), std::cos(p1), std::sin(p1) * std::sin(t0) };
            float c[3] = { std::sin(p1) * std::cos(t1), std::cos(p1), std::sin(p1) * std::sin(t1) };
            float d[3] = { std::sin(p0) * std::cos(t1), std::cos(p0), std::sin(p0) * std::sin(t1) };
            s.tri(a, b, c); s.tri(a, c, d);
        }
    }
}

static void figure_cube(Soup &s) {
    const float h = 1.0f;
    const float v[8][3] = {
        { -h, -h, -h }, {  h, -h, -h }, {  h,  h, -h }, { -h,  h, -h },
        { -h, -h,  h }, {  h, -h,  h }, {  h,  h,  h }, { -h,  h,  h }
    };
    const int f[12][3] = {
        {0,2,1},{0,3,2},{4,5,6},{4,6,7},{0,1,5},{0,5,4},
        {3,7,6},{3,6,2},{0,4,7},{0,7,3},{1,2,6},{1,6,5}
    };
    for (int i = 0; i < 12; ++i) s.tri(v[f[i][0]], v[f[i][1]], v[f[i][2]]);
}

static void figure_ring(Soup &s, int segs, float thick) {
    for (int j = 0; j < segs; ++j) {
        float t0 = 2.0f * (float)M_PI * (float)j / (float)segs;
        float t1 = 2.0f * (float)M_PI * (float)(j + 1) / (float)segs;
        for (int k = 0; k < 12; ++k) {
            float u0 = 2.0f * (float)M_PI * (float)k / 12.0f;
            float u1 = 2.0f * (float)M_PI * (float)(k + 1) / 12.0f;
            auto pt = [&](float t, float u, float *o) {
                float rr = 1.0f + thick * std::cos(u);
                o[0] = rr * std::cos(t); o[1] = thick * std::sin(u); o[2] = rr * std::sin(t);
            };
            float a[3], b[3], c[3], d[3];
            pt(t0, u0, a); pt(t1, u0, b); pt(t1, u1, c); pt(t0, u1, d);
            s.tri(a, b, c); s.tri(a, c, d);
        }
    }
}

int main(int argc, char **argv) {
    std::string outdir = argc > 1 ? argv[1] : ".gauntlet-shots";
    const uint32_t W = argc > 2 ? (uint32_t)atoi(argv[2]) : 1600;
    const uint32_t H = argc > 3 ? (uint32_t)atoi(argv[3]) : 900;
    // A tag in the file names, so the same tool can photograph the same show at
    // a second window size without overwriting the first run. Narrow is where
    // labels collide, so the narrow run is the one worth keeping.
    std::string tag = argc > 4 ? argv[4] : "";

    // ---- the show ---------------------------------------------------------
    dai_show_settings s = dai_show_settings_default();
    s.drone_count    = 420;
    s.min_distance_m = 2.0f;
    s.v_max_ms       = 8.0f;
    s.a_max_ms2      = 4.0f;
    s.takeoff_alt_m  = 30.0f;
    s.fps            = 10;                 // a shot tool, not a flight review
    s.seed           = 20260814ull;

    dai_show *sh = dai_show_create(&s);
    if (!sh) { std::printf("show failed\n"); return 1; }

    struct Fig { int kind; const char *name; float size; int mode; float hold; };
    const Fig figs[3] = {
        { 0, "Sphere",     70.0f, DAI_SHOW_SAMPLE_SURFACE,    4.0f },
        { 2, "Ring",       90.0f, DAI_SHOW_SAMPLE_SILHOUETTE, 4.0f },
        { 1, "Cube",       70.0f, DAI_SHOW_SAMPLE_SURFACE,    4.0f }
    };
    for (int i = 0; i < 3; ++i) {
        Soup soup;
        if (figs[i].kind == 0)      figure_sphere(soup, 24, 32);
        else if (figs[i].kind == 1) figure_cube(soup);
        else                        figure_ring(soup, 48, 0.28f);

        dai_show_sample_desc d;
        std::memset(&d, 0, sizeof(d));
        d.positions        = soup.pos.data();
        d.vertex_count     = (uint32_t)(soup.pos.size() / 3);
        d.indices          = soup.idx.data();
        d.index_count      = (uint32_t)soup.idx.size();
        d.base_rgba        = (i == 0) ? 0xFF40C0FFu : (i == 1) ? 0xFF60FF90u : 0xFFFF80D0u;
        d.mode             = figs[i].mode;
        d.count            = s.drone_count;
        d.min_distance_m   = s.min_distance_m;
        d.scale            = figs[i].size * 0.5f;
        d.centre           = dai_vec3{ 0.0f, s.takeoff_alt_m + figs[i].size * 0.6f, 0.0f };
        d.view_dir         = dai_vec3{ 0.0f, 0.0f, 1.0f };
        d.relax_iterations = 6;
        d.seed             = s.seed + (uint64_t)i * 7919ull;

        char err[256] = { 0 };
        uint32_t idx = dai_show_formation_from_mesh(sh, figs[i].name, "builtin://figure",
                                                    &d, err, sizeof(err));
        if (idx == 0xFFFFFFFFu) { std::printf("sample %s failed: %s\n", figs[i].name, err); return 1; }
        dai_show_formation_set_hold(sh, idx, figs[i].hold);
        std::printf("formation %u  %-8s %u points\n", idx, figs[i].name, s.drone_count);
    }

    // The figure that is WRONG on purpose: the sphere again, with two drones
    // pushed to 0.4 m apart. The validator has to find it, the list has to show
    // it and the preview has to paint it red - that is what these images are for.
    {
        const dai_show_point *base = dai_show_formation_points(sh, 0);
        std::vector<dai_show_point> pts(base, base + s.drone_count);
        pts[173] = pts[172];
        pts[173].x += 0.4f;
        uint32_t idx = dai_show_formation_add(sh, "Sphere (near miss)", "builtin://figure",
                                              pts.data(), (uint32_t)pts.size(), 5.0f);
        std::printf("formation %u  near miss injected between drones 172 and 173\n", idx);
    }

    char err[256] = { 0 };
    dai_result sr = dai_show_solve(sh, err, sizeof(err));
    dai_show_validate_show(sh);
    dai_show_timings tm = dai_show_get_timings(sh);
    std::printf("solve %s: assign %.1f ms, layer %.1f ms, validate %.1f ms, %u conflicts\n",
                sr == DAI_OK ? "ok" : err, tm.assign_ms, tm.layer_ms, tm.validate_ms,
                dai_show_conflict_count(sh));

    dai_show_ui *show = dai_show_ui_create(sh);

    // ---- the editor around it --------------------------------------------
    dai_config cfg{};
    cfg.tick_hz = 60; cfg.max_bodies = 64; cfg.physics_threads = 1;
    cfg.snapshot_ring = 16; cfg.seed = 9;
    dai_world *w = nullptr;
    if (dai_create(&cfg, &w) != DAI_OK) { std::printf("world failed\n"); return 1; }
    dai_scene *sc = dai_scene_create(w);
    dai_doc *doc = dai_doc_create();
    dai_doc_sync *sync = dai_doc_sync_create(doc, sc);
    dai_doc_sync_apply(sync);
    dai_step(w);

    dai_render_desc rd{};
    rd.width = W; rd.height = H; rd.msaa = 4;
    char rerr[256] = { 0 };
    dai_renderer *r = dai_render_create(&rd, rerr, sizeof(rerr));
    if (!r) { std::printf("renderer failed: %s\n", rerr); return 1; }

    dai_font *font = dai_font_load_ui(13.0f, rerr, sizeof(rerr));
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
    dai_icons *icons = dai_icons_create(16.0f);
    if (icons) {
        uint32_t iw = 0, ih = 0;
        const uint8_t *irgba = dai_icons_atlas_rgba(icons, &iw, &ih);
        if (irgba && iw && ih)
            dai_ui_set_icons(ui, icons, dai_render_texture_create(r, irgba, iw, ih, 0));
    }

    dai_vec3 eye{ 0.0f, 60.0f, 200.0f }, look{ 0, 60.0f, 0 }, up{ 0, 1, 0 };
    dai_render_camera(r, eye, look, up, 55.0f, 0.1f, 800.0f);
    dai_render_sun(r, dai_vec3{ 0.42f, 0.80f, 0.42f }, dai_vec3{ 1.0f, 0.95f, 0.86f }, 1.2f);
    dai_render_ambient(r, dai_vec3{ 0.10f, 0.14f, 0.24f }, dai_vec3{ 0.06f, 0.06f, 0.08f }, 0.35f);
    dai_render_exposure(r, 0.45f);
    dai_render_sky(r, 0);

    dai_editor *ed = dai_editor_create(doc, sync);
    dai_editor_camera(ed, eye, look, up, 55.0f, 0.1f, 800.0f, (float)W, (float)H);
    dai_editor_ui *panels = dai_editor_ui_create(ed, ui);
    dai_editor_ui_show_host(panels, show);

    // One frame of the whole editor. `mx/my/down` are the pointer, so a hover
    // or a click can be photographed as well as a resting screen.
    auto frame = [&](float mx, float my, int down) {
        dai_ui_input in{};
        in.mouse_x = mx; in.mouse_y = my; in.mouse_down = down;
        dai_ui_begin(ui, (float)W, (float)H, &in);
        dai_editor_ui_frame(panels, (float)W, (float)H);
        dai_ui_end(ui);
    };
    auto flush = [&](const char *path) {
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
        dai_render_world_clip(r, 0, 0, 0, 0);
        dai_render_frame(r, nullptr, 0);
        dai_render_write_png(r, path);
        std::printf("%-46s %u ui vertices\n", path, (uint32_t)verts.size());
    };
    auto shot = [&](const char *name, float mx, float my, int down) {
        // Twice: the dock registers its panels on the first frame and lays them
        // out on it, and immediate mode widgets settle their scroll state one
        // frame later. The picture wanted is the settled one.
        frame(mx, my, down);
        frame(mx, my, down);
        std::string path = outdir + "/" + name + tag + ".png";
        flush(path.c_str());
    };

    // 1  the show as it opens: every panel, the fleet at t = 0.
    dai_show_ui_seek(show, 0.0f);
    shot("01-start", (float)W * 0.5f, (float)H * 0.5f, 0);

    // 2  mid transition - the fleet in flight between two figures, which is
    //    where the colours and the layering are actually visible.
    dai_show_ui_seek(show, 10.5f);
    shot("02-transition", (float)W * 0.5f, (float)H * 0.5f, 0);

    // 3  the conflict. Seek to the moment the validator complained about and
    //    select the drones it named, exactly as clicking the row does.
    int conflict_row = -1;
    dai_show_conflict c{};
    if (dai_show_conflict_count(sh) > 0) {
        conflict_row = (int)(dai_show_conflict_count(sh) / 2);
        dai_show_conflict_at(sh, (uint32_t)conflict_row, &c);
        std::printf("conflict %d at %.2fs between drones %u and %u (%.2f m)\n",
                    conflict_row, c.time_s, c.a, c.b, c.value);
    }
    if (conflict_row >= 0) dai_show_ui_seek(show, c.time_s);
    shot("03-viewport-conflict", (float)W * 0.5f, (float)H * 0.5f, 0);

    // ---- the panels on their own -----------------------------------------
    // Same functions the dock calls, given the whole frame, so a reviewer can
    // read the rows instead of squinting at a docked column.
    auto panel_shot = [&](const char *name, void (*fn)(dai_show_ui *, dai_ui *, float, float, float, float)) {
        for (int pass = 0; pass < 2; ++pass) {
            dai_ui_input in{};
            in.mouse_x = -100; in.mouse_y = -100;
            dai_ui_begin(ui, (float)W, (float)H, &in);
            fn(show, ui, 20.0f, 20.0f, (float)W - 40.0f, (float)H - 100.0f);
            dai_show_ui_status(show, ui, 0.0f, (float)H - 30.0f, (float)W, 30.0f);
            dai_ui_end(ui);
        }
        std::string path = outdir + "/" + name + tag + ".png";
        flush(path.c_str());
    };
    panel_shot("04-storyboard", dai_show_ui_storyboard);
    panel_shot("05-parameters", dai_show_ui_parameters);
    panel_shot("06-validation", dai_show_ui_validation);
    panel_shot("07-preview-full", dai_show_ui_viewport);

    // 8  the click the validation panel promises: a row is pressed, the
    //    timeline jumps to it and the two drones go red in the preview. Done
    //    through the pointer, not by setting the fields, so the picture proves
    //    the interaction rather than the state.
    {
        // The dock puts Validation across the bottom quarter of the frame, and
        // the rows begin under its tab strip, the Re-check button and the red
        // header. Rather than hard coding that stack, the pointer walks down
        // the panel until a row actually takes the click - the interface's own
        // answer (dai_show_ui_selected_conflict) is the test that it landed.
        const float TOP = 34.0f, BOTTOM = 24.0f;
        float py = TOP + 0.74f * ((float)H - TOP - BOTTOM);
        int hit = 0;
        for (float dy = 60.0f; dy < 160.0f && !hit; dy += 6.0f) {
            float rx = 120.0f, ry = py + dy;
            frame(rx, ry, 0);          // hover
            frame(rx, ry, 1);          // press
            frame(rx, ry, 0);          // release: the row commits
            frame(rx, ry, 0);
            if (dai_show_ui_selected_conflict(show) >= 0) {
                hit = 1;
                std::string path = outdir + "/08-conflict-clicked" + tag + ".png";
                flush(path.c_str());
                std::printf("clicked validation row at %.0f,%.0f -> conflict %d, t=%.2f, drone %u\n",
                            rx, ry, dai_show_ui_selected_conflict(show),
                            dai_show_ui_time(show), dai_show_ui_selected_drone(show));
            }
        }
        if (!hit) std::printf("no validation row took the click\n");
    }

    dai_editor_ui_destroy(panels);
    dai_editor_destroy(ed);
    dai_show_ui_destroy(show);
    dai_show_destroy(sh);
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
