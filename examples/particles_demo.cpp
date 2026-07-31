// Particles driven by the simulation: a fire with smoke, and sparks that only
// exist because something actually hit the ground.
//
//   DAI_SHADER_DIR=shaders ./build/particles_demo [frames] [outdir]

#include "daidalos.h"
#include "dai_scene.h"
#include "dai_render.h"
#include "dai_particles.h"
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <vector>

int main(int argc, char **argv) {
    int frames = argc > 1 ? std::atoi(argv[1]) : 6;
    const char *outdir = argc > 2 ? argv[2] : ".";

    dai_config cfg{};
    cfg.tick_hz = 60; cfg.max_bodies = 512; cfg.physics_threads = 3; cfg.seed = 77;
    dai_world *w = nullptr;
    if (dai_create(&cfg, &w) != DAI_OK) return 1;
    dai_scene *sc = dai_scene_create(w);

    dai_entity_desc g = dai_entity_desc_default();
    g.body.shape = DAI_SHAPE_BOX; g.body.motion = DAI_STATIC;
    g.body.half_extent = { 30, 1, 30 }; g.body.position = { 0, -1, 0 };
    g.color = { 0.28f, 0.30f, 0.26f };
    g.render_flags = DAI_RI_CHECKER | DAI_RI_NO_SHADOW;
    dai_scene_spawn(sc, &g);

    // a low wall to catch the light of the fire
    for (int i = 0; i < 5; ++i) {
        dai_entity_desc d = dai_entity_desc_default();
        d.body.shape = DAI_SHAPE_BOX; d.body.motion = DAI_DYNAMIC;
        d.body.half_extent = { 0.6f, 0.4f, 0.6f };
        d.body.position = { -3.5f + i * 1.4f, 0.4f, -2.2f };
        d.body.density = 800.0f;
        d.color = { 0.55f, 0.5f, 0.45f };
        dai_scene_spawn(sc, &d);
    }

    // something that falls and makes sparks when it lands
    dai_entity_desc bd = dai_entity_desc_default();
    bd.body.shape = DAI_SHAPE_SPHERE; bd.body.motion = DAI_DYNAMIC;
    bd.body.half_extent = { 0.6f, 0, 0 };
    bd.body.position = { 2.0f, 9.0f, 1.0f };
    bd.body.density = 4000.0f; bd.body.restitution = 0.35f;
    bd.color = { 0.85f, 0.78f, 0.30f };
    bd.roughness = 0.25f;
    dai_entity ball = dai_scene_spawn(sc, &bd);

    dai_particles *fx = dai_particles_create(20000);

    dai_emitter_desc fire = dai_emitter_desc_default();
    fire.position = { -1.0f, 0.2f, 0.5f };
    fire.rate = 700.0f; fire.lifetime = 0.8f; fire.lifetime_jitter = 0.4f;
    fire.speed = 2.4f; fire.spread_deg = 22.0f; fire.gravity = -0.25f; fire.drag = 1.4f;
    fire.size_start = 0.55f; fire.size_end = 0.12f;
    fire.color_start = { 1.15f, 0.72f, 0.28f }; fire.color_end = { 0.9f, 0.18f, 0.03f };
    fire.alpha_start = 0.85f; fire.alpha_end = 0.0f;
    fire.blend = DAI_BLEND_ADD; fire.seed = 11;
    dai_particles_add(fx, &fire);

    dai_emitter_desc smoke = dai_emitter_desc_default();
    smoke.position = { -1.0f, 1.2f, 0.5f };
    smoke.rate = 120.0f; smoke.lifetime = 3.2f; smoke.lifetime_jitter = 0.35f;
    smoke.speed = 1.1f; smoke.spread_deg = 30.0f; smoke.gravity = -0.12f; smoke.drag = 0.8f;
    smoke.size_start = 0.6f; smoke.size_end = 2.6f;
    smoke.color_start = { 0.20f, 0.19f, 0.18f }; smoke.color_end = { 0.32f, 0.32f, 0.34f };
    smoke.alpha_start = 0.55f; smoke.alpha_end = 0.0f;
    smoke.blend = DAI_BLEND_ALPHA; smoke.seed = 22;
    dai_particles_add(fx, &smoke);

    dai_emitter_desc spark = dai_emitter_desc_default();
    spark.rate = 0.0f; spark.lifetime = 0.7f; spark.lifetime_jitter = 0.5f;
    spark.speed = 6.0f; spark.speed_jitter = 0.7f; spark.spread_deg = 75.0f;
    spark.gravity = 1.6f; spark.drag = 0.6f;
    spark.size_start = 0.10f; spark.size_end = 0.02f;
    spark.color_start = { 2.0f, 1.5f, 0.6f }; spark.color_end = { 1.0f, 0.35f, 0.05f };
    spark.blend = DAI_BLEND_ADD; spark.seed = 33;

    char err[256] = {0};
    dai_render_desc rd{}; rd.width = 1280; rd.height = 720; rd.msaa = 4; rd.shadow_size = 2048;
    dai_renderer *r = dai_render_create(&rd, err, sizeof(err));
    if (!r) { std::printf("no renderer: %s\n", err); return 2; }
    dai_render_sun(r, dai_vec3{ 0.35f, 0.55f, 0.45f }, dai_vec3{ 0.55f, 0.60f, 0.75f }, 0.45f);   // dusk
    dai_render_ambient(r, dai_vec3{ 0.10f, 0.14f, 0.26f }, dai_vec3{ 0.10f, 0.09f, 0.08f }, 0.35f);
    dai_render_fog(r, 0.006f, dai_vec3{ 0.16f, 0.18f, 0.26f });
    dai_render_exposure(r, 0.75f);
    dai_render_shadow_extent(r, 18.0f);

    std::vector<dai_render_instance> inst(1024);
    std::vector<dai_particle> parts(20000);
    bool landed = false;

    for (int f = 0; f < frames; ++f) {
        for (int i = 0; i < 12; ++i) {
            dai_step(w);
            dai_particles_update(fx, 1.0f / 60.0f);
            dai_transform t{};
            dai_body_get(w, dai_scene_body(sc, ball), &t);
            if (!landed && t.position.y < 0.75f) {          // it hit the ground
                landed = true;
                dai_particles_burst_at(fx, &spark, dai_vec3{ t.position.x, 0.15f, t.position.z }, 260);
            }
        }

        float a = 0.75f + f * 0.10f;
        dai_vec3 eye{ cosf(a) * 9.0f, 3.2f, sinf(a) * 9.0f };
        dai_render_camera(r, eye, dai_vec3{ -0.6f, 1.4f, 0.2f }, dai_vec3{ 0,1,0 }, 52.0f, 0.1f, 200.0f);

        uint32_t n = dai_scene_instances(sc, inst.data(), (uint32_t)inst.size(), 0.0f);
        uint32_t pn = dai_particles_fill(fx, parts.data(), (uint32_t)parts.size(), eye);
        dai_render_particles(r, parts.data(), pn);
        dai_render_frame(r, inst.data(), n);

        char path[512];
        std::snprintf(path, sizeof(path), "%s/particles_%02d.png", outdir, f);
        dai_render_write_png(r, path);
        std::printf("frame %d | %u bodies | %u particles | %.1f ms -> %s\n",
                    f, n, pn, dai_render_last_ms(r), path);
    }

    dai_particles_destroy(fx);
    dai_render_destroy(r);
    dai_scene_destroy(sc);
    dai_destroy(w);
    return 0;
}
