// Live window: simulation + renderer + presentation, the loop a game actually
// has. WASD orbits, Space drops a crate, Escape quits.
//
//   DAI_SHADER_DIR=shaders ./build/window_demo
//
// Headless machines can still run it under Xvfb:
//   Xvfb :77 -screen 0 1280x720x24 & DISPLAY=:77 ./build/window_demo

#include "daidalos.h"
#include "dai_scene.h"
#include "dai_render.h"
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <vector>

int main(int argc, char **argv) {
    int max_frames = argc > 1 ? std::atoi(argv[1]) : 0;      // 0 = until closed

    dai_config cfg{};
    cfg.tick_hz = 60; cfg.max_bodies = 2048; cfg.physics_threads = 3; cfg.seed = 1234;
    dai_world *w = nullptr;
    if (dai_create(&cfg, &w) != DAI_OK) return 1;
    dai_scene *sc = dai_scene_create(w);

    dai_entity_desc g = dai_entity_desc_default();
    g.body.shape = DAI_SHAPE_BOX; g.body.motion = DAI_STATIC;
    g.body.half_extent = { 40, 1, 40 }; g.body.position = { 0, -1, 0 };
    g.color = { 0.30f, 0.34f, 0.27f };
    g.render_flags = DAI_RI_CHECKER | DAI_RI_NO_SHADOW;
    dai_scene_spawn(sc, &g);

    for (int i = 0; i < 40; ++i) {
        dai_entity_desc d = dai_entity_desc_default();
        d.body.shape = (i % 3 == 1) ? DAI_SHAPE_SPHERE : DAI_SHAPE_BOX;
        d.body.motion = DAI_DYNAMIC;
        d.body.half_extent = { 0.5f, 0.5f, 0.5f };
        d.body.position = { -4.0f + (i % 5) * 2.0f, 2.0f + (i / 5) * 1.5f, -3.0f + (i / 5) * 1.2f };
        d.body.restitution = 0.3f;
        dai_scene_spawn(sc, &d);
    }

    char err[256] = {0};
    dai_render_desc rd{}; rd.width = 1280; rd.height = 720; rd.msaa = 4; rd.shadow_size = 2048;
    dai_renderer *r = dai_render_create(&rd, err, sizeof(err));
    if (!r) { std::printf("no renderer: %s\n", err); return 2; }
    dai_window *win = dai_window_open(r, "Daidalos", 1280, 720, err, sizeof(err));
    if (!win) { std::printf("no window: %s\n", err); return 3; }

    dai_render_sun(r, dai_vec3{ 0.42f, 0.80f, 0.42f }, dai_vec3{ 1.0f, 0.95f, 0.86f }, 1.3f);
    dai_render_ambient(r, dai_vec3{ 0.24f, 0.40f, 0.72f }, dai_vec3{ 0.26f, 0.24f, 0.20f }, 0.38f);
    dai_render_fog(r, 0.0035f, dai_vec3{ 0.56f, 0.64f, 0.74f });
    dai_render_exposure(r, 0.58f);
    dai_render_shadow_extent(r, 25.0f);

    dai_camera cam = dai_camera_default();
    cam.distance = 18.0f; cam.pitch = 0.32f;

    std::vector<dai_render_instance> inst(4096);
    int frames = 0;
    while (dai_window_poll(win)) {
        if (dai_window_key_down(win, DAI_KEY_A)) cam.yaw -= 0.03f;
        if (dai_window_key_down(win, DAI_KEY_D)) cam.yaw += 0.03f;
        if (dai_window_key_down(win, DAI_KEY_W)) cam.distance = fmaxf(4.0f, cam.distance - 0.3f);
        if (dai_window_key_down(win, DAI_KEY_S)) cam.distance += 0.3f;
        if (dai_window_key_down(win, DAI_KEY_SPACE) && (frames % 12) == 0) {
            dai_entity_desc d = dai_entity_desc_default();
            d.body.shape = DAI_SHAPE_BOX; d.body.motion = DAI_DYNAMIC;
            d.body.half_extent = { 0.5f, 0.5f, 0.5f };
            d.body.position = { dai_random_float(w) * 6.0f - 3.0f, 12.0f, dai_random_float(w) * 6.0f - 3.0f };
            dai_scene_spawn(sc, &d);
        }
        dai_step(w);

        dai_render_camera(r, dai_camera_eye(&cam), cam.target, dai_vec3{ 0,1,0 }, cam.fov, cam.znear, cam.zfar);
        uint32_t n = dai_scene_instances(sc, inst.data(), (uint32_t)inst.size(), 0.0f);
        dai_render_frame(r, inst.data(), n);
        dai_window_present(win);

        if (++frames % 60 == 0)
            std::printf("frame %d | %u instances | %.1f ms\n", frames, n, dai_render_last_ms(r));
        if (max_frames && frames >= max_frames) break;
    }

    dai_window_close(win);
    dai_render_destroy(r);
    dai_scene_destroy(sc);
    dai_destroy(w);
    return 0;
}
