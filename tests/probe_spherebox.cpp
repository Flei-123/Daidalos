// Does a sphere collide with a box under Talos? Justin's report is that in
// his scene they pass straight through each other. Three arrangements, each
// measured against what the geometry says has to happen.
#include "daidalos.h"
#include <cmath>
#include <cstdio>

static dai_world *mk(int backend) {
    dai_config cfg{};
    cfg.backend = backend;
    cfg.tick_hz = 60;
    cfg.max_bodies = 256;
    cfg.physics_threads = 1;
    cfg.snapshot_ring = 8;
    cfg.seed = 7;
    dai_world *w = nullptr;
    if (dai_create(&cfg, &w) != DAI_OK) return nullptr;
    return w;
}
static dai_vec3 P(dai_world *w, dai_body b) {
    dai_transform t{}; dai_body_get(w, b, &t); return t.position;
}
static void step(dai_world *w, int n) { for (int i = 0; i < n; ++i) dai_step(w); }

int main() {
    dai_world *w = mk(DAI_PHYSICS_TALOS);
    if (!w) { std::printf("no talos world\n"); return 1; }
    std::printf("backend: %s\n", dai_backend_name(w));

    // 1. sphere dropped onto a STATIC box
    dai_body_desc b{};
    b.shape = DAI_SHAPE_BOX; b.motion = DAI_STATIC;
    b.half_extent = { 1, 1, 1 }; b.position = { 0, 0, 0 }; b.rotation = { 0, 0, 0, 1 };
    dai_body box = dai_body_create(w, &b);

    dai_body_desc s{};
    s.shape = DAI_SHAPE_SPHERE; s.motion = DAI_DYNAMIC;
    s.half_extent = { 0.5f, 0.5f, 0.5f };
    s.position = { 0, 4, 0 }; s.rotation = { 0, 0, 0, 1 };
    s.density = 1000.0f;
    dai_body sph = dai_body_create(w, &s);
    step(w, 180);
    dai_vec3 p = P(w, sph);
    std::printf("1 static box, sphere lands at y=%.4f  (want 1.5)  %s\n",
                p.y, std::fabs(p.y - 1.5f) < 0.06f ? "OK" : "FAIL");

    // 2. both DYNAMIC, box resting on a floor, sphere dropped on it
    dai_world *w2 = mk(DAI_PHYSICS_TALOS);
    dai_body_desc f{};
    f.shape = DAI_SHAPE_BOX; f.motion = DAI_STATIC;
    f.half_extent = { 20, 0.5f, 20 }; f.position = { 0, -0.5f, 0 }; f.rotation = { 0, 0, 0, 1 };
    dai_body_create(w2, &f);
    dai_body_desc db{};
    db.shape = DAI_SHAPE_BOX; db.motion = DAI_DYNAMIC;
    db.half_extent = { 1, 1, 1 }; db.position = { 0, 1.0f, 0 }; db.rotation = { 0, 0, 0, 1 };
    db.density = 1000.0f;
    dai_body dbox = dai_body_create(w2, &db);
    dai_body_desc ds = s;
    ds.position = { 0, 5, 0 };
    dai_body dsph = dai_body_create(w2, &ds);
    step(w2, 240);
    dai_vec3 pb = P(w2, dbox), ps = P(w2, dsph);
    std::printf("2 dynamic box y=%.4f (want ~1.0), sphere y=%.4f (want ~2.5)  %s\n",
                pb.y, ps.y, (std::fabs(pb.y - 1.0f) < 0.15f && ps.y > 2.2f) ? "OK" : "FAIL");

    // 3. sphere pushed horizontally into a static box: does it stop?
    dai_world *w3 = mk(DAI_PHYSICS_TALOS);
    dai_body_desc fl3{};
    fl3.shape = DAI_SHAPE_BOX; fl3.motion = DAI_STATIC;
    fl3.half_extent = { 20, 0.5f, 20 }; fl3.position = { 0, -1.0f, 0 };
    fl3.rotation = { 0, 0, 0, 1 };
    dai_body_create(w3, &fl3);
    dai_body_desc sb{};
    sb.shape = DAI_SHAPE_BOX; sb.motion = DAI_STATIC;
    sb.half_extent = { 1, 1, 1 }; sb.position = { 3, -0.5f, 0 }; sb.rotation = { 0, 0, 0, 1 };
    dai_body_create(w3, &sb);
    dai_body_desc ms = s;
    ms.position = { -2, 0.0f, 0 };
    dai_body mv = dai_body_create(w3, &ms);
    dai_body_set_velocity(w3, mv, dai_vec3{ 4, 0, 0 }, dai_vec3{ 0, 0, 0 });
    for (int i = 0; i < 120; ++i) { dai_body_set_velocity(w3, mv, dai_vec3{ 4, 0, 0 }, dai_vec3{ 0, 0, 0 }); dai_step(w3); }
    dai_vec3 pm = P(w3, mv);
    std::printf("3 sphere driven into box stops at x=%.4f (want <=1.6)  %s\n",
                pm.x, pm.x <= 1.6f ? "OK" : "FAIL");

    // 4. two DYNAMIC bodies in free fall, nothing under them - the case that
    //    looks like "they do not collide" but is gravity doing its job.
    dai_world *w4 = mk(DAI_PHYSICS_TALOS);
    dai_body_desc a1 = db; a1.position = { 0, 0, 0 };
    dai_body_desc a2 = s;  a2.position = { 0, 3, 0 };
    dai_body c1 = dai_body_create(w4, &a1);
    dai_body c2 = dai_body_create(w4, &a2);
    step(w4, 120);
    dai_vec3 q1 = P(w4, c1), q2 = P(w4, c2);
    std::printf("4 free fall: gap started 3.00, now %.4f (unchanged = both just fall)\n",
                q2.y - q1.y);
    return 0;
}
