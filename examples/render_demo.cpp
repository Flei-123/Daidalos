// Daidalos: simulate, then draw the result with the Vulkan backend and write
// the frames out. Runs headless - on this machine through Mesa lavapipe, on a
// real GPU through the exact same code path.
//
//   ./build/render_demo [frames] [outdir]

#include "daidalos.h"
#include "dai_render.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <vector>

struct Game { dai_body player = DAI_INVALID_BODY; };

static void on_tick(dai_world *w, dai_tick t, void *user) {
    Game *g = (Game *)user;
    dai_input in{};
    dai_get_input(w, 0, t, &in);
    if (in.axis[0] != 0.0f)
        dai_body_add_impulse(w, g->player, dai_vec3{ in.axis[0] * 30.0f, 0, 0 });
}

int main(int argc, char **argv) {
    int frames = argc > 1 ? std::atoi(argv[1]) : 4;
    const char *outdir = argc > 2 ? argv[2] : ".";

    dai_config cfg{};
    cfg.tick_hz = 60; cfg.max_bodies = 512; cfg.physics_threads = 3; cfg.seed = 5;
    dai_world *w = nullptr;
    if (dai_create(&cfg, &w) != DAI_OK) { std::printf("dai_create failed\n"); return 1; }

    // ground
    dai_body_desc f{};
    f.shape = DAI_SHAPE_BOX; f.motion = DAI_STATIC;
    f.half_extent = { 30, 0.5f, 30 }; f.position = { 0, -0.5f, 0 }; f.rotation = { 0,0,0,1 };
    dai_body_create(w, &f);

    // a tower plus a wall of crates, so there is something to look at
    for (int i = 0; i < 8; ++i) {
        dai_body_desc d{};
        d.shape = DAI_SHAPE_BOX; d.motion = DAI_DYNAMIC;
        d.half_extent = { 0.5f, 0.5f, 0.5f };
        d.position = { 0, 0.5f + i * 1.02f, 0 }; d.rotation = { 0,0,0,1 };
        d.user_data = 1;
        dai_body_create(w, &d);
    }
    for (int x = 0; x < 6; ++x) for (int y = 0; y < 4; ++y) {
        dai_body_desc d{};
        d.shape = DAI_SHAPE_BOX; d.motion = DAI_DYNAMIC;
        d.half_extent = { 0.45f, 0.45f, 0.45f };
        d.position = { -6.0f + x * 0.95f, 0.45f + y * 0.92f, -4.0f };
        d.rotation = { 0,0,0,1 }; d.user_data = 2;
        dai_body_create(w, &d);
    }
    Game g;
    dai_body_desc p{};
    p.shape = DAI_SHAPE_SPHERE; p.motion = DAI_DYNAMIC;
    p.half_extent = { 0.8f, 0, 0 }; p.position = { 7, 4, 3 }; p.rotation = { 0,0,0,1 };
    p.no_sleeping = 1; p.density = 4000.0f; p.user_data = 3;
    g.player = dai_body_create(w, &p);
    dai_set_tick_callback(w, on_tick, &g);

    // push the sphere towards the tower
    for (dai_tick t = 0; t < 400; ++t) {
        dai_input in{}; if (t > 30 && t < 60) in.axis[0] = -1.0f;
        dai_set_input(w, 0, t, &in);
    }

    char err[256] = {0};
    dai_render_desc rd{}; rd.width = 960; rd.height = 540;
    dai_renderer *r = dai_render_create(&rd, err, sizeof(err));
    if (!r) { std::printf("renderer failed: %s\n", err); dai_destroy(w); return 2; }
    std::printf("GPU: %s\n", dai_render_device_name(r));
    dai_render_clear_color(r, 0.06f, 0.07f, 0.09f);
    dai_render_light(r, dai_vec3{ 0.45f, 0.8f, 0.35f });

    std::vector<dai_transform> tr(1024);
    std::vector<dai_render_instance> inst(1024);

    for (int fi = 0; fi < frames; ++fi) {
        for (int i = 0; i < 40; ++i) dai_step(w);

        uint32_t n = dai_get_transforms(w, tr.data(), (uint32_t)tr.size(), 0.0f);
        for (uint32_t i = 0; i < n; ++i) {
            inst[i].position = tr[i].position;
            inst[i].rotation = tr[i].rotation;
            // half extents are not part of dai_transform, so the demo derives
            // them from user_data - a real game keeps its own render component
            switch (tr[i].user_data) {
            case 1:  inst[i].half_extent = { 0.5f, 0.5f, 0.5f };  inst[i].color = { 0.85f, 0.45f, 0.25f }; break;
            case 2:  inst[i].half_extent = { 0.45f, 0.45f, 0.45f };inst[i].color = { 0.30f, 0.55f, 0.85f }; break;
            case 3:  inst[i].half_extent = { 0.8f, 0.8f, 0.8f };   inst[i].color = { 0.95f, 0.85f, 0.30f }; break;
            default: inst[i].half_extent = { 30.0f, 0.5f, 30.0f }; inst[i].color = { 0.20f, 0.22f, 0.24f }; break;
            }
        }
        dai_render_camera(r, dai_vec3{ 12, 8, 14 }, dai_vec3{ 0, 2, -1 }, dai_vec3{ 0, 1, 0 }, 50.0f, 0.1f, 400.0f);
        if (dai_render_frame(r, inst.data(), n) != DAI_OK) { std::printf("render failed\n"); break; }

        char path[512];
        std::snprintf(path, sizeof(path), "%s/frame_%02d.ppm", outdir, fi);
        dai_render_write_ppm(r, path);

        // cheap sanity check: how much of the frame is not the clear colour
        std::vector<uint8_t> px((size_t)dai_render_width(r) * dai_render_height(r) * 4);
        dai_render_readback(r, px.data(), px.size());
        size_t lit = 0;
        for (size_t i = 0; i < px.size(); i += 4)
            if (px[i] > 40 || px[i+1] > 40 || px[i+2] > 40) lit++;
        std::printf("Frame %d: Tick %llu, %u Bodies, %.1f ms, %.1f%% des Bildes bemalt -> %s\n",
            fi, (unsigned long long)dai_current_tick(w), n, dai_render_last_ms(r),
            100.0 * (double)lit / (double)(px.size() / 4), path);
    }

    dai_render_destroy(r);
    dai_destroy(w);
    return 0;
}
