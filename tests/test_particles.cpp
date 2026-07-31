// Particles: the simulation side is checked on the CPU, the drawing side is
// checked in pixels. Both matter - a particle system that looks right but
// spawns unboundedly will kill a game an hour into a session, and one that
// counts correctly but draws nothing is worse than none.
//
//   DAI_SHADER_DIR=shaders ./build/test_particles [outdir]

#include "dai_particles.h"
#include "dai_render.h"
#include <cstdio>
#include <cstring>
#include <cmath>
#include <cstdlib>
#include <vector>

static int g_fail = 0, g_pass = 0;
#define CHECK(cond, ...) do { \
    if (cond) { ++g_pass; } \
    else { ++g_fail; std::printf("  FAIL "); std::printf(__VA_ARGS__); std::printf("\n"); } \
} while (0)

static float mean_lum(const std::vector<uint8_t> &px) {
    double s = 0;
    for (size_t i = 0; i < px.size(); i += 4) s += (0.2126*px[i] + 0.7152*px[i+1] + 0.0722*px[i+2]) / 255.0;
    return (float)(s / (px.size() / 4));
}

int main(int argc, char **argv) {
    const char *outdir = argc > 1 ? argv[1] : "/tmp";
    std::printf("particles\n");

    // ---- 1. rate, lifetime, and a hard cap
    {
        dai_particles *p = dai_particles_create(4096);
        dai_emitter_desc d = dai_emitter_desc_default();
        d.rate = 100.0f; d.lifetime = 1.0f; d.lifetime_jitter = 0.0f;
        d.speed = 0.0f; d.gravity = 0.0f; d.drag = 0.0f;
        dai_emitter e = dai_particles_add(p, &d);
        CHECK(e != DAI_INVALID_EMITTER, "emitter creation failed");

        for (int i = 0; i < 30; ++i) dai_particles_update(p, 1.0f / 60.0f);   // 0.5 s
        uint32_t after_half = dai_particles_count(p);
        CHECK(after_half >= 45 && after_half <= 55, "0.5 s at 100/s gave %u particles, expected ~50", after_half);

        for (int i = 0; i < 120; ++i) dai_particles_update(p, 1.0f / 60.0f);  // 2 s more
        uint32_t steady = dai_particles_count(p);
        CHECK(steady >= 90 && steady <= 110, "steady state is %u, expected ~100 (rate * lifetime)", steady);

        dai_particles_enable(p, e, 0);
        for (int i = 0; i < 90; ++i) dai_particles_update(p, 1.0f / 60.0f);
        CHECK(dai_particles_count(p) == 0, "%u particles outlived their lifetime after the emitter stopped",
              dai_particles_count(p));
        dai_particles_destroy(p);
    }

    // ---- 2. the capacity is a hard ceiling, not a suggestion
    {
        dai_particles *p = dai_particles_create(64);
        dai_emitter_desc d = dai_emitter_desc_default();
        d.rate = 10000.0f; d.lifetime = 100.0f;
        dai_particles_add(p, &d);
        for (int i = 0; i < 60; ++i) dai_particles_update(p, 1.0f / 60.0f);
        CHECK(dai_particles_count(p) == 64, "capacity 64 but %u particles are live", dai_particles_count(p));
        dai_particles_destroy(p);
    }

    // ---- 3. physics: gravity pulls down, drag slows down
    {
        dai_particles *p = dai_particles_create(256);
        dai_emitter_desc d = dai_emitter_desc_default();
        d.rate = 0.0f; d.lifetime = 10.0f; d.lifetime_jitter = 0.0f;
        d.position = { 0, 10, 0 };
        d.direction = { 1, 0, 0 }; d.spread_deg = 0.0f;
        d.speed = 5.0f; d.speed_jitter = 0.0f;
        d.gravity = 1.0f; d.drag = 0.0f;
        dai_emitter e = dai_particles_add(p, &d);
        dai_particles_burst(p, e, 1);
        for (int i = 0; i < 60; ++i) dai_particles_update(p, 1.0f / 60.0f);   // 1 s
        dai_particle out[8];
        uint32_t n = dai_particles_fill(p, out, 8, dai_vec3{ 0, 0, 20 });
        CHECK(n == 1, "expected one particle, got %u", n);
        if (n) {
            // x = v*t = 5, y = 10 - 0.5*g*t^2 ~= 5.1 (discrete integration is a bit lossy)
            CHECK(fabsf(out[0].position.x - 5.0f) < 0.2f, "x is %.2f after 1 s at 5 m/s, expected 5", out[0].position.x);
            CHECK(out[0].position.y > 4.5f && out[0].position.y < 5.8f,
                  "y is %.2f after 1 s of gravity from 10 m, expected ~5.1", out[0].position.y);
        }
        dai_particles_destroy(p);
    }

    // ---- 4. same seed, same effect (a replay must look identical)
    {
        auto run = [](uint32_t seed, std::vector<dai_particle> &out) {
            dai_particles *p = dai_particles_create(512);
            dai_emitter_desc d = dai_emitter_desc_default();
            d.seed = seed; d.rate = 200.0f;
            dai_particles_add(p, &d);
            for (int i = 0; i < 45; ++i) dai_particles_update(p, 1.0f / 60.0f);
            out.resize(dai_particles_count(p));
            if (!out.empty()) dai_particles_fill(p, out.data(), (uint32_t)out.size(), dai_vec3{ 0,0,10 });
            dai_particles_destroy(p);
        };
        std::vector<dai_particle> a, b, c;
        run(1234, a); run(1234, b); run(9999, c);
        CHECK(!a.empty() && a.size() == b.size(), "same seed gave %zu vs %zu particles", a.size(), b.size());
        bool same = a.size() == b.size() && !std::memcmp(a.data(), b.data(), a.size() * sizeof(dai_particle));
        CHECK(same, "same seed produced different particles - effects will not replay");
        bool differ = c.size() != a.size() || std::memcmp(a.data(), c.data(), std::min(a.size(), c.size()) * sizeof(dai_particle));
        CHECK(differ, "a different seed produced the identical effect - the seed is ignored");
    }

    // ---- 5. sorted back to front, which alpha blending depends on
    {
        dai_particles *p = dai_particles_create(64);
        dai_emitter_desc d = dai_emitter_desc_default();
        d.rate = 0.0f; d.lifetime = 10.0f; d.speed = 0.0f; d.gravity = 0.0f;
        for (int i = 0; i < 5; ++i) {
            d.position = { 0, 0, (float)i * 3.0f };
            dai_emitter e = dai_particles_add(p, &d);
            dai_particles_burst(p, e, 1);
        }
        dai_particle out[16];
        uint32_t n = dai_particles_fill(p, out, 16, dai_vec3{ 0, 0, 100 });   // camera far on +Z
        CHECK(n == 5, "expected 5 particles, got %u", n);
        bool sorted = true;
        for (uint32_t i = 1; i < n; ++i) if (out[i].position.z < out[i-1].position.z) sorted = false;
        CHECK(sorted, "particles are not sorted back to front for the camera");
        dai_particles_destroy(p);
    }

    // ---- 6. they actually show up on screen, and additive is brighter
    {
        char err[256] = {0};
        dai_render_desc rd{}; rd.width = 512; rd.height = 512; rd.msaa = 4;
        dai_renderer *r = dai_render_create(&rd, err, sizeof(err));
        if (!r) { std::printf("  renderer unavailable (%s), skipping the visual half\n", err); }
        else {
            dai_render_sky(r, 0);
            dai_render_clear_color(r, 0.02f, 0.02f, 0.03f);
            dai_render_fog(r, 0.0f, dai_vec3{0,0,0});
            dai_render_exposure(r, 1.0f);
            dai_render_camera(r, dai_vec3{ 0, 0, 10 }, dai_vec3{ 0,0,0 }, dai_vec3{ 0,1,0 }, 55.0f, 0.1f, 100.0f);

            std::vector<uint8_t> px((size_t)512 * 512 * 4);
            dai_render_particles(r, nullptr, 0);
            dai_render_frame(r, nullptr, 0);
            dai_render_readback(r, px.data(), px.size());
            float empty = mean_lum(px);

            std::vector<dai_particle> ps;
            for (int i = 0; i < 200; ++i) {
                dai_particle q{};
                float a = i * 0.31f;
                q.position = { cosf(a) * (0.5f + i * 0.02f), sinf(a) * (0.5f + i * 0.02f), 0 };
                q.size = 0.6f; q.color = { 1.0f, 0.6f, 0.2f }; q.alpha = 0.8f; q.rotation = a;
                q.blend = DAI_BLEND_ALPHA;
                ps.push_back(q);
            }
            dai_render_particles(r, ps.data(), (uint32_t)ps.size());
            dai_render_frame(r, nullptr, 0);
            dai_render_readback(r, px.data(), px.size());
            float alpha_lum = mean_lum(px);
            char path[512];
            std::snprintf(path, sizeof(path), "%s/particles_alpha.png", outdir);
            dai_render_write_png(r, path);

            for (auto &q : ps) q.blend = DAI_BLEND_ADD;
            dai_render_particles(r, ps.data(), (uint32_t)ps.size());
            dai_render_frame(r, nullptr, 0);
            dai_render_readback(r, px.data(), px.size());
            float add_lum = mean_lum(px);
            std::snprintf(path, sizeof(path), "%s/particles_additive.png", outdir);
            dai_render_write_png(r, path);

            std::printf("  empty %.4f | alpha %.4f | additive %.4f\n", empty, alpha_lum, add_lum);
            CHECK(alpha_lum > empty + 0.02f, "200 particles did not change the frame (%.4f vs %.4f)", alpha_lum, empty);
            CHECK(add_lum > alpha_lum, "additive (%.4f) is not brighter than alpha (%.4f)", add_lum, alpha_lum);

            // Particles must be occluded by geometry in front of them. Comparing
            // brightness against the particle-only frame would be meaningless -
            // the wall itself is bright. Compare against the SAME wall without
            // particles instead: if the depth test works, the frames match.
            dai_render_instance wall = dai_render_instance_default();
            wall.position = { 0, 0, 4 }; wall.scale = { 6, 6, 0.2f }; wall.color = { 0.5f, 0.5f, 0.5f };
            dai_render_particles(r, nullptr, 0);
            dai_render_frame(r, &wall, 1);
            std::vector<uint8_t> wall_only(px.size());
            dai_render_readback(r, wall_only.data(), wall_only.size());

            dai_render_particles(r, ps.data(), (uint32_t)ps.size());
            dai_render_frame(r, &wall, 1);
            dai_render_readback(r, px.data(), px.size());
            std::snprintf(path, sizeof(path), "%s/particles_occluded.png", outdir);
            dai_render_write_png(r, path);
            size_t changed = 0;
            for (size_t i = 0; i < px.size(); i += 4)
                if (std::abs((int)px[i] - (int)wall_only[i]) > 8) ++changed;
            double leak = 100.0 * (double)changed / (double)(px.size() / 4);
            CHECK(leak < 2.0, "%.1f%% of the frame changed behind a solid wall - particles ignore the depth test", leak);
            std::printf("  occlusion leak %.2f%% of pixels\n", leak);
            dai_render_destroy(r);
        }
    }

    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
