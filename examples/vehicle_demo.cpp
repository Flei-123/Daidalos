// Daidalos: a machine built from joints, drawn through the scene layer.
// Chassis + 4 wheels on motorised hinges (bearings) + a piston arm (slider).
// Everything is driven from the tick callback, so it survives a rollback.
//
//   DAI_SHADER_DIR=shaders ./build/vehicle_demo [frames] [outdir]

#include "daidalos.h"
#include "dai_scene.h"
#include "dai_render.h"
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <vector>

struct Machine {
    dai_body  chassis = DAI_INVALID_BODY;
    dai_body  wheel[4]{};
    dai_joint axle[4]{};
    dai_body  arm = DAI_INVALID_BODY;
    dai_joint piston = DAI_INVALID_JOINT;
};

static void on_tick(dai_world *w, dai_tick t, void *user) {
    Machine *m = (Machine *)user;
    dai_input in{};
    dai_get_input(w, 0, t, &in);
    for (int i = 0; i < 4; ++i)
        dai_joint_set_motor(w, m->axle[i], DAI_MOTOR_VELOCITY, in.axis[0] * 12.0f);
    dai_joint_set_motor(w, m->piston, DAI_MOTOR_POSITION, 0.6f + in.axis[1] * 0.6f);
}

int main(int argc, char **argv) {
    int frames = argc > 1 ? std::atoi(argv[1]) : 6;
    const char *outdir = argc > 2 ? argv[2] : ".";

    dai_config cfg{};
    cfg.tick_hz = 60; cfg.max_bodies = 256; cfg.physics_threads = 3; cfg.seed = 11;
    dai_world *w = nullptr;
    if (dai_create(&cfg, &w) != DAI_OK) { std::printf("dai_create failed\n"); return 1; }
    dai_scene *sc = dai_scene_create(w);
    std::printf("%s | physics: %s\n", dai_version(), dai_backend_name(w));

    // ground + ramp
    {
        dai_entity_desc d = dai_entity_desc_default();
        d.body.shape = DAI_SHAPE_BOX; d.body.motion = DAI_STATIC;
        d.body.half_extent = { 80, 1, 80 }; d.body.position = { 0, -1, 0 };
        d.body.friction_static = 0.9f;
        d.color = { 0.31f, 0.34f, 0.28f };
        d.render_flags = DAI_RI_CHECKER | DAI_RI_NO_SHADOW;
        d.name = "ground";
        dai_scene_spawn(sc, &d);

        dai_entity_desc r = dai_entity_desc_default();
        r.body.shape = DAI_SHAPE_BOX; r.body.motion = DAI_STATIC;
        r.body.half_extent = { 4.0f, 0.25f, 3.0f };
        r.body.position = { 13.0f, 0.9f, 0 };
        r.body.rotation = { 0, 0, -0.13f, 0.99f };
        r.color = { 0.55f, 0.52f, 0.48f };
        r.roughness = 0.7f;
        r.name = "ramp";
        dai_scene_spawn(sc, &r);
    }

    Machine m;
    {
        dai_entity_desc d = dai_entity_desc_default();
        d.body.shape = DAI_SHAPE_BOX; d.body.motion = DAI_DYNAMIC;
        d.body.half_extent = { 1.6f, 0.35f, 0.9f };
        d.body.position = { 0, 1.2f, 0 };
        d.body.density = 700.0f; d.body.no_sleeping = 1;
        d.color = { 0.88f, 0.42f, 0.14f };
        d.roughness = 0.35f;
        d.name = "chassis";
        m.chassis = dai_scene_body(sc, dai_scene_spawn(sc, &d));
    }

    const float wx[4] = {  1.2f,  1.2f, -1.2f, -1.2f };
    const float wz[4] = {  1.05f, -1.05f, 1.05f, -1.05f };
    for (int i = 0; i < 4; ++i) {
        dai_entity_desc d = dai_entity_desc_default();
        d.body.shape = DAI_SHAPE_SPHERE; d.body.motion = DAI_DYNAMIC;
        d.body.half_extent = { 0.55f, 0, 0 };
        d.body.position = { wx[i], 0.6f, wz[i] };
        d.body.density = 900.0f; d.body.friction_static = 1.4f; d.body.no_sleeping = 1;
        d.color = { 0.12f, 0.12f, 0.14f };
        d.roughness = 0.8f;
        m.wheel[i] = dai_scene_body(sc, dai_scene_spawn(sc, &d));

        dai_joint_desc jd{};
        jd.type = DAI_JOINT_HINGE;
        jd.a = m.chassis; jd.b = m.wheel[i];
        jd.anchor = { wx[i], 0.6f, wz[i] };
        jd.axis = { 0, 0, 1 };
        jd.normal_axis = { 1, 0, 0 };
        jd.max_motor_force = 60000.0f;
        m.axle[i] = dai_joint_create(w, &jd);
        if (m.axle[i] == DAI_INVALID_JOINT) std::printf("axle %d failed: %s\n", i, dai_last_error(w));
    }

    {
        dai_entity_desc d = dai_entity_desc_default();
        d.body.shape = DAI_SHAPE_BOX; d.body.motion = DAI_DYNAMIC;
        d.body.half_extent = { 0.25f, 0.7f, 0.25f };
        d.body.position = { 0, 2.4f, 0 };
        d.body.density = 400.0f; d.body.no_sleeping = 1;
        d.mesh = DAI_MESH_CYLINDER;
        d.render_scale = { 0.25f, 0.7f, 0.25f };
        d.color = { 0.35f, 0.75f, 0.95f };
        d.roughness = 0.25f;
        d.name = "arm";
        m.arm = dai_scene_body(sc, dai_scene_spawn(sc, &d));
    }

    dai_joint_desc pd{};
    pd.type = DAI_JOINT_SLIDER;
    pd.a = m.chassis; pd.b = m.arm;
    pd.anchor = { 0, 2.0f, 0 };
    pd.axis = { 0, 1, 0 };
    pd.normal_axis = { 1, 0, 0 };
    pd.enable_limits = 1; pd.limit_min = 0.0f; pd.limit_max = 1.4f;
    pd.max_motor_force = 40000.0f;
    m.piston = dai_joint_create(w, &pd);
    if (m.piston == DAI_INVALID_JOINT) std::printf("piston failed: %s\n", dai_last_error(w));

    // a few crates to knock over, so the frames show something happening
    for (int i = 0; i < 8; ++i) {
        dai_entity_desc d = dai_entity_desc_default();
        d.body.shape = DAI_SHAPE_BOX; d.body.motion = DAI_DYNAMIC;
        d.body.half_extent = { 0.4f, 0.4f, 0.4f };
        d.body.position = { 7.0f + (i % 2) * 0.85f, 0.4f + (i / 2) * 0.85f, -1.5f };
        d.body.density = 300.0f;
        dai_scene_spawn(sc, &d);
    }

    std::printf("machine: %u entities, %u joints\n", dai_scene_count(sc), dai_joint_count(w));
    dai_set_tick_callback(w, on_tick, &m);

    // NOTE: inputs live in a ring sized after the snapshot window. Writing
    // thousands of ticks ahead silently overwrites the near future - feed them
    // as the simulation advances.
    auto feed = [&](dai_tick t) {
        dai_input in{};
        if (t > 40) in.axis[0] = -1.0f;
        in.axis[1] = sinf((float)t * 0.02f);
        dai_set_input(w, 0, t, &in);
    };

    char err[256] = {0};
    dai_render_desc rd{}; rd.width = 1280; rd.height = 720; rd.msaa = 4; rd.shadow_size = 2048;
    dai_renderer *r = dai_render_create(&rd, err, sizeof(err));
    if (!r) { std::printf("no renderer: %s\n", err); }
    if (r) {
        std::printf("GPU: %s\n", dai_render_device_name(r));
        dai_render_sun(r, dai_vec3{ 0.38f, 0.86f, 0.34f }, dai_vec3{ 1.0f, 0.95f, 0.86f }, 1.3f);
        dai_render_ambient(r, dai_vec3{ 0.24f, 0.40f, 0.72f }, dai_vec3{ 0.26f, 0.24f, 0.20f }, 0.38f);
        dai_render_fog(r, 0.0035f, dai_vec3{ 0.56f, 0.64f, 0.74f });
        dai_render_shadow_extent(r, 18.0f);
        dai_render_exposure(r, 0.58f);
    }

    dai_camera cam = dai_camera_default();
    cam.distance = 11.0f; cam.pitch = 0.22f; cam.yaw = -0.9f; cam.fov = 55.0f;
    std::vector<dai_render_instance> inst(512);

    for (int fi = 0; fi < frames; ++fi) {
        for (int i = 0; i < 45; ++i) { feed(dai_current_tick(w)); dai_step(w); }

        dai_transform c{}; dai_body_get(w, m.chassis, &c);
        dai_joint_state ps{}; dai_joint_get(w, m.piston, &ps);
        dai_joint_state as{}; dai_joint_get(w, m.axle[0], &as);
        std::printf("frame %d | tick %4llu | chassis x=%6.2f y=%5.2f | wheel %6.2f rad/s | piston %.2f m\n",
            fi, (unsigned long long)dai_current_tick(w), c.position.x, c.position.y, as.speed, ps.position);
        if (!r) continue;

        dai_camera_follow(&cam, dai_vec3{ c.position.x, c.position.y + 0.6f, c.position.z }, 0.25f, 0.75f);
        dai_render_camera(r, dai_camera_eye(&cam), cam.target, dai_vec3{ 0,1,0 }, cam.fov, cam.znear, cam.zfar);

        uint32_t n = dai_scene_instances(sc, inst.data(), (uint32_t)inst.size(), 0.0f);
        dai_render_frame(r, inst.data(), n);
        char path[512];
        std::snprintf(path, sizeof(path), "%s/vehicle_%02d.png", outdir, fi);
        dai_render_write_png(r, path);
    }

    dai_stats st{}; dai_get_stats(w, &st);
    std::printf("sim: %llu ticks, %.4f ms/tick, %u bodies (%u active), %u joints\n",
        (unsigned long long)st.ticks_simulated, st.avg_step_ms, st.bodies, st.active_bodies, dai_joint_count(w));

    if (r) dai_render_destroy(r);
    dai_scene_destroy(sc);
    dai_destroy(w);
    return 0;
}
