// Opens a real window, renders and presents frames into it.
//
// Runs under Xvfb in CI:  xvfb-run -s "-screen 0 1024x640x24" ./build/test_window
// Without a display it reports "skipped" rather than failing, so the normal
// headless test run stays clean.

#include "dai_render.h"
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <vector>

int main() {
    if (!std::getenv("DISPLAY") && !std::getenv("WAYLAND_DISPLAY")) { std::printf("no display server, skipped\n"); return 0; }

    char err[256] = {0};
    dai_render_desc rd{}; rd.width = 640; rd.height = 400; rd.msaa = 4;
    dai_renderer *r = dai_render_create(&rd, err, sizeof(err));
    if (!r) { std::printf("renderer unavailable: %s\n", err); return 77; }

    dai_window *w = dai_window_open(r, "Daidalos", 640, 400, err, sizeof(err));
    if (!w) { std::printf("FAIL window: %s\n", err); dai_render_destroy(r); return 1; }
    uint32_t ww = 0, wh = 0;
    dai_window_size(w, &ww, &wh);
    std::printf("window %ux%u on %s\n", ww, wh, dai_render_device_name(r));

    std::vector<dai_render_instance> inst;
    for (int i = 0; i < 24; ++i) {
        dai_render_instance in = dai_render_instance_default();
        float a = i * 0.26f;
        in.position = { cosf(a) * 4.0f, 1.0f + (i % 3), sinf(a) * 4.0f };
        in.mesh = (uint32_t)(i % 3 == 0 ? DAI_MESH_BOX : i % 3 == 1 ? DAI_MESH_SPHERE : DAI_MESH_CYLINDER);
        in.scale = { 0.6f, 0.6f, 0.6f };
        in.color = { 0.3f + 0.02f * i, 0.6f, 0.9f - 0.02f * i };
        inst.push_back(in);
    }
    dai_render_instance floor_i = dai_render_instance_default();
    floor_i.position = { 0, -1, 0 }; floor_i.scale = { 30, 1, 30 };
    floor_i.color = { 0.3f, 0.34f, 0.28f };
    floor_i.flags = DAI_RI_CHECKER | DAI_RI_NO_SHADOW;
    inst.push_back(floor_i);

    int presented = 0, failed = 0;
    for (int f = 0; f < 12 && dai_window_poll(w); ++f) {
        float a = 0.6f + f * 0.12f;
        dai_render_camera(r, dai_vec3{ cosf(a) * 12.0f, 6.0f, sinf(a) * 12.0f },
                          dai_vec3{ 0, 1, 0 }, dai_vec3{ 0, 1, 0 }, 55.0f, 0.1f, 200.0f);
        if (dai_render_frame(r, inst.data(), (uint32_t)inst.size()) != DAI_OK) { ++failed; break; }
        if (dai_window_present(w) != DAI_OK) { ++failed; break; }
        ++presented;
    }
    std::printf("presented %d frames, %d failures, last frame %.1f ms\n", presented, failed, dai_render_last_ms(r));

    // the frame that went to the screen must still be readable and non empty
    std::vector<uint8_t> px((size_t)dai_render_width(r) * dai_render_height(r) * 4);
    dai_render_readback(r, px.data(), px.size());
    size_t lit = 0;
    for (size_t i = 0; i < px.size(); i += 4) if (px[i] + px[i+1] + px[i+2] > 30) ++lit;
    double cover = 100.0 * (double)lit / (double)(px.size() / 4);
    std::printf("frame coverage %.1f%%\n", cover);

    dai_window_close(w);
    dai_render_destroy(r);
    int ok = (presented >= 10 && failed == 0 && cover > 50.0);
    std::printf("%s\n", ok ? "window test passed" : "WINDOW TEST FAILED");
    return ok ? 0 : 1;
}
