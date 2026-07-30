// Daidalos sandbox: a general purpose scene, not a genre demo.
//
// Shows the parts a game actually needs from an engine:
//   scene layer      -> spawn entities, never guess sizes from user_data
//   mixed primitives -> boxes, spheres, capsules, cylinders, cones
//   materials        -> colour, roughness, emissive, checkerboard floor
//   camera helper    -> orbit around the action, follow a body
//   deterministic sim-> same seed, same frames, always
//
//   DAI_SHADER_DIR=shaders ./build/sandbox_demo [frames] [outdir]

#include "daidalos.h"
#include "dai_scene.h"
#include "dai_render.h"
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <vector>

static dai_entity_desc box_desc(dai_vec3 pos, dai_vec3 he, dai_vec3 color, int motion = DAI_DYNAMIC) {
    dai_entity_desc d = dai_entity_desc_default();
    d.body.shape = DAI_SHAPE_BOX; d.body.motion = motion;
    d.body.half_extent = he; d.body.position = pos;
    d.color = color;
    return d;
}

int main(int argc, char **argv) {
    int frames = argc > 1 ? std::atoi(argv[1]) : 6;
    const char *outdir = argc > 2 ? argv[2] : ".";

    dai_config cfg{};
    cfg.tick_hz = 60; cfg.max_bodies = 1024; cfg.physics_threads = 3; cfg.seed = 20260730;
    dai_world *w = nullptr;
    if (dai_create(&cfg, &w) != DAI_OK) { std::printf("dai_create failed\n"); return 1; }
    dai_scene *sc = dai_scene_create(w);
    std::printf("%s | physics: %s\n", dai_version(), dai_backend_name(w));

    // ---- ground: one static box, drawn with a checkerboard so the eye can
    //      read distance and scale. It never casts a shadow on itself.
    {
        dai_entity_desc d = box_desc({ 0, -1, 0 }, { 60, 1, 60 }, { 0.30f, 0.34f, 0.27f }, DAI_STATIC);
        d.body.friction_static = 0.9f;
        d.render_flags = DAI_RI_CHECKER | DAI_RI_NO_SHADOW;
        d.name = "ground";
        dai_scene_spawn(sc, &d);
    }

    // ---- a wall of crates
    for (int x = 0; x < 6; ++x)
        for (int y = 0; y < 5; ++y) {
            dai_entity_desc d = box_desc({ -3.0f + x * 1.02f, 0.5f + y * 1.02f, -4.0f },
                                         { 0.5f, 0.5f, 0.5f },
                                         { 0.78f, 0.42f, 0.18f });
            d.body.density = 500.0f;
            dai_scene_spawn(sc, &d);
        }

    // ---- a ramp
    {
        dai_entity_desc d = box_desc({ 9.0f, 0.6f, 2.0f }, { 4.0f, 0.25f, 3.0f },
                                     { 0.50f, 0.52f, 0.56f }, DAI_STATIC);
        d.body.rotation = { 0, 0, -0.13f, 0.99f };
        d.roughness = 0.6f;
        dai_scene_spawn(sc, &d);
    }

    // ---- pillars: cylinders and cones, to prove the mesh library is real
    for (int i = 0; i < 4; ++i) {
        dai_entity_desc d = dai_entity_desc_default();
        d.body.shape = DAI_SHAPE_BOX; d.body.motion = DAI_STATIC;
        d.body.half_extent = { 0.5f, 2.0f, 0.5f };
        d.body.position = { -9.0f, 2.0f, -6.0f + i * 4.0f };
        d.mesh = DAI_MESH_CYLINDER;
        d.render_scale = { 0.5f, 2.0f, 0.5f };
        d.color = { 0.75f, 0.75f, 0.78f };
        d.roughness = 0.35f;
        dai_scene_spawn(sc, &d);

        dai_entity_desc c = dai_entity_desc_default();
        c.body.shape = DAI_SHAPE_BOX; c.body.motion = DAI_STATIC;
        c.body.half_extent = { 0.6f, 0.6f, 0.6f };
        c.body.position = { -9.0f, 4.6f, -6.0f + i * 4.0f };
        c.mesh = DAI_MESH_CONE;
        c.render_scale = { 0.7f, 0.6f, 0.7f };
        c.color = { 0.85f, 0.35f, 0.25f };
        c.emissive = 0.15f;
        dai_scene_spawn(sc, &c);
    }

    // ---- a capsule "character" standing around
    {
        dai_entity_desc d = dai_entity_desc_default();
        d.body.shape = DAI_SHAPE_CAPSULE; d.body.motion = DAI_DYNAMIC;
        d.body.half_extent = { 0.4f, 0.6f, 0 };     // radius, half shaft
        d.body.position = { 3.5f, 1.2f, 3.0f };
        d.body.no_sleeping = 1;
        d.color = { 0.30f, 0.62f, 0.85f };
        d.roughness = 0.5f;
        d.name = "character";
        dai_scene_spawn(sc, &d);
    }

    // ---- the wrecking ball, aimed at the crates
    dai_entity ball;
    {
        dai_entity_desc d = dai_entity_desc_default();
        d.body.shape = DAI_SHAPE_SPHERE; d.body.motion = DAI_DYNAMIC;
        d.body.half_extent = { 0.9f, 0, 0 };
        d.body.position = { 6.0f, 5.0f, 6.0f };
        d.body.density = 3000.0f;
        d.body.no_sleeping = 1;
        d.color = { 0.90f, 0.78f, 0.25f };
        d.roughness = 0.25f;
        d.name = "ball";
        ball = dai_scene_spawn(sc, &d);
        dai_body_set_velocity(w, dai_scene_body(sc, ball), dai_vec3{ -6.0f, 0, -7.0f }, dai_vec3{ 0,0,0 });
    }

    // ---- some loose spheres for good measure
    for (int i = 0; i < 12; ++i) {
        dai_entity_desc d = dai_entity_desc_default();
        d.body.shape = DAI_SHAPE_SPHERE; d.body.motion = DAI_DYNAMIC;
        float r = 0.25f + 0.15f * (i % 3);
        d.body.half_extent = { r, 0, 0 };
        d.body.position = { -1.0f + (i % 4) * 0.8f, 6.0f + i * 0.7f, 2.0f + (i / 4) * 0.8f };
        d.body.restitution = 0.4f;
        dai_scene_spawn(sc, &d);   // colour comes from the scene palette
    }

    std::printf("scene: %u entities\n", dai_scene_count(sc));

    // ---- renderer
    char err[256] = {0};
    dai_render_desc rd{}; rd.width = 1280; rd.height = 720; rd.msaa = 4; rd.shadow_size = 2048;
    dai_renderer *r = dai_render_create(&rd, err, sizeof(err));
    if (!r) { std::printf("renderer failed: %s\n", err); dai_scene_destroy(sc); dai_destroy(w); return 2; }
    std::printf("GPU: %s | meshes: %u\n", dai_render_device_name(r), dai_render_mesh_count(r));

    dai_render_sun(r, dai_vec3{ 0.42f, 0.80f, 0.42f }, dai_vec3{ 1.0f, 0.95f, 0.86f }, 1.3f);
    dai_render_ambient(r, dai_vec3{ 0.24f, 0.40f, 0.72f }, dai_vec3{ 0.26f, 0.24f, 0.20f }, 0.38f);
    dai_render_fog(r, 0.0035f, dai_vec3{ 0.56f, 0.64f, 0.74f });
    dai_render_shadow_extent(r, 22.0f);
    dai_render_exposure(r, 0.58f);

    dai_camera cam = dai_camera_default();
    cam.target = { 0.5f, 1.6f, -0.5f };
    cam.distance = 13.0f;
    cam.pitch = 0.24f;
    cam.fov = 52.0f;

    std::vector<dai_render_instance> inst(2048);
    for (int fi = 0; fi < frames; ++fi) {
        for (int i = 0; i < 20; ++i) dai_step(w);

        cam.yaw = 0.75f + fi * 0.16f;          // slow orbit
        dai_vec3 eye = dai_camera_eye(&cam);
        dai_render_camera(r, eye, cam.target, dai_vec3{ 0,1,0 }, cam.fov, cam.znear, cam.zfar);

        uint32_t n = dai_scene_instances(sc, inst.data(), (uint32_t)inst.size(), 0.0f);
        if (dai_render_frame(r, inst.data(), n) != DAI_OK) { std::printf("render failed\n"); break; }

        char path[512];
        std::snprintf(path, sizeof(path), "%s/sandbox_%02d.png", outdir, fi);
        dai_render_write_png(r, path);
        std::printf("frame %d | tick %4llu | %3u instances | %u draws | %.1f ms -> %s\n",
                    fi, (unsigned long long)dai_current_tick(w), n, dai_render_last_draws(r),
                    dai_render_last_ms(r), path);
    }

    dai_stats st{}; dai_get_stats(w, &st);
    std::printf("sim: %llu ticks, %.4f ms/tick, %u bodies (%u active)\n",
                (unsigned long long)st.ticks_simulated, st.avg_step_ms, st.bodies, st.active_bodies);

    dai_render_destroy(r);
    dai_scene_destroy(sc);
    dai_destroy(w);
    return 0;
}
