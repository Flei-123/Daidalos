// Daidalos test suite. Every claim in the README has to survive this file.
#include "daidalos.h"
#include <cstdio>
#include <cstring>
#include <cmath>
#include <chrono>
#include <vector>

static int g_fail = 0, g_pass = 0;
#define CHECK(cond, ...) do { \
    if (cond) { g_pass++; } \
    else { g_fail++; std::printf("  FAIL %s:%d  ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } \
} while (0)

// ---------------------------------------------------------------------------
// A tiny "game": one controllable box, a floor, and a crate spawned on demand.
// It exists to make the input actually change the simulation, otherwise the
// rollback tests would prove nothing.
// ---------------------------------------------------------------------------

struct Game {
    dai_body player = DAI_INVALID_BODY;
    int      spawned = 0;
    int      sounds  = 0;
};

static void tick_cb(dai_world *w, dai_tick t, void *user) {
    Game *g = (Game *)user;
    dai_input in{};
    dai_get_input(w, 0, t, &in);

    if (g->player != DAI_INVALID_BODY && (in.axis[0] != 0.0f || in.axis[1] != 0.0f)) {
        dai_vec3 imp{ in.axis[0] * 40.0f, 0.0f, in.axis[1] * 40.0f };
        dai_body_add_impulse(w, g->player, imp);
    }
    // button 0 spawns a crate above the player - a world mutation inside the
    // callback, which is the case a rollback has to get right
    if (in.buttons & 1u) {
        dai_body_desc d{};
        d.shape = DAI_SHAPE_BOX; d.motion = DAI_DYNAMIC;
        d.half_extent = { 0.4f, 0.4f, 0.4f };
        d.position = { 0.0f, 6.0f + 0.1f * (float)(t % 7), 0.0f };
        d.rotation = { 0, 0, 0, 1 };
        d.user_data = 42;
        dai_body_create(w, &d);
        g->spawned++;
        dai_play(w, "blip", d.position, 1);
        g->sounds++;
    }
}

static int g_backend = DAI_PHYSICS_TALOS;

static dai_world *make_world(Game *g, uint64_t seed = 1234, uint32_t hz = 60, const char *bank = nullptr) {
    dai_config cfg{};
    cfg.backend = g_backend;
    cfg.tick_hz = hz;
    cfg.max_bodies = 2048;
    cfg.physics_threads = 3;
    cfg.snapshot_ring = 128;
    cfg.seed = seed;
    cfg.audio_bank = bank;
    cfg.asset_root = "/root/projects/aulos/assets";
    cfg.enable_audio_device = 0;

    dai_world *w = nullptr;
    if (dai_create(&cfg, &w) != DAI_OK) return nullptr;

    // floor
    dai_body_desc f{};
    f.shape = DAI_SHAPE_BOX; f.motion = DAI_STATIC;
    f.half_extent = { 100.0f, 0.5f, 100.0f };
    f.position = { 0.0f, -0.5f, 0.0f };
    f.rotation = { 0, 0, 0, 1 };
    f.friction_static = 0.6f;
    dai_body_create(w, &f);

    // player box
    dai_body_desc p{};
    p.shape = DAI_SHAPE_BOX; p.motion = DAI_DYNAMIC;
    p.half_extent = { 0.5f, 0.5f, 0.5f };
    p.position = { 0.0f, 0.5f, 0.0f };
    p.rotation = { 0, 0, 0, 1 };
    p.no_sleeping = 1;
    p.user_data = 7;
    g->player = dai_body_create(w, &p);

    // a small pile so the solver has real work
    for (int i = 0; i < 40; ++i) {
        dai_body_desc d{};
        d.shape = DAI_SHAPE_BOX; d.motion = DAI_DYNAMIC;
        d.half_extent = { 0.3f, 0.3f, 0.3f };
        d.position = { 2.0f + 0.7f * (float)(i % 6), 0.4f + 0.7f * (float)(i / 6), 1.0f };
        d.rotation = { 0, 0, 0, 1 };
        dai_body_create(w, &d);
    }
    dai_set_tick_callback(w, tick_cb, g);
    return w;
}

// ---------------------------------------------------------------------------

static void test_determinism() {
    std::printf("\n[1] Zwei Welten, gleiche Eingaben -> gleicher Zustand\n");
    Game ga, gb;
    dai_world *a = make_world(&ga), *b = make_world(&gb);
    CHECK(a && b, "world creation failed");
    if (!a || !b) return;

    for (dai_tick t = 0; t < 300; ++t) {
        dai_input in{};
        in.axis[0] = ((t / 7) % 3) - 1.0f;
        in.axis[1] = sinf((float)t * 0.13f);
        in.buttons = (t % 47 == 0) ? 1u : 0u;
        dai_set_input(a, 0, t, &in);
        dai_set_input(b, 0, t, &in);
    }
    bool same = true;
    uint64_t last = 0;
    for (int i = 0; i < 300; ++i) {
        dai_step(a); dai_step(b);
        uint64_t ca = dai_checksum(a), cb = dai_checksum(b);
        if (ca != cb) { same = false; std::printf("  divergiert bei Tick %d: %016llx != %016llx\n", i, (unsigned long long)ca, (unsigned long long)cb); break; }
        last = ca;
    }
    CHECK(same, "worlds diverged");
    std::printf("  300 Ticks, Checksumme %016llx, %d Kisten gespawnt\n", (unsigned long long)last, ga.spawned);
    CHECK(ga.spawned == gb.spawned && ga.spawned > 0, "spawn count %d vs %d", ga.spawned, gb.spawned);
    dai_destroy(a); dai_destroy(b);
}

static void test_rollback_identity() {
    std::printf("\n[2] Rollback ohne Eingabeaenderung muss exakt dasselbe Ergebnis liefern\n");
    Game g;
    dai_world *w = make_world(&g);
    if (!w) { CHECK(false, "no world"); return; }
    for (dai_tick t = 0; t < 200; ++t) {
        dai_input in{}; in.axis[0] = ((t / 5) % 3) - 1.0f;
        in.buttons = (t % 37 == 0) ? 1u : 0u;
        dai_set_input(w, 0, t, &in);
    }
    for (int i = 0; i < 200; ++i) dai_step(w);
    uint64_t before = dai_checksum(w);
    dai_tick at = dai_current_tick(w);

    dai_result r = dai_rollback_to(w, at - 100);
    CHECK(r == DAI_OK, "rollback failed: %s", dai_last_error(w));
    CHECK(dai_current_tick(w) == at, "tick not restored: %llu vs %llu",
          (unsigned long long)dai_current_tick(w), (unsigned long long)at);
    uint64_t after = dai_checksum(w);
    CHECK(before == after, "checksum changed: %016llx -> %016llx",
          (unsigned long long)before, (unsigned long long)after);
    std::printf("  100 Ticks zurueck und neu simuliert: %016llx\n", (unsigned long long)after);

    dai_stats st{}; dai_get_stats(w, &st);
    std::printf("  resimuliert: %llu Ticks, %u Rollbacks\n",
        (unsigned long long)st.ticks_resimulated, st.rollbacks);
    dai_destroy(w);
}

static void test_rollback_correction() {
    std::printf("\n[3] Rollback mit korrigierter Eingabe == Simulation, die sie von Anfang an hatte\n");
    const dai_tick LATE = 80, END = 200;
    dai_input corr{}; corr.axis[0] = 1.0f; corr.axis[1] = -0.5f; corr.buttons = 1u;

    // A: simulates with a wrong prediction, then gets the real input late
    Game ga; dai_world *a = make_world(&ga);
    // B: has the correct input from the start
    Game gb; dai_world *b = make_world(&gb);
    if (!a || !b) { CHECK(false, "no world"); return; }

    for (dai_tick t = 0; t < END; ++t) {
        dai_input in{}; in.axis[0] = ((t / 9) % 3) - 1.0f;
        dai_set_input(a, 0, t, &in);
        if (t == LATE) dai_set_input(b, 0, t, &corr); else dai_set_input(b, 0, t, &in);
    }
    for (int i = 0; i < (int)END; ++i) dai_step(a);
    for (int i = 0; i < (int)END; ++i) dai_step(b);

    uint64_t a_before = dai_checksum(a), b_final = dai_checksum(b);
    int resim = dai_apply_remote_input(a, 0, LATE, &corr);
    uint64_t a_after = dai_checksum(a);

    CHECK(resim == (int)(END - LATE), "resimulated %d ticks, expected %d", resim, (int)(END - LATE));
    CHECK(a_before != a_after, "correction had no effect at all");
    CHECK(a_after == b_final, "rollback result %016llx != from-scratch %016llx",
          (unsigned long long)a_after, (unsigned long long)b_final);
    std::printf("  nach Korrektur: %016llx | von Anfang an: %016llx\n",
        (unsigned long long)a_after, (unsigned long long)b_final);
    std::printf("  Kisten A=%d B=%d\n", ga.spawned, gb.spawned);
    dai_destroy(a); dai_destroy(b);
}

static void test_body_lifetime_rollback() {
    std::printf("\n[4] Bodies, die im zurueckgerollten Fenster entstehen/sterben\n");
    Game g; dai_world *w = make_world(&g);
    if (!w) { CHECK(false, "no world"); return; }
    for (int i = 0; i < 50; ++i) dai_step(w);

    dai_stats s0{}; dai_get_stats(w, &s0);
    uint64_t c0 = dai_checksum(w);
    dai_tick t0 = dai_current_tick(w);

    // spawn 5 crates from OUTSIDE the callback (setup style command), then
    // destroy 2 of them, then roll back past all of it
    std::vector<dai_body> made;
    for (int i = 0; i < 5; ++i) {
        dai_body_desc d{};
        d.shape = DAI_SHAPE_SPHERE; d.motion = DAI_DYNAMIC;
        d.half_extent = { 0.35f, 0, 0 };
        d.position = { -3.0f, 2.0f + (float)i, 0.0f };
        d.rotation = { 0, 0, 0, 1 };
        made.push_back(dai_body_create(w, &d));
    }
    for (int i = 0; i < 10; ++i) dai_step(w);
    dai_body_destroy(w, made[1]);
    dai_body_destroy(w, made[3]);
    for (int i = 0; i < 10; ++i) dai_step(w);

    dai_stats s1{}; dai_get_stats(w, &s1);
    CHECK(s1.bodies == s0.bodies + 3, "expected +3 bodies, got %u -> %u", s0.bodies, s1.bodies);

    dai_result r = dai_rollback_to(w, t0);
    CHECK(r == DAI_OK, "rollback failed: %s", dai_last_error(w));
    dai_stats s2{}; dai_get_stats(w, &s2);
    std::printf("  Bodies: vorher %u, nach Spawn/Destroy %u, nach Rollback %u\n", s0.bodies, s1.bodies, s2.bodies);
    CHECK(s2.bodies == s1.bodies, "body count changed by the replay: %u vs %u", s1.bodies, s2.bodies);
    CHECK(dai_current_tick(w) == t0 + 20, "tick wrong");

    // rolling back to t0 and replaying must reproduce s1 exactly
    uint64_t c1 = dai_checksum(w);
    dai_rollback_to(w, t0);
    CHECK(dai_checksum(w) == c1, "second rollback gave a different result");
    (void)c0;
    dai_destroy(w);
}

static void test_audio_cancel() {
    std::printf("\n[5] Rollback storniert Geraeusche, die nie passiert sind\n");
    Game g; dai_world *w = make_world(&g);
    if (!w) { CHECK(false, "no world"); return; }
    for (dai_tick t = 0; t < 100; ++t) {
        dai_input in{}; in.buttons = (t >= 60 && t % 5 == 0) ? 1u : 0u;
        dai_set_input(w, 0, t, &in);
    }
    for (int i = 0; i < 60; ++i) dai_step(w);
    uint32_t before = dai_poll_audio(w, nullptr, 0);
    for (int i = 0; i < 40; ++i) dai_step(w);
    uint32_t queued = dai_poll_audio(w, nullptr, 0);
    std::printf("  Events vor Tick 60: %u, danach in der Warteschlange: %u\n", before, queued);
    CHECK(queued > 0, "no audio events queued at all");

    dai_rollback_to(w, 60);
    uint32_t after = dai_poll_audio(w, nullptr, 0);
    // the replay re-emits them, so the count has to match again
    std::printf("  nach Rollback+Replay: %u\n", after);
    CHECK(after == queued, "audio events not reproduced by replay: %u vs %u", after, queued);
    dai_destroy(w);
}

static void test_stiction() {
    std::printf("\n[6] Haft- und Gleitreibung durch die Engine hindurch\n");
    // slope emulated by tilted gravity, same trick as the Jolt experiments
    auto run = [](float deg, float mus, float muk, int steps) {
        dai_config cfg{}; cfg.tick_hz = 60; cfg.max_bodies = 64; cfg.physics_threads = 1; cfg.seed = 7;
        dai_world *w = nullptr; dai_create(&cfg, &w);
        dai_body_desc f{}; f.shape = DAI_SHAPE_BOX; f.motion = DAI_STATIC;
        f.half_extent = { 500, 0.5f, 500 }; f.position = { 0, -0.5f, 0 }; f.rotation = { 0,0,0,1 };
        f.friction_static = mus; f.friction_kinetic = muk;
        dai_body_create(w, &f);
        dai_body_desc b{}; b.shape = DAI_SHAPE_BOX; b.motion = DAI_DYNAMIC;
        b.half_extent = { 0.5f, 0.5f, 0.5f }; b.position = { 0, 0.5f, 0 }; b.rotation = { 0,0,0,1 };
        b.friction_static = mus; b.friction_kinetic = muk; b.no_sleeping = 1;
        dai_body body = dai_body_create(w, &b);
        for (int i = 0; i < 60; ++i) dai_step(w);          // settle
        float th = deg * 3.14159265f / 180.0f;
        dai_set_gravity(w, dai_vec3{ 9.81f * sinf(th), -9.81f * cosf(th), 0 });
        dai_transform t0{}; dai_body_get(w, body, &t0);
        for (int i = 0; i < steps; ++i) dai_step(w);
        dai_transform t1{}; dai_body_get(w, body, &t1);
        dai_destroy(w);
        return t1.position.x - t0.position.x;
    };
    float hold  = run(26.0f, 0.5f, 0.5f, 300);
    float slide = run(30.0f, 0.5f, 0.5f, 300);
    std::printf("  mu=0.5: 26 Grad -> %.6f m, 30 Grad -> %.3f m (kritisch: 26.57)\n", hold, slide);
    CHECK(fabsf(hold) < 1e-3f, "box slid at 26 deg: %f", hold);
    CHECK(slide > 5.0f, "box did not slide at 30 deg: %f", slide);

    float st_hold  = run(30.0f, 0.8f, 0.35f, 300);
    float st_slide = run(40.0f, 0.8f, 0.35f, 300);
    std::printf("  Haft 0.8 / Gleit 0.35: 30 Grad -> %.6f m, 40 Grad -> %.3f m (kritisch: 38.66)\n", st_hold, st_slide);
    CHECK(fabsf(st_hold) < 1e-2f, "stiction failed to hold at 30 deg: %f", st_hold);
    CHECK(st_slide > 5.0f, "did not slide at 40 deg: %f", st_slide);
}

static void test_interpolation_and_stats() {
    std::printf("\n[7] Interpolation und Zeitschritt-Akkumulator\n");
    Game g; dai_world *w = make_world(&g);
    if (!w) { CHECK(false, "no world"); return; }
    float alpha = -1.0f;
    uint32_t n = dai_advance(w, 0.05, &alpha);        // 50 ms bei 60 Hz -> 3 Ticks
    CHECK(n == 3, "advance ran %u ticks, expected 3", n);
    CHECK(alpha >= 0.0f && alpha < 1.0f, "alpha out of range: %f", alpha);
    std::printf("  50 ms -> %u Ticks, alpha=%.4f\n", n, alpha);

    std::vector<dai_transform> tr(256);
    uint32_t got0 = dai_get_transforms(w, tr.data(), 256, 0.0f);
    std::vector<dai_transform> tr1(256);
    uint32_t got1 = dai_get_transforms(w, tr1.data(), 256, 1.0f);
    CHECK(got0 == got1 && got0 > 0, "transform count mismatch");
    bool moved = false;
    for (uint32_t i = 0; i < got0; ++i)
        if (fabsf(tr[i].position.y - tr1[i].position.y) > 1e-7f) { moved = true; break; }
    CHECK(moved, "interpolation produced identical results for alpha 0 and 1");

    uint32_t big = dai_advance(w, 10.0, nullptr);     // hitch: must not death spiral
    CHECK(big <= 8, "advance ran %u ticks after a 10 s hitch (cap is 8)", big);
    std::printf("  10 s Hitch -> nur %u Ticks (Death-Spiral-Schutz)\n", big);
    dai_destroy(w);
}

static void test_perf() {
    std::printf("\n[8] Kosten\n");
    Game g; dai_world *w = make_world(&g);
    if (!w) { CHECK(false, "no world"); return; }
    for (int i = 0; i < 60; ++i) dai_step(w);
    auto t0 = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < 600; ++i) dai_step(w);
    double ms = std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - t0).count();
    dai_stats st{}; dai_get_stats(w, &st);
    std::printf("  600 Ticks in %.1f ms -> %.4f ms/Tick, %u Bodies (%u aktiv)\n",
        ms, ms / 600.0, st.bodies, st.active_bodies);

    // what a rollback costs
    auto r0 = std::chrono::high_resolution_clock::now();
    dai_rollback_to(w, dai_current_tick(w) - 60);
    double rms = std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - r0).count();
    std::printf("  Rollback ueber 60 Ticks: %.2f ms (%.4f ms pro nachsimuliertem Tick)\n", rms, rms / 60.0);
    CHECK(rms < 2000.0, "rollback took %f ms", rms);
    dai_destroy(w);
}

// The leak test: everything the engine does must also work on a backend that
// knows nothing about Jolt. If this section stops compiling or stops passing,
// a physics detail has escaped dai_physics.hpp.
static void test_null_backend() {
    std::printf("\n[9] Backend-Wechsel: dieselbe Engine auf dem Null-Backend\n");
    g_backend = DAI_PHYSICS_NULL;

    Game ga, gb;
    dai_world *a = make_world(&ga), *b = make_world(&gb);
    if (!a || !b) { CHECK(false, "no world"); g_backend = DAI_PHYSICS_TALOS; return; }
    std::printf("  Backend: %s\n", dai_backend_name(a));
    CHECK(std::strcmp(dai_backend_name(a), "null") == 0, "wrong backend: %s", dai_backend_name(a));

    for (dai_tick t = 0; t < 150; ++t) {
        dai_input in{};
        in.axis[0] = ((t / 7) % 3) - 1.0f;
        in.buttons = (t % 31 == 0) ? 1u : 0u;
        dai_set_input(a, 0, t, &in);
        dai_set_input(b, 0, t, &in);
    }
    bool same = true;
    for (int i = 0; i < 150; ++i) {
        dai_step(a); dai_step(b);
        if (dai_checksum(a) != dai_checksum(b)) { same = false; break; }
    }
    CHECK(same, "null backend diverged");

    // rollback has to work here too - it is engine logic, not physics logic
    uint64_t before = dai_checksum(a);
    dai_tick at = dai_current_tick(a);
    dai_result r = dai_rollback_to(a, at - 50);
    CHECK(r == DAI_OK, "rollback on null backend failed: %s", dai_last_error(a));
    CHECK(dai_checksum(a) == before, "null rollback changed the state");
    std::printf("  150 Ticks + Rollback ueber 50: %016llx\n", (unsigned long long)dai_checksum(a));

    // queries must exist on both backends
    dai_ray_hit hit{};
    int got = dai_raycast(a, dai_vec3{ 0, 5, 0 }, dai_vec3{ 0, -1, 0 }, 20.0f, &hit);
    std::printf("  Raycast nach unten: %s, Distanz %.3f\n", got ? "Treffer" : "kein Treffer", hit.distance);
    CHECK(got == 1, "raycast found nothing on the null backend");

    dai_destroy(a); dai_destroy(b);
    g_backend = DAI_PHYSICS_TALOS;
}

static void test_queries_talos() {
    std::printf("\n[10] Raycast und Kontakte auf dem Talos-Backend\n");
    Game g; dai_world *w = make_world(&g);
    if (!w) { CHECK(false, "no world"); return; }
    for (int i = 0; i < 5; ++i) dai_step(w);   // few ticks: the pile must still be awake

    dai_ray_hit hit{};
    int got = dai_raycast(w, dai_vec3{ 0, 8, 0 }, dai_vec3{ 0, -1, 0 }, 30.0f, &hit);
    std::printf("  Strahl von oben: %s, Distanz %.3f, Normale (%.2f %.2f %.2f)\n",
        got ? "Treffer" : "nichts", hit.distance, hit.normal.x, hit.normal.y, hit.normal.z);
    CHECK(got == 1, "raycast hit nothing");
    CHECK(hit.normal.y > 0.5f, "surface normal points the wrong way: %f", hit.normal.y);

    uint32_t n = dai_poll_contacts(w, nullptr, 0);
    std::vector<dai_contact> cs(n ? n : 1);
    dai_poll_contacts(w, cs.data(), (uint32_t)cs.size());
    std::printf("  Kontakte im letzten Tick: %u\n", n);
    CHECK(n > 0, "no contacts reported although bodies rest on the floor");
    dai_destroy(w);

    // ---- how hard was the hit --------------------------------------------
    // A sphere dropped from a known height has a known impact impulse:
    //   v = sqrt(2gh),  j = (1 + e) * m * v
    // The check is against that number, not against "greater than zero" -
    // an impulse that is merely non zero can still be nonsense.
    {
        dai_config cfg{};
        cfg.backend = DAI_PHYSICS_TALOS;
        cfg.tick_hz = 240;                 // fine ticks: less overshoot into the floor
        cfg.max_bodies = 16; cfg.physics_threads = 1; cfg.snapshot_ring = 8; cfg.seed = 5;
        dai_world *iw = nullptr;
        if (dai_create(&cfg, &iw) != DAI_OK) { CHECK(false, "impulse world"); return; }

        dai_body_desc floor{};
        floor.shape = DAI_SHAPE_BOX; floor.motion = DAI_STATIC;
        floor.half_extent = { 20, 0.5f, 20 };
        floor.position = { 0, -0.5f, 0 };
        floor.rotation = { 0, 0, 0, 1 };
        dai_body_create(iw, &floor);

        const float drop = 2.0f, radius = 0.5f, density = 1000.0f;
        dai_body_desc ball{};
        ball.shape = DAI_SHAPE_SPHERE; ball.motion = DAI_DYNAMIC;
        ball.half_extent = { radius, radius, radius };
        ball.position = { 0, drop + radius, 0 };
        ball.rotation = { 0, 0, 0, 1 };
        ball.density = density;
        ball.restitution = 0.0f;           // no bounce: one clean impact
        dai_body_create(iw, &ball);

        float mass = density * (4.0f / 3.0f) * 3.14159265f * radius * radius * radius;
        float v = std::sqrt(2.0f * 9.81f * drop);
        float expect = mass * v;

        float peak = 0.0f;
        for (int i = 0; i < 240 && peak == 0.0f; ++i) {
            dai_step(iw);
            uint32_t cn = dai_poll_contacts(iw, nullptr, 0);
            if (!cn) continue;
            std::vector<dai_contact> cc(cn);
            dai_poll_contacts(iw, cc.data(), cn);
            for (const dai_contact &c : cc) if (c.impulse > peak) peak = c.impulse;
        }
        std::printf("  Aufprall: erwartet ~%.0f Ns, gemessen %.0f Ns (Masse %.1f kg, v %.2f m/s)\n",
                    expect, peak, mass, v);
        CHECK(peak > 0.0f, "the impact impulse is still zero - nothing was computed");
        CHECK(peak > expect * 0.7f && peak < expect * 1.3f,
              "impact impulse %.1f Ns is not within 30%% of the textbook %.1f Ns", peak, expect);

        // Resting is not impact: after it has settled, the contact is still
        // reported but carries almost nothing.
        for (int i = 0; i < 240; ++i) dai_step(iw);
        uint32_t rn = dai_poll_contacts(iw, nullptr, 0);
        std::vector<dai_contact> rc(rn ? rn : 1);
        dai_poll_contacts(iw, rc.data(), (uint32_t)rc.size());
        float resting = 0.0f;
        for (uint32_t i = 0; i < rn; ++i) if (rc[i].impulse > resting) resting = rc[i].impulse;
        std::printf("  danach in Ruhe: %.2f Ns\n", resting);
        CHECK(resting < expect * 0.1f,
              "a resting body reports %.1f Ns - impact and load are being confused", resting);
        dai_destroy(iw);
    }
}

// ---------------------------------------------------------------------------
// Joints: the bearing and the piston, i.e. what a construction game is made of
// ---------------------------------------------------------------------------

static dai_world *joint_world() {
    dai_config cfg{};
    cfg.backend = g_backend;
    cfg.tick_hz = 60; cfg.max_bodies = 128; cfg.physics_threads = 1;
    cfg.snapshot_ring = 128; cfg.seed = 4242;
    dai_world *w = nullptr;
    if (dai_create(&cfg, &w) != DAI_OK) return nullptr;
    dai_body_desc f{};
    f.shape = DAI_SHAPE_BOX; f.motion = DAI_STATIC;
    f.half_extent = { 100, 0.5f, 100 }; f.position = { 0, -0.5f, 0 }; f.rotation = { 0,0,0,1 };
    dai_body_create(w, &f);
    return w;
}

static void test_hinge_motor() {
    std::printf("\n[11] Lager (Hinge) mit Motor\n");
    dai_world *w = joint_world();
    if (!w) { CHECK(false, "no world"); return; }

    dai_body_desc arm{};
    arm.shape = DAI_SHAPE_BOX; arm.motion = DAI_DYNAMIC;
    arm.half_extent = { 1.5f, 0.15f, 0.15f };
    // Pivot 6 m up so the 3 m arm swings freely. At 3 m its tip drags across
    // the floor and the test measures the torque needed to tear a resting beam
    // loose, not the motor.
    arm.position = { 1.5f, 6.0f, 0 }; arm.rotation = { 0,0,0,1 };
    arm.no_sleeping = 1; arm.density = 500.0f;
    dai_body a = dai_body_create(w, &arm);

    dai_joint_desc jd{};
    jd.type = DAI_JOINT_HINGE;
    jd.a = a; jd.b = DAI_INVALID_BODY;               // anchored to the world
    jd.anchor = { 0, 6.0f, 0 };
    jd.axis = { 0, 0, 1 };                            // rotates in the XY plane
    jd.normal_axis = { 1, 0, 0 };
    jd.max_motor_force = 40000.0f;   // 135 kg arm: 4000 Nm stalls, measured
    dai_joint j = dai_joint_create(w, &jd);
    CHECK(j != DAI_INVALID_JOINT, "hinge could not be created: %s", dai_last_error(w));
    CHECK(dai_joint_count(w) == 1, "joint count is %u", dai_joint_count(w));

    // without a motor the arm has to fall
    for (int i = 0; i < 30; ++i) dai_step(w);
    dai_joint_state st{}; dai_joint_get(w, j, &st);
    std::printf("  ohne Motor nach 0.5 s: Winkel %.3f rad\n", st.position);
    CHECK(fabsf(st.position) > 0.05f, "arm did not swing down: %f", st.position);

    // motor holds it and drives it back up
    dai_joint_set_motor(w, j, DAI_MOTOR_VELOCITY, 2.0f);
    for (int i = 0; i < 60; ++i) dai_step(w);
    dai_joint_state st2{}; dai_joint_get(w, j, &st2);
    std::printf("  Motor 2 rad/s, 1 s spaeter: Winkel %.3f rad, Drehzahl %.3f rad/s\n", st2.position, st2.speed);
    CHECK(st2.position > st.position, "motor did not turn the arm: %f -> %f", st.position, st2.position);
    CHECK(fabsf(fabsf(st2.speed) - 2.0f) < 0.6f, "motor speed off target: %f", st2.speed);

    // limits
    dai_joint_set_motor(w, j, DAI_MOTOR_OFF, 0.0f);
    dai_destroy(w);
}

static void test_slider_piston() {
    std::printf("\n[12] Kolben (Slider) mit Grenzen und Motor\n");
    dai_world *w = joint_world();
    if (!w) { CHECK(false, "no world"); return; }

    dai_body_desc b{};
    b.shape = DAI_SHAPE_BOX; b.motion = DAI_DYNAMIC;
    b.half_extent = { 0.4f, 0.4f, 0.4f };
    b.position = { 0, 3.0f, 0 }; b.rotation = { 0,0,0,1 };
    b.no_sleeping = 1; b.density = 500.0f;
    dai_body body = dai_body_create(w, &b);

    dai_joint_desc jd{};
    jd.type = DAI_JOINT_SLIDER;
    jd.a = body; jd.b = DAI_INVALID_BODY;
    jd.anchor = { 0, 3.0f, 0 };
    jd.axis = { 1, 0, 0 };
    jd.normal_axis = { 0, 1, 0 };
    jd.enable_limits = 1; jd.limit_min = -0.5f; jd.limit_max = 1.5f;
    jd.max_motor_force = 20000.0f;
    dai_joint j = dai_joint_create(w, &jd);
    CHECK(j != DAI_INVALID_JOINT, "slider could not be created: %s", dai_last_error(w));

    dai_joint_set_motor(w, j, DAI_MOTOR_VELOCITY, 1.0f);
    for (int i = 0; i < 60; ++i) dai_step(w);
    dai_joint_state st{}; dai_joint_get(w, j, &st);
    dai_transform tr{}; dai_body_get(w, body, &tr);
    std::printf("  nach 1 s bei 1 m/s: Position %.3f m (x=%.3f)\n", st.position, tr.position.x);
    CHECK(st.position > 0.7f, "piston did not extend: %f", st.position);

    // drive into the limit and stay there
    for (int i = 0; i < 120; ++i) dai_step(w);
    dai_joint_get(w, j, &st);
    std::printf("  gegen die Grenze gefahren (max 1.5): Position %.3f m\n", st.position);
    CHECK(st.position < 1.6f, "piston blew through its limit: %f", st.position);
    CHECK(st.position > 1.35f, "piston did not reach its limit: %f", st.position);
    dai_destroy(w);
}

static void test_joint_rollback() {
    std::printf("\n[13] Gelenke ueberleben einen Rollback\n");
    dai_world *w = joint_world();
    if (!w) { CHECK(false, "no world"); return; }

    dai_body_desc arm{};
    arm.shape = DAI_SHAPE_BOX; arm.motion = DAI_DYNAMIC;
    arm.half_extent = { 1.0f, 0.15f, 0.15f };
    arm.position = { 1.0f, 3.0f, 0 }; arm.rotation = { 0,0,0,1 };
    arm.no_sleeping = 1;
    dai_body a = dai_body_create(w, &arm);

    dai_joint_desc jd{};
    jd.type = DAI_JOINT_HINGE;
    jd.a = a; jd.anchor = { 0, 3.0f, 0 };
    jd.axis = { 0, 0, 1 }; jd.normal_axis = { 1, 0, 0 };
    jd.max_motor_force = 2000.0f;
    dai_joint j = dai_joint_create(w, &jd);
    dai_joint_set_motor(w, j, DAI_MOTOR_VELOCITY, 1.5f);

    for (int i = 0; i < 40; ++i) dai_step(w);
    dai_tick mark = dai_current_tick(w);

    // a second joint and a second body, both created inside the window
    dai_body_desc b2{};
    b2.shape = DAI_SHAPE_SPHERE; b2.motion = DAI_DYNAMIC;
    b2.half_extent = { 0.3f, 0, 0 }; b2.position = { -2.0f, 4.0f, 0 }; b2.rotation = { 0,0,0,1 };
    dai_body bb = dai_body_create(w, &b2);
    dai_joint_desc jd2{};
    jd2.type = DAI_JOINT_DISTANCE;
    jd2.a = bb; jd2.anchor = { -2.0f, 4.0f, 0 }; jd2.normal_axis = { -2.0f, 6.0f, 0 };
    jd2.min_distance = 0.0f; jd2.max_distance = 2.0f;
    dai_joint j2 = dai_joint_create(w, &jd2);
    CHECK(j2 != DAI_INVALID_JOINT, "distance joint failed: %s", dai_last_error(w));

    for (int i = 0; i < 30; ++i) dai_step(w);
    uint32_t before_joints = dai_joint_count(w);
    uint64_t before = dai_checksum(w);

    dai_result r = dai_rollback_to(w, mark);
    CHECK(r == DAI_OK, "rollback with joints failed: %s", dai_last_error(w));
    CHECK(dai_joint_count(w) == before_joints, "joint count changed: %u -> %u", before_joints, dai_joint_count(w));
    uint64_t after = dai_checksum(w);
    std::printf("  vor Rollback %016llx, danach %016llx, Gelenke %u\n",
        (unsigned long long)before, (unsigned long long)after, dai_joint_count(w));
    CHECK(before == after, "joint state not reproduced by the replay");

    // rolling back past the creation of joint 2 must remove it again
    dai_rollback_to(w, mark);
    dai_joint_state js{};
    CHECK(dai_joint_get(w, j, &js) == DAI_OK, "the old joint got lost");
    std::printf("  Lagerwinkel nach zwei Rollbacks: %.4f rad\n", js.position);
    dai_destroy(w);
}

int main() {
    std::printf("%s\n", dai_version());
    test_determinism();
    test_rollback_identity();
    test_rollback_correction();
    test_body_lifetime_rollback();
    test_audio_cancel();
    test_stiction();
    test_interpolation_and_stats();
    test_perf();
    test_null_backend();
    test_queries_talos();
    test_hinge_motor();
    test_slider_piston();
    test_joint_rollback();
    std::printf("\n==================================\n");
    std::printf("  %d bestanden, %d fehlgeschlagen\n", g_pass, g_fail);
    std::printf("==================================\n");
    return g_fail == 0 ? 0 : 1;
}
