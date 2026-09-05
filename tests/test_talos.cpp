// The Talos backend, held to the same claims the engine test makes.
//
// This is the file that decides whether "the physics backend is swappable" is
// true. A backend that merely LINKS proves nothing: it has to collide, report
// contacts with a believable impulse, answer raycasts, drive a motor, and
// survive a rollback - because the engine above it uses all of that.
//
// Where a number is checkable from first principles, it is checked against the
// physics and not against Jolt: two engines are allowed to disagree in the last
// digit, but neither is allowed to disagree with sqrt(2gh).

#include "daidalos.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

static int g_fail = 0, g_pass = 0;
#define CHECK(cond, ...) do { \
    if (cond) { g_pass++; } \
    else { g_fail++; std::printf("  FAIL %s:%d  ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } \
} while (0)

static dai_world *world_with_floor(int backend, uint32_t hz = 60, uint32_t threads = 1) {
    dai_config cfg{};
    cfg.backend = backend;
    cfg.tick_hz = hz;
    cfg.max_bodies = 512;
    cfg.physics_threads = threads;
    cfg.snapshot_ring = 128;
    cfg.seed = 99;
    dai_world *w = nullptr;
    if (dai_create(&cfg, &w) != DAI_OK) return nullptr;
    dai_body_desc f{};
    f.shape = DAI_SHAPE_BOX; f.motion = DAI_STATIC;
    f.half_extent = { 50, 0.5f, 50 };
    f.position = { 0, -0.5f, 0 };
    f.rotation = { 0, 0, 0, 1 };
    dai_body_create(w, &f);
    return w;
}

static dai_body drop_box(dai_world *w, float x, float y, float he = 0.5f) {
    dai_body_desc d{};
    d.shape = DAI_SHAPE_BOX; d.motion = DAI_DYNAMIC;
    d.half_extent = { he, he, he };
    d.position = { x, y, 0 };
    d.rotation = { 0, 0, 0, 1 };
    d.density = 1000.0f;
    return dai_body_create(w, &d);
}

static dai_vec3 pos_of(dai_world *w, dai_body b) {
    dai_transform t{};
    dai_body_get(w, b, &t);
    return t.position;
}

// ---------------------------------------------------------------------------

static void test_exists() {
    std::printf("\n[1] Das Backend existiert und meldet sich\n");
    dai_world *w = world_with_floor(DAI_PHYSICS_TALOS);
    if (!w) { CHECK(false, "dai_create with DAI_PHYSICS_TALOS failed"); return; }
    std::printf("  %s\n", dai_version());
    std::printf("  Backend: %s\n", dai_backend_name(w));
    CHECK(std::strcmp(dai_backend_name(w), "talos") == 0,
          "asked for talos, got %s", dai_backend_name(w));
    dai_destroy(w);
}

static void test_it_collides() {
    std::printf("\n[2] Eine Kiste faellt und bleibt auf dem Boden liegen\n");
    dai_world *w = world_with_floor(DAI_PHYSICS_TALOS);
    if (!w) { CHECK(false, "no world"); return; }
    dai_body b = drop_box(w, 0, 4.0f, 0.5f);
    for (int i = 0; i < 240; ++i) dai_step(w);
    dai_vec3 p = pos_of(w, b);
    std::printf("  nach 4 s: y = %.4f (erwartet 0.5 = halbe Kantenlaenge)\n", p.y);
    // Resting on the floor means the centre sits one half extent above it. A
    // backend without collisions reports a large negative number here, which
    // is exactly the failure this catches.
    CHECK(p.y > 0.40f && p.y < 0.60f, "box came to rest at y = %.4f, not on the floor", p.y);
    CHECK(std::fabs(p.x) < 0.05f, "box drifted sideways to x = %.4f", p.x);
    dai_destroy(w);
}

static void test_determinism() {
    std::printf("\n[3] Zwei gleiche Welten bleiben gleich (300 Ticks)\n");
    dai_world *a = world_with_floor(DAI_PHYSICS_TALOS);
    dai_world *b = world_with_floor(DAI_PHYSICS_TALOS);
    if (!a || !b) { CHECK(false, "no world"); return; }
    for (int i = 0; i < 12; ++i) {
        float x = -2.0f + 0.37f * (float)i;
        drop_box(a, x, 1.0f + 0.9f * (float)i, 0.4f);
        drop_box(b, x, 1.0f + 0.9f * (float)i, 0.4f);
    }
    bool same = true;
    int diverged_at = -1;
    for (int i = 0; i < 300; ++i) {
        dai_step(a); dai_step(b);
        if (dai_checksum(a) != dai_checksum(b)) { same = false; diverged_at = i; break; }
    }
    std::printf("  Pruefsumme nach 300 Ticks: %016llx\n", (unsigned long long)dai_checksum(a));
    CHECK(same, "the two worlds diverged at tick %d", diverged_at);
    dai_destroy(a); dai_destroy(b);
}

static void test_rollback() {
    std::printf("\n[4] Rollback: der Zustand des Backends kommt zurueck\n");
    dai_world *w = world_with_floor(DAI_PHYSICS_TALOS);
    if (!w) { CHECK(false, "no world"); return; }
    for (int i = 0; i < 8; ++i) drop_box(w, -1.5f + 0.4f * (float)i, 2.0f + 0.6f * (float)i, 0.35f);
    for (int i = 0; i < 100; ++i) dai_step(w);

    // dai_rollback_to restores AND replays back to where it was, so landing
    // on the same checksum is the claim: the backend state came back and 40
    // ticks of simulation reproduced exactly.
    uint64_t before = dai_checksum(w);
    dai_tick at = dai_current_tick(w);
    dai_result r = dai_rollback_to(w, at - 40);
    CHECK(r == DAI_OK, "rollback failed: %s", dai_last_error(w));
    uint64_t after = dai_checksum(w);
    std::printf("  vor %016llx, nach Rollback ueber 40 Ticks %016llx\n",
                (unsigned long long)before, (unsigned long long)after);
    CHECK(dai_current_tick(w) == at, "tick not restored: %llu vs %llu",
          (unsigned long long)dai_current_tick(w), (unsigned long long)at);
    CHECK(before == after, "rollback + resimulation landed somewhere else");

    // Seeking back and STAYING there must show the past, not the present.
    uint64_t now = dai_checksum(w);
    r = dai_seek_to(w, at - 40);
    CHECK(r == DAI_OK, "seek failed: %s", dai_last_error(w));
    CHECK(dai_checksum(w) != now, "seeking 40 ticks back changed nothing");
    for (int i = 0; i < 40; ++i) dai_step(w);
    CHECK(dai_checksum(w) == now, "replaying from the seek did not reproduce");
    dai_destroy(w);
}

static void test_raycast() {
    std::printf("\n[5] Raycast\n");
    dai_world *w = world_with_floor(DAI_PHYSICS_TALOS);
    if (!w) { CHECK(false, "no world"); return; }
    dai_body b = drop_box(w, 0, 3.0f, 0.5f);
    for (int i = 0; i < 120; ++i) dai_step(w);

    dai_ray_hit hit{};
    int got = dai_raycast(w, dai_vec3{ 0, 8, 0 }, dai_vec3{ 0, -1, 0 }, 30.0f, &hit);
    std::printf("  von oben: %s, Distanz %.3f, Normale (%.2f %.2f %.2f)\n",
                got ? "Treffer" : "nichts", hit.distance, hit.normal.x, hit.normal.y, hit.normal.z);
    CHECK(got == 1, "the ray found nothing although a box is lying there");
    CHECK(hit.normal.y > 0.5f, "surface normal points the wrong way: %.3f", hit.normal.y);
    // 8 m up, the box top is at 1.0 m -> 7 m of air.
    CHECK(hit.distance > 6.5f && hit.distance < 7.5f, "distance %.3f m is not the box top", hit.distance);
    CHECK(hit.body == b, "the ray hit a different body than the box");

    // A ray into empty air must MISS, or every hit above is meaningless.
    dai_ray_hit miss{};
    int none = dai_raycast(w, dai_vec3{ 40, 8, 0 }, dai_vec3{ 0, 1, 0 }, 30.0f, &miss);
    CHECK(none == 0, "a ray fired at the sky reported a hit");
    dai_destroy(w);
}

static void test_impact_impulse() {
    std::printf("\n[6] Aufprall: gegen sqrt(2gh), nicht gegen 'groesser als null'\n");
    dai_world *w = world_with_floor(DAI_PHYSICS_TALOS, 240);
    if (!w) { CHECK(false, "no world"); return; }

    const float drop = 2.0f, radius = 0.5f, density = 1000.0f;
    dai_body_desc ball{};
    ball.shape = DAI_SHAPE_SPHERE; ball.motion = DAI_DYNAMIC;
    ball.half_extent = { radius, radius, radius };
    ball.position = { 0, drop + radius, 0 };
    ball.rotation = { 0, 0, 0, 1 };
    ball.density = density;
    ball.restitution = 0.0f;
    // Kept awake on purpose: a sleeping body reports no contacts at all (both
    // backends do this), and the resting-load check below needs one to exist.
    ball.no_sleeping = 1;
    dai_body_create(w, &ball);

    float mass = density * (4.0f / 3.0f) * 3.14159265f * radius * radius * radius;
    float v = std::sqrt(2.0f * 9.81f * drop);
    float expect = mass * v;

    float peak = 0.0f;
    for (int i = 0; i < 240 && peak == 0.0f; ++i) {
        dai_step(w);
        uint32_t cn = dai_poll_contacts(w, nullptr, 0);
        if (!cn) continue;
        std::vector<dai_contact> cc(cn);
        dai_poll_contacts(w, cc.data(), cn);
        for (const dai_contact &c : cc) if (c.impulse > peak) peak = c.impulse;
    }
    std::printf("  erwartet ~%.0f Ns, gemessen %.0f Ns (Masse %.1f kg, v %.2f m/s)\n",
                expect, peak, mass, v);
    CHECK(peak > 0.0f, "no impact impulse at all - contacts are not being measured");
    CHECK(peak > expect * 0.7f && peak < expect * 1.3f,
          "impulse %.0f Ns is not within 30%% of the textbook %.0f Ns", peak, expect);

    for (int i = 0; i < 480; ++i) dai_step(w);
    uint32_t rn = dai_poll_contacts(w, nullptr, 0);
    std::vector<dai_contact> rc(rn ? rn : 1);
    dai_poll_contacts(w, rc.data(), (uint32_t)rc.size());
    float resting = 0.0f;
    for (uint32_t i = 0; i < rn; ++i) if (rc[i].impulse > resting) resting = rc[i].impulse;
    std::printf("  in Ruhe danach: %.2f Ns (%u Kontakte)\n", resting, rn);
    CHECK(rn > 0, "a body resting on the floor reports no contact at all");
    CHECK(resting < expect * 0.1f, "resting reports %.1f Ns - impact and load are confused", resting);
    dai_destroy(w);
}

static void test_hinge_motor() {
    std::printf("\n[7] Lager mit Motor\n");
    dai_world *w = world_with_floor(DAI_PHYSICS_TALOS);
    if (!w) { CHECK(false, "no world"); return; }

    // Der Drehpunkt liegt 6 m hoch, damit der 3 m lange Arm frei durchschwingen
    // kann. Bei 3 m schleift sein Ende ueber den Boden, und dann misst der Test
    // nicht mehr den Motor, sondern wie viel Drehmoment noetig ist, um einen
    // aufliegenden Balken loszureissen - Jolt schafft das mit 40 kNm, Talos
    // braucht dafuer mehr. Das ist ein Unterschied der Backends, aber nicht der,
    // um den es hier geht.
    dai_body_desc arm{};
    arm.shape = DAI_SHAPE_BOX; arm.motion = DAI_DYNAMIC;
    arm.half_extent = { 1.5f, 0.15f, 0.15f };
    arm.position = { 1.5f, 6.0f, 0 };
    arm.rotation = { 0, 0, 0, 1 };
    arm.no_sleeping = 1;
    arm.density = 500.0f;
    dai_body a = dai_body_create(w, &arm);

    dai_joint_desc jd{};
    jd.type = DAI_JOINT_HINGE;
    jd.a = a; jd.b = DAI_INVALID_BODY;
    jd.anchor = { 0, 6.0f, 0 };
    jd.axis = { 0, 0, 1 };
    jd.normal_axis = { 1, 0, 0 };
    jd.max_motor_force = 40000.0f;
    dai_joint j = dai_joint_create(w, &jd);
    CHECK(j != DAI_INVALID_JOINT, "hinge could not be created: %s", dai_last_error(w));

    for (int i = 0; i < 30; ++i) dai_step(w);
    dai_joint_state st{};
    dai_joint_get(w, j, &st);
    std::printf("  ohne Motor nach 0.5 s: Winkel %.3f rad\n", st.position);
    CHECK(std::fabs(st.position) > 0.05f, "the arm did not swing down: %.4f rad", st.position);
    // The pivot has to hold: an arm anchored at the origin stays 1.5 m from it.
    dai_vec3 p = pos_of(w, a);
    float r = std::sqrt(p.x * p.x + (p.y - 6.0f) * (p.y - 6.0f));
    std::printf("  Abstand zum Drehpunkt: %.3f m (soll 1.5)\n", r);
    CHECK(std::fabs(r - 1.5f) < 0.15f, "the joint is not holding: radius %.3f m", r);

    dai_result mr = dai_joint_set_motor(w, j, DAI_MOTOR_VELOCITY, 2.0f);
    CHECK(mr == DAI_OK, "set_motor rejected: %d (%s)", (int)mr, dai_last_error(w));
    for (int i = 0; i < 60; ++i) dai_step(w);
    dai_joint_state st2{};
    dai_joint_get(w, j, &st2);
    std::printf("  Motor 2 rad/s, nach 1 s: Winkel %.3f rad, Drehzahl %.3f rad/s\n",
                st2.position, st2.speed);
    CHECK(std::fabs(st2.speed) > 1.0f, "the motor is not turning the arm: %.3f rad/s", st2.speed);
    dai_destroy(w);
}

static void test_compound() {
    std::printf("\n[8] Zusammengesetzter Koerper (merge)\n");
    dai_world *w = world_with_floor(DAI_PHYSICS_TALOS);
    if (!w) { CHECK(false, "no world"); return; }
    dai_body parts[3];
    for (int i = 0; i < 3; ++i) parts[i] = drop_box(w, (float)i * 1.0f, 3.0f, 0.5f);
    dai_body merged = dai_body_merge(w, parts, 3, 0);
    CHECK(merged != DAI_INVALID_BODY, "merge failed: %s", dai_last_error(w));
    for (int i = 0; i < 240; ++i) dai_step(w);
    dai_vec3 p = pos_of(w, merged);
    std::printf("  Verbund liegt bei y = %.3f\n", p.y);
    CHECK(p.y > 0.40f && p.y < 0.60f, "the compound body is at y = %.3f, not on the floor", p.y);
    dai_destroy(w);
}

int main() {
    test_exists();
    test_it_collides();
    test_determinism();
    test_rollback();
    test_raycast();
    test_impact_impulse();
    test_hinge_motor();
    test_compound();
    std::printf("\n==================================\n");
    std::printf("  %d bestanden, %d fehlgeschlagen\n", g_pass, g_fail);
    std::printf("==================================\n");
    return g_fail == 0 ? 0 : 1;
}
