// The world lives inside the scene window, not behind the whole editor.
//
//   DAI_SHADER_DIR=shaders ./build/test_viewport
//
// Two halves of one promise:
//
//   1. dai_render_world_clip: the world pass writes ONLY inside the given
//      rectangle. Everything outside keeps the clear colour, and the UI pass
//      is untouched - a panel may cover the scene, the scene may not cover a
//      panel.
//
//   2. dai_editor_camera_viewport_rect: picking and projection speak surface
//      pixels, so a click at (window.x + 10, window.y + 10) must be the ray
//      through the viewport's (10, 10), and dai_editor_project must answer
//      with surface pixels again.

#include "dai_render.h"
#include "dai_editor.h"
#include "dai_editor_ui.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

static int g_fail = 0, g_pass = 0;
#define CHECK(cond, ...) do { \
    if (cond) { ++g_pass; } \
    else { ++g_fail; std::printf("  FAIL "); std::printf(__VA_ARGS__); std::printf("\n"); } \
} while (0)

int main() {
    const uint32_t W = 320, H = 200;
    char err[256] = { 0 };
    dai_render_desc rd{};
    rd.width = W; rd.height = H; rd.msaa = 1;
    dai_renderer *r = dai_render_create(&rd, err, sizeof(err));
    if (!r) { std::printf("renderer unavailable: %s\n", err); return 77; }

    // Flat clear colour, one bright box dead centre. No sky: the sky is world
    // content too, and it must obey the same clip.
    dai_render_clear_color(r, 0.10f, 0.10f, 0.10f);
    dai_render_sky(r, 0);
    dai_render_ambient(r, dai_vec3{ 1, 1, 1 }, dai_vec3{ 1, 1, 1 }, 1.0f);
    dai_render_sun(r, dai_vec3{ 0, 1, 0 }, dai_vec3{ 1, 1, 1 }, 0.0f);
    dai_render_camera(r, dai_vec3{ 0, 1.5f, 6 }, dai_vec3{ 0, 0, 0 }, dai_vec3{ 0, 1, 0 },
                      55.0f, 0.1f, 100.0f);

    dai_render_instance box = dai_render_instance_default();
    box.position = { 0, 0, 0 };
    box.scale = { 2.0f, 2.0f, 2.0f };
    box.color = { 1.0f, 0.2f, 0.2f };
    box.roughness = 1.0f;

    // ---- 1a. no clip: the box covers the centre of the frame --------------
    dai_render_world_clip(r, 0, 0, 0, 0);
    dai_render_frame(r, &box, 1);
    std::vector<uint8_t> full((size_t)W * H * 4);
    if (dai_render_readback(r, full.data(), full.size()) != DAI_OK) {
        std::printf("readback failed\n");
        dai_render_destroy(r);
        return 1;
    }
    auto px = [&](const std::vector<uint8_t> &img, uint32_t x, uint32_t y) {
        size_t o = ((size_t)y * W + x) * 4;
        return (int)img[o] + (int)img[o + 1] + (int)img[o + 2];
    };
    CHECK(px(full, W / 2, H / 2) > 120, "the box is not in the middle of the frame");

    // ---- 1b. clipped to the left half: the right half stays clear ---------
    dai_render_world_clip(r, 0, 0, W / 2.0f, (float)H);
    dai_render_frame(r, &box, 1);
    std::vector<uint8_t> half((size_t)W * H * 4);
    dai_render_readback(r, half.data(), half.size());
    int clearish = 0;
    for (uint32_t y = 4; y < H - 4; y += 4)
        for (uint32_t x = W / 2 + 8; x < W - 4; x += 4) {
            int s = px(half, x, y);
            if (std::abs(s - 77) < 24) ++clearish;    // 3 * 0.1 * 255 ~ 77
        }
    int right_total = (int)((H - 8) / 4) * (int)((W / 2 - 12) / 4);
    CHECK(clearish > right_total * 9 / 10,
          "the world spilled outside its clip (%d/%d pixels outside are clear)",
          clearish, right_total);
    // The clip moved the CENTRE of the projection into the left half: with
    // the aspect now half the width, the box sits in the middle of the LEFT
    // half, not in the middle of the frame.
    CHECK(px(half, W / 4, H / 2) > 120,
          "the box is not centred in the clipped viewport - the aspect did not follow");
    dai_render_world_clip(r, 0, 0, 0, 0);

    // ---- 2. the editor's camera knows where the viewport is ---------------
    dai_config cfg{};
    cfg.tick_hz = 60; cfg.max_bodies = 64; cfg.physics_threads = 1; cfg.seed = 2;
    dai_world *w = nullptr;
    if (dai_create(&cfg, &w) != DAI_OK) { std::printf("world failed\n"); return 1; }
    dai_scene *sc = dai_scene_create(w);
    dai_doc *doc = dai_doc_create();
    dai_doc_sync *sync = dai_doc_sync_create(doc, sc);
    dai_editor *ed = dai_editor_create(doc, sync);

    dai_editor_camera(ed, dai_vec3{ 0, 0, 10 }, dai_vec3{ 0, 0, 0 }, dai_vec3{ 0, 1, 0 },
                      60.0f, 0.1f, 100.0f, 800.0f, 600.0f);
    // The scene window sits at (300, 100) and is 800x600.
    dai_editor_camera_viewport_rect(ed, 300.0f, 100.0f, 800.0f, 600.0f);

    // The viewport's centre must be the camera axis: the world origin
    // projects to (300+400, 100+300), not to (400, 300).
    float sx = 0, sy = 0;
    CHECK(dai_editor_project(ed, dai_vec3{ 0, 0, 0 }, &sx, &sy) == 1,
          "the origin is behind the camera?");
    CHECK(std::fabs(sx - 700.0f) < 1.0f && std::fabs(sy - 400.0f) < 1.0f,
          "the origin projects to (%.1f, %.1f), expected the viewport centre (700, 400) - "
          "the offset is not applied", sx, sy);

    // And a ray through the viewport centre looks straight at it.
    dai_vec3 o{}, d{};
    dai_editor_ray(ed, 700.0f, 400.0f, &o, &d);
    CHECK(std::fabs(d.x) < 1e-4f && std::fabs(d.y) < 1e-4f && d.z < -0.99f,
          "the ray through the viewport centre is (%.3f %.3f %.3f), expected straight ahead",
          d.x, d.y, d.z);

    dai_editor_destroy(ed);
    dai_doc_sync_destroy(sync);
    dai_doc_destroy(doc);
    dai_scene_destroy(sc);
    dai_destroy(w);
    dai_render_destroy(r);

    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
