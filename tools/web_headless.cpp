// The engine core running in WebAssembly.
//
// Simulates a fixed scene and prints the checksum. Built natively AND for the
// web from the same sources; if the two numbers match, the simulation is
// bit identical in a browser and on the server - which is the whole promise of
// "state(n+1) = step(state(n), input(n))" and the prerequisite for a web
// client that can share a rollback session with a native one.
//
//   ./build_web.sh && node build/web/daidalos_web.js

#include "daidalos.h"
#include <cstdio>

int main() {
    dai_config cfg{};
    cfg.backend = DAI_PHYSICS_NULL;      // the portable backend, no Jolt SIMD in play
    cfg.tick_hz = 60;
    cfg.max_bodies = 512;
    cfg.physics_threads = 1;
    cfg.seed = 20260731;
    dai_world *w = nullptr;
    if (dai_create(&cfg, &w) != DAI_OK) { std::printf("create failed\n"); return 1; }

    dai_body_desc f{};
    f.shape = DAI_SHAPE_BOX; f.motion = DAI_STATIC;
    f.half_extent = { 50, 1, 50 }; f.position = { 0, -1, 0 }; f.rotation = { 0,0,0,1 };
    dai_body_create(w, &f);

    for (int i = 0; i < 64; ++i) {
        dai_body_desc d{};
        d.shape = (i % 2) ? DAI_SHAPE_SPHERE : DAI_SHAPE_BOX;
        d.motion = DAI_DYNAMIC;
        d.half_extent = { 0.5f, 0.5f, 0.5f };
        d.position = { -4.0f + (i % 8) * 1.1f, 2.0f + (i / 8) * 1.3f, -3.0f + (i / 8) * 0.7f };
        d.rotation = { 0,0,0,1 };
        d.density = 500.0f + i;
        dai_body_create(w, &d);
    }

    for (int t = 0; t < 600; ++t) {
        dai_input in{};
        in.axis[0] = (float)((t % 120) - 60) / 60.0f;
        dai_set_input(w, 0, dai_current_tick(w), &in);
        dai_step(w);
    }

    dai_stats st{};
    dai_get_stats(w, &st);
    std::printf("%s\n", dai_version());
    std::printf("backend   : %s\n", dai_backend_name(w));
    std::printf("ticks     : %llu\n", (unsigned long long)st.ticks_simulated);
    std::printf("bodies    : %u\n", st.bodies);
    std::printf("CHECKSUM  : %016llx\n", (unsigned long long)dai_checksum(w));

    // rollback still has to work in the browser
    dai_tick target = dai_current_tick(w) - 30;
    uint64_t before = dai_checksum(w);
    dai_result rr = dai_rollback_to(w, target);
    std::printf("rollback  : %s, checksum %s\n",
                rr == DAI_OK ? "ok" : "failed",
                dai_checksum(w) == before ? "reproduced" : "DIVERGED");
    dai_destroy(w);
    return 0;
}
