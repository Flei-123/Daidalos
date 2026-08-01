// The first thing Daidalos ever runs on real Windows hardware.
//
// Everything up to now was cross compiled and asserted to work: the Win32
// window backend compiled, the Vulkan calls looked right, the physics linked.
// None of that is evidence. This opens a window on the machine's actual GPU,
// steps the physics, draws frames, reads one back and writes it out as a PPM so
// the result can be looked at rather than believed.
//
//   win_smoke.exe [frames] [out.ppm]
//
// It prints what it found - device name, window size, frame time, how much of
// the frame is lit - and returns non zero if anything failed, so it can be run
// from a script without reading the output.

#include "dai_render.h"
#include "daidalos.h"
#include "dai_scene.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

static int fail(const char *what, const char *err) {
    std::printf("FAIL %s: %s\n", what, err && err[0] ? err : "(no detail)");
    return 1;
}

int main(int argc, char **argv) {
    const int frames = argc > 1 ? std::atoi(argv[1]) : 120;
    const char *out_path = argc > 2 ? argv[2] : "win_smoke.ppm";

    // Unbuffered, because the interesting runs are the ones that crash and a
    // buffered line is a line that never reaches the log.
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("daidalos win_smoke\n");

    char err[256] = { 0 };
    dai_render_desc rd{};
    rd.width = 1280;
    rd.height = 720;
    rd.msaa = 4;
    dai_renderer *r = dai_render_create(&rd, err, sizeof(err));
    if (!r) return fail("renderer", err);
    std::printf("  device: %s\n", dai_render_device_name(r));

    dai_window *w = dai_window_open(r, "Daidalos - smoke test", 1280, 720, err, sizeof(err));
    if (!w) { dai_render_destroy(r); return fail("window", err); }
    uint32_t ww = 0, wh = 0;
    dai_window_size(w, &ww, &wh);
    std::printf("  window: %ux%u\n", ww, wh);

    // Physics too, not just pixels: a stack of boxes falling onto a floor is
    // the cheapest thing that proves Jolt is actually stepping over here.
    std::printf("  step: creating world\n");
    dai_config cfg{};
    dai_world *world = nullptr;
    if (dai_create(&cfg, &world) != DAI_OK) {
        dai_window_close(w); dai_render_destroy(r);
        return fail("world", "dai_create");
    }
    std::printf("  step: creating scene\n");
    dai_scene *scene = dai_scene_create(world);

    dai_entity_desc floor_d = dai_entity_desc_default();
    floor_d.body.shape = DAI_SHAPE_BOX;
    floor_d.body.motion = DAI_STATIC;
    floor_d.body.half_extent = { 20.0f, 0.5f, 20.0f };
    floor_d.body.position = { 0.0f, -0.5f, 0.0f };
    floor_d.color = { 0.30f, 0.34f, 0.28f };
    floor_d.render_flags = DAI_RI_CHECKER | DAI_RI_NO_SHADOW;
    dai_scene_spawn(scene, &floor_d);

    std::printf("  step: spawning bodies\n");
    for (int i = 0; i < 24; ++i) {
        dai_entity_desc d = dai_entity_desc_default();
        d.body.shape = (i % 3 == 0) ? DAI_SHAPE_SPHERE : DAI_SHAPE_BOX;
        d.body.motion = DAI_DYNAMIC;
        d.body.half_extent = { 0.5f, 0.5f, 0.5f };
        const float a = i * 0.5f;
        d.body.position = { std::cos(a) * 3.0f, 2.0f + i * 1.2f, std::sin(a) * 3.0f };
        d.color = { 0.35f + 0.02f * i, 0.55f, 0.85f - 0.02f * i };
        dai_scene_spawn(scene, &d);
    }

    std::printf("  step: first frame\n");
    std::vector<dai_render_instance> inst(64);
    int drawn = 0, failed = 0;
    double ms_total = 0;
    for (int f = 0; f < frames && dai_window_poll(w); ++f) {
        dai_step(world);
        uint32_t n = dai_scene_instances(scene, inst.data(), (uint32_t)inst.size(), 0.0f);

        const float a = 0.6f + f * 0.01f;
        dai_render_camera(r, dai_vec3{ std::cos(a) * 18.0f, 8.0f, std::sin(a) * 18.0f },
                          dai_vec3{ 0.0f, 2.0f, 0.0f }, dai_vec3{ 0.0f, 1.0f, 0.0f },
                          55.0f, 0.1f, 200.0f);
        if (dai_render_frame(r, inst.data(), n) != DAI_OK) { ++failed; break; }
        if (dai_window_present(w) != DAI_OK) { ++failed; break; }
        ms_total += dai_render_last_ms(r);
        ++drawn;
    }
    std::printf("  frames: %d drawn, %d failed, %.2f ms average\n",
                drawn, failed, drawn ? ms_total / drawn : 0.0);

    // Read the last frame back and write it out, so the run leaves evidence
    // rather than a claim.
    std::printf("  step: readback\n");
    const uint32_t rw = dai_render_width(r), rh = dai_render_height(r);
    std::vector<uint8_t> px((size_t)rw * rh * 4);
    dai_render_readback(r, px.data(), px.size());
    size_t lit = 0;
    for (size_t i = 0; i < px.size(); i += 4)
        if (px[i] + px[i + 1] + px[i + 2] > 30) ++lit;
    const double cover = 100.0 * (double)lit / (double)(px.size() / 4);
    std::printf("  coverage: %.1f%% of the frame is lit\n", cover);

    FILE *o = std::fopen(out_path, "wb");
    if (o) {
        std::fprintf(o, "P6\n%u %u\n255\n", rw, rh);
        for (size_t i = 0; i < px.size(); i += 4) std::fwrite(&px[i], 1, 3, o);
        std::fclose(o);
        std::printf("  wrote: %s (%ux%u)\n", out_path, rw, rh);
    }

    dai_scene_destroy(scene);
    dai_destroy(world);
    dai_window_close(w);
    dai_render_destroy(r);

    // A window that opened and presented nothing is a failure, and so is a
    // frame that came back black - both would otherwise look like success.
    const bool ok = drawn > 0 && failed == 0 && cover > 1.0;
    std::printf("%s\n", ok ? "ok" : "FAILED");
    return ok ? 0 : 1;
}
