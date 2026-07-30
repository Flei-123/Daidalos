// Daidalos: a machine built from joints - the thing a construction game needs.
// Chassis + 4 wheels on motorised hinges (bearings) + a piston arm (slider).
// Everything is driven from the tick callback, so it survives a rollback.
//
//   DAI_SHADER_DIR=shaders ./build/vehicle_demo [frames] [outdir]

#include "daidalos.h"
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

    float throttle = in.axis[0];            // -1 .. 1
    for (int i = 0; i < 4; ++i)
        dai_joint_set_motor(w, m->axle[i], DAI_MOTOR_VELOCITY, throttle * 12.0f);

    // piston follows axis 1 as a position target
    dai_joint_set_motor(w, m->piston, DAI_MOTOR_POSITION, 0.6f + in.axis[1] * 0.6f);
}

int main(int argc, char **argv) {
    int frames = argc > 1 ? std::atoi(argv[1]) : 4;
    const char *outdir = argc > 2 ? argv[2] : ".";

    dai_config cfg{};
    cfg.tick_hz = 60; cfg.max_bodies = 256; cfg.physics_threads = 3; cfg.seed = 11;
    dai_world *w = nullptr;
    if (dai_create(&cfg, &w) != DAI_OK) { std::printf("dai_create failed\n"); return 1; }
    std::printf("%s | Backend: %s\n", dai_version(), dai_backend_name(w));

    dai_body_desc f{};
    f.shape = DAI_SHAPE_BOX; f.motion = DAI_STATIC;
    f.half_extent = { 60, 0.5f, 60 }; f.position = { 0, -0.5f, 0 }; f.rotation = { 0,0,0,1 };
    f.friction_static = 0.9f;
    dai_body_create(w, &f);

    // a ramp to drive at
    dai_body_desc ramp{};
    ramp.shape = DAI_SHAPE_BOX; ramp.motion = DAI_STATIC;
    ramp.half_extent = { 4.0f, 0.25f, 3.0f };
    ramp.position = { 13.0f, 0.9f, 0 };
    ramp.rotation = { 0, 0, -0.13f, 0.99f };   // ~15 degrees around Z
    ramp.user_data = 9;
    dai_body_create(w, &ramp);

    Machine m;
    dai_body_desc ch{};
    ch.shape = DAI_SHAPE_BOX; ch.motion = DAI_DYNAMIC;
    ch.half_extent = { 1.6f, 0.35f, 0.9f };
    ch.position = { 0, 1.2f, 0 }; ch.rotation = { 0,0,0,1 };
    ch.density = 700.0f; ch.no_sleeping = 1; ch.user_data = 1;
    m.chassis = dai_body_create(w, &ch);

    const float wx[4] = {  1.2f,  1.2f, -1.2f, -1.2f };
    const float wz[4] = {  1.05f, -1.05f, 1.05f, -1.05f };
    for (int i = 0; i < 4; ++i) {
        dai_body_desc wd{};
        wd.shape = DAI_SHAPE_SPHERE; wd.motion = DAI_DYNAMIC;
        wd.half_extent = { 0.55f, 0, 0 };
        wd.position = { wx[i], 0.6f, wz[i] }; wd.rotation = { 0,0,0,1 };
        wd.density = 900.0f; wd.friction_static = 1.4f; wd.no_sleeping = 1;
        wd.user_data = 2;
        m.wheel[i] = dai_body_create(w, &wd);

        dai_joint_desc jd{};
        jd.type = DAI_JOINT_HINGE;
        jd.a = m.chassis; jd.b = m.wheel[i];
        jd.anchor = { wx[i], 0.6f, wz[i] };
        jd.axis = { 0, 0, 1 };                 // wheels turn around Z
        jd.normal_axis = { 1, 0, 0 };
        jd.max_motor_force = 60000.0f;
        m.axle[i] = dai_joint_create(w, &jd);
        if (m.axle[i] == DAI_INVALID_JOINT) std::printf("axle %d failed: %s\n", i, dai_last_error(w));
    }

    // piston arm on the roof
    dai_body_desc ad{};
    ad.shape = DAI_SHAPE_BOX; ad.motion = DAI_DYNAMIC;
    ad.half_extent = { 0.25f, 0.7f, 0.25f };
    ad.position = { 0, 2.4f, 0 }; ad.rotation = { 0,0,0,1 };
    ad.density = 400.0f; ad.no_sleeping = 1; ad.user_data = 3;
    m.arm = dai_body_create(w, &ad);

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

    std::printf("Maschine: %u Bodies, %u Gelenke\n", 8u, dai_joint_count(w));
    dai_set_tick_callback(w, on_tick, &m);

    // NOTE: inputs live in a ring sized after the snapshot window. Writing
    // 2000 ticks ahead silently overwrites the near future, and dai_get_input
    // then correctly reports "not set". Feed inputs as the sim advances.
    auto feed = [&](dai_tick t) {
        dai_input in{};
        if (t > 60) in.axis[0] = -1.0f;                       // drive towards the ramp
        in.axis[1] = sinf((float)t * 0.02f);                  // pump the piston
        dai_set_input(w, 0, t, &in);
    };

    char err[256] = {0};
    dai_renderer *r = nullptr;
    dai_render_desc rd{}; rd.width = 960; rd.height = 540;
    r = dai_render_create(&rd, err, sizeof(err));
    if (r) {
        std::printf("GPU: %s\n", dai_render_device_name(r));
        dai_render_clear_color(r, 0.05f, 0.06f, 0.08f);
        dai_render_light(r, dai_vec3{ 0.4f, 0.85f, 0.35f });
    } else {
        std::printf("kein Renderer: %s\n", err);
    }

    std::vector<dai_transform> tr(512);
    std::vector<dai_render_instance> inst(512);

    for (int fi = 0; fi < frames; ++fi) {
        for (int i = 0; i < 45; ++i) { feed(dai_current_tick(w)); dai_step(w); }

        dai_transform c{}; dai_body_get(w, m.chassis, &c);
        dai_joint_state ps{}; dai_joint_get(w, m.piston, &ps);
        dai_joint_state as{}; dai_joint_get(w, m.axle[0], &as);
        std::printf("Frame %d | Tick %4llu | Chassis x=%6.2f y=%5.2f | Rad %6.2f rad/s | Kolben %.2f m\n",
            fi, (unsigned long long)dai_current_tick(w), c.position.x, c.position.y, as.speed, ps.position);

        if (!r) continue;
        uint32_t n = dai_get_transforms(w, tr.data(), (uint32_t)tr.size(), 0.0f);
        for (uint32_t i = 0; i < n; ++i) {
            inst[i].position = tr[i].position;
            inst[i].rotation = tr[i].rotation;
            switch (tr[i].user_data) {
            case 1: inst[i].half_extent = { 1.6f, 0.35f, 0.9f }; inst[i].color = { 0.90f, 0.55f, 0.20f }; break;
            case 2: inst[i].half_extent = { 0.55f, 0.55f, 0.55f }; inst[i].color = { 0.15f, 0.15f, 0.17f }; break;
            case 3: inst[i].half_extent = { 0.25f, 0.7f, 0.25f }; inst[i].color = { 0.35f, 0.75f, 0.95f }; break;
            case 9: inst[i].half_extent = { 4.0f, 0.25f, 3.0f }; inst[i].color = { 0.45f, 0.40f, 0.35f }; break;
            default: inst[i].half_extent = { 60.0f, 0.5f, 60.0f }; inst[i].color = { 0.18f, 0.20f, 0.22f }; break;
            }
        }
        dai_render_camera(r,
            dai_vec3{ c.position.x - 6.5f, 4.2f, 8.5f },
            dai_vec3{ c.position.x, 1.2f, 0.0f },
            dai_vec3{ 0, 1, 0 }, 50.0f, 0.1f, 300.0f);
        dai_render_frame(r, inst.data(), n);
        char path[512];
        std::snprintf(path, sizeof(path), "%s/vehicle_%02d.ppm", outdir, fi);
        dai_render_write_ppm(r, path);
    }

    dai_stats st{}; dai_get_stats(w, &st);
    std::printf("Sim: %llu Ticks, %.4f ms/Tick, %u Bodies (%u aktiv), %u Gelenke\n",
        (unsigned long long)st.ticks_simulated, st.avg_step_ms, st.bodies, st.active_bodies, dai_joint_count(w));

    if (r) dai_render_destroy(r);
    dai_destroy(w);
    return 0;
}
