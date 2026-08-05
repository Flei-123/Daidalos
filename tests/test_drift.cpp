// Does a symmetric stack stay symmetric?
//
// The report: a box is dropped, a ball is dropped on top of it, both at
// x = 0, z = 0 with no rotation - and the ball ends up rolling off in one
// direction. Nothing in that setup breaks the symmetry, so whatever moved it
// sideways came out of the solver, and a solver that invents a direction will
// invent it again in a game.
//
// This measures it instead of arguing about it: run the exact scene and print
// how far off centre things end up. The number is the test.

#include "daidalos.h"

#include <cmath>
#include <cstdio>
#include <cstring>

static int failures = 0;

#define CHECK(cond, ...)                                                       \
    do {                                                                       \
        if (!(cond)) { std::printf("FAIL: "); std::printf(__VA_ARGS__);         \
                       std::printf("\n"); ++failures; }                        \
    } while (0)

static float run_case(int backend, const char *label, float *out_box_drift) {
    dai_config cfg{};
    cfg.backend = backend;
    cfg.tick_hz = 60;
    cfg.max_bodies = 64;
    dai_world *w = nullptr;
    if (dai_create(&cfg, &w) != DAI_OK || !w) {
        std::printf("   %s: world creation failed\n", label);
        return -1.0f;
    }
    dai_set_gravity(w, dai_vec3{ 0.0f, -9.81f, 0.0f });

    // the floor
    dai_body_desc fd{};
    fd.shape = DAI_SHAPE_BOX;
    fd.motion = DAI_STATIC;
    fd.position = dai_vec3{ 0.0f, -0.5f, 0.0f };
    fd.half_extent = dai_vec3{ 20.0f, 0.5f, 20.0f };
    fd.rotation = dai_quat{ 0, 0, 0, 1 };
    dai_body floor_b = dai_body_create(w, &fd);
    (void)floor_b;

    // a box, dropped
    dai_body_desc bd{};
    bd.shape = DAI_SHAPE_BOX;
    bd.motion = DAI_DYNAMIC;
    bd.position = dai_vec3{ 0.0f, 4.0f, 0.0f };
    bd.half_extent = dai_vec3{ 0.5f, 0.5f, 0.5f };
    bd.rotation = dai_quat{ 0, 0, 0, 1 };
    bd.density = 20.0f; bd.restitution = 0.5f;
    dai_body box = dai_body_create(w, &bd);

    // a ball, dropped onto it - same axis, no offset, no spin
    dai_body_desc sd{};
    sd.shape = DAI_SHAPE_SPHERE;
    sd.motion = DAI_DYNAMIC;
    sd.position = dai_vec3{ 0.0f, 8.0f, 0.0f };
    sd.half_extent = dai_vec3{ 0.4f, 0.4f, 0.4f };
    sd.rotation = dai_quat{ 0, 0, 0, 1 };
    sd.density = 20.0f; sd.restitution = 0.5f;
    dai_body ball = dai_body_create(w, &sd);

    for (int i = 0; i < 60 * 12; ++i) dai_step(w);

    dai_transform tb{}, ts{};
    dai_body_get(w, box, &tb);
    dai_body_get(w, ball, &ts);
    float ball_drift = std::sqrt(ts.position.x * ts.position.x + ts.position.z * ts.position.z);
    float box_drift  = std::sqrt(tb.position.x * tb.position.x + tb.position.z * tb.position.z);
    std::printf("   %-8s box (%.4f, %.3f, %.4f) drift %.4f | ball (%.4f, %.3f, %.4f) drift %.4f\n",
                label, (double)tb.position.x, (double)tb.position.y, (double)tb.position.z,
                (double)box_drift,
                (double)ts.position.x, (double)ts.position.y, (double)ts.position.z,
                (double)ball_drift);
    if (out_box_drift) *out_box_drift = box_drift;
    dai_destroy(w);
    return ball_drift;
}

int main(void) {
    std::printf("-- symmetric drop: box under ball, both on the y axis\n");

    float box_drift = 0.0f;
    float talos = run_case(DAI_PHYSICS_TALOS, "talos", &box_drift);

    // 6 seconds of simulation. A perfectly symmetric setup should not move
    // sideways at all; floating point contact ordering makes a few
    // millimetres unavoidable, and anything past a centimetre is the solver
    // choosing a direction.
    const float LIMIT = 0.01f;
    if (talos >= 0.0f) {
        CHECK(talos < LIMIT,
              "the ball drifted %.4f m sideways from a symmetric drop (limit %.3f). "
              "Nothing in the scene breaks the x/z symmetry, so this is the solver.",
              (double)talos, (double)LIMIT);
        CHECK(box_drift < LIMIT,
              "the box drifted %.4f m sideways from a symmetric drop (limit %.3f)",
              (double)box_drift, (double)LIMIT);
    }

    if (failures == 0) std::printf("ok: symmetric drops stay symmetric\n");
    else std::printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
