// The WebAssembly surface of the engine.
//
// A browser cannot pass structs by value or read C pointers, so the web API is
// deliberately flat: primitives in, one shared Float32Array out. Everything
// here is a thin wrapper - no logic lives in this file, so the web build cannot
// drift away from the native one.
//
// Transforms are written into a buffer the JS side reads directly (no copy per
// body): 8 floats per body - body handle, user data, position xyz, rotation
// xyzw is 9, so the layout is documented in dai_web_transforms below.

#include "daidalos.h"
#include "dai_input.h"

#include <emscripten/emscripten.h>
#include <cstring>
#include <vector>

namespace {
std::vector<dai_transform> g_scratch;
std::vector<float> g_out;
}

extern "C" {

EMSCRIPTEN_KEEPALIVE dai_world *dai_web_create(int backend, int tick_hz, int max_bodies, double seed) {
    dai_config cfg{};
    cfg.backend = backend;
    cfg.tick_hz = (uint32_t)(tick_hz > 0 ? tick_hz : 60);
    cfg.max_bodies = (uint32_t)(max_bodies > 0 ? max_bodies : 1024);
    cfg.physics_threads = 1;                       // wasm: one thread unless pthreads are on
    cfg.seed = (uint64_t)seed;
    dai_world *w = nullptr;
    return dai_create(&cfg, &w) == DAI_OK ? w : nullptr;
}

EMSCRIPTEN_KEEPALIVE void dai_web_destroy(dai_world *w) { dai_destroy(w); }

EMSCRIPTEN_KEEPALIVE uint32_t dai_web_body(dai_world *w, int shape, int motion,
                                           float hx, float hy, float hz,
                                           float px, float py, float pz,
                                           float density, uint32_t user_data) {
    dai_body_desc d{};
    d.shape = shape; d.motion = motion;
    d.half_extent = { hx, hy, hz };
    d.position = { px, py, pz };
    d.rotation = { 0, 0, 0, 1 };
    d.density = density;
    d.user_data = user_data;
    return dai_body_create(w, &d);
}

EMSCRIPTEN_KEEPALIVE int dai_web_impulse(dai_world *w, uint32_t body, float x, float y, float z) {
    return (int)dai_body_add_impulse(w, body, dai_vec3{ x, y, z });
}

EMSCRIPTEN_KEEPALIVE void dai_web_step(dai_world *w, int ticks) {
    for (int i = 0; i < ticks; ++i) dai_step(w);
}

EMSCRIPTEN_KEEPALIVE double dai_web_tick(dai_world *w) { return (double)dai_current_tick(w); }

// Checksums are 64 bit and JS numbers are not, so it comes back as a hex
// string - lying about precision here would defeat the whole point.
EMSCRIPTEN_KEEPALIVE const char *dai_web_checksum(dai_world *w) {
    static char buf[24];
    unsigned long long c = (unsigned long long)dai_checksum(w);
    std::snprintf(buf, sizeof(buf), "%016llx", c);
    return buf;
}

/* Fills a flat float array and returns how many bodies were written.
 * Layout, 9 floats per body:
 *   0: handle (as float; handles fit in 24 bits)
 *   1: user_data
 *   2,3,4: position x y z
 *   5,6,7,8: rotation x y z w
 * Call dai_web_transform_ptr() for the pointer to pass to Module.HEAPF32. */
EMSCRIPTEN_KEEPALIVE uint32_t dai_web_transforms(dai_world *w, float alpha) {
    uint32_t cap = 4096;
    if (g_scratch.size() < cap) g_scratch.resize(cap);
    uint32_t n = dai_get_transforms(w, g_scratch.data(), cap, alpha);
    g_out.resize((size_t)n * 9);
    for (uint32_t i = 0; i < n; ++i) {
        float *o = &g_out[(size_t)i * 9];
        const dai_transform &t = g_scratch[i];
        o[0] = (float)t.body;
        o[1] = (float)t.user_data;
        o[2] = t.position.x; o[3] = t.position.y; o[4] = t.position.z;
        o[5] = t.rotation.x; o[6] = t.rotation.y; o[7] = t.rotation.z; o[8] = t.rotation.w;
    }
    return n;
}

EMSCRIPTEN_KEEPALIVE float *dai_web_transform_ptr(void) { return g_out.empty() ? nullptr : g_out.data(); }

EMSCRIPTEN_KEEPALIVE void dai_web_input(dai_world *w, int player, double tick,
                                        float ax0, float ax1, uint32_t buttons) {
    dai_input in{};
    in.axis[0] = ax0; in.axis[1] = ax1;
    in.buttons = buttons;
    dai_set_input(w, (uint32_t)player, (dai_tick)tick, &in);
}

EMSCRIPTEN_KEEPALIVE int dai_web_rollback(dai_world *w, double tick) {
    return (int)dai_rollback_to(w, (dai_tick)tick);
}

EMSCRIPTEN_KEEPALIVE uint32_t dai_web_body_count(dai_world *w) {
    dai_stats s{};
    dai_get_stats(w, &s);
    return s.bodies;
}

EMSCRIPTEN_KEEPALIVE const char *dai_web_version(void) { return dai_version(); }

} // extern "C"
