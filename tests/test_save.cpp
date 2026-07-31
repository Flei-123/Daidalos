// Save files.
//
// The property that matters: loading a save and stepping it must produce the
// same simulation as never having saved at all. Anything less and a player's
// game changes because they quit and came back.
//
//   ./build/test_save [/tmp]

#include "daidalos.h"
#include <cstdio>
#include <cmath>
#include <string>
#include <vector>

static int g_fail = 0, g_pass = 0;
#define CHECK(cond, ...) do { \
    if (cond) { ++g_pass; } \
    else { ++g_fail; std::printf("  FAIL "); std::printf(__VA_ARGS__); std::printf("\n"); } \
} while (0)

static dai_world *build_world(uint64_t seed) {
    dai_config cfg{};
    cfg.tick_hz = 60; cfg.max_bodies = 256; cfg.physics_threads = 1; cfg.seed = seed;
    dai_world *w = nullptr;
    if (dai_create(&cfg, &w) != DAI_OK) return nullptr;

    dai_body_desc f{};
    f.shape = DAI_SHAPE_BOX; f.motion = DAI_STATIC;
    f.half_extent = { 50, 1, 50 }; f.position = { 0, -1, 0 }; f.rotation = { 0,0,0,1 };
    dai_body_create(w, &f);

    for (int i = 0; i < 24; ++i) {
        dai_body_desc d{};
        d.shape = (i % 3 == 0) ? DAI_SHAPE_SPHERE : DAI_SHAPE_BOX;
        d.motion = DAI_DYNAMIC;
        d.half_extent = { 0.4f, 0.4f, 0.4f };
        d.position = { -3.0f + (i % 6) * 1.05f, 1.0f + (i / 6) * 1.1f, (float)(i % 4) * 0.6f };
        d.rotation = { 0,0,0,1 };
        d.density = 400.0f + i * 10.0f;
        d.restitution = 0.2f;
        d.user_data = 100 + i;
        dai_body_create(w, &d);
    }
    // one compound, because parts are the part of the format most likely to rot
    dai_compound_part parts[3]{};
    for (int i = 0; i < 3; ++i) {
        parts[i].shape = DAI_SHAPE_BOX;
        parts[i].half_extent = { 0.3f, 0.3f, 0.3f };
        parts[i].offset = { (float)i * 0.62f, 0, 0 };
        parts[i].rotation = { 0,0,0,1 };
    }
    dai_body_desc cd{};
    cd.shape = DAI_SHAPE_COMPOUND; cd.motion = DAI_DYNAMIC;
    cd.position = { 4, 3, 0 }; cd.rotation = { 0,0,0,1 };
    cd.parts = parts; cd.part_count = 3; cd.user_data = 999;
    dai_body_create(w, &cd);
    return w;
}

int main(int argc, char **argv) {
    std::string dir = argc > 1 ? argv[1] : "/tmp";
    std::string path = dir + "/daidalos_test.save";
    std::printf("save / load (format v%u)\n", dai_save_version());

    dai_world *a = build_world(7);
    CHECK(a != nullptr, "world creation failed");
    if (!a) return 1;
    for (int i = 0; i < 40; ++i) dai_step(a);

    uint64_t checksum_at_save = dai_checksum(a);
    dai_tick tick_at_save = dai_current_tick(a);
    std::vector<dai_transform> before(64);
    uint32_t n_before = dai_get_transforms(a, before.data(), 64, 0.0f);

    CHECK(dai_world_save(a, path.c_str()) == DAI_OK, "saving failed");

    dai_config cfg{};
    cfg.tick_hz = 60; cfg.max_bodies = 256; cfg.physics_threads = 1; cfg.seed = 7;
    dai_world *b = nullptr;
    CHECK(dai_world_load(&cfg, path.c_str(), &b) == DAI_OK, "loading failed");
    if (!b) return 1;

    CHECK(dai_current_tick(b) == tick_at_save, "tick is %llu after loading, saved at %llu",
          (unsigned long long)dai_current_tick(b), (unsigned long long)tick_at_save);

    std::vector<dai_transform> after(64);
    uint32_t n_after = dai_get_transforms(b, after.data(), 64, 0.0f);
    CHECK(n_after == n_before, "loaded %u bodies, saved %u", n_after, n_before);

    float worst = 0.0f;
    uint32_t bad_handle = 0, bad_user = 0;
    for (uint32_t i = 0; i < n_after && i < n_before; ++i) {
        worst = fmaxf(worst, fabsf(after[i].position.x - before[i].position.x));
        worst = fmaxf(worst, fabsf(after[i].position.y - before[i].position.y));
        worst = fmaxf(worst, fabsf(after[i].position.z - before[i].position.z));
        if (after[i].body != before[i].body) ++bad_handle;
        if (after[i].user_data != before[i].user_data) ++bad_user;
    }
    std::printf("  %u bodies restored, worst position error %.6f m\n", n_after, worst);
    CHECK(worst < 1e-4f, "positions differ by up to %.6f m after a round trip", worst);
    CHECK(bad_handle == 0, "%u body handles changed - saved references would break", bad_handle);
    CHECK(bad_user == 0, "%u user_data values were lost", bad_user);
    CHECK(dai_body_part_count(b, after[n_after-1].body) == 3 ||
          dai_body_part_count(b, before[n_before-1].body) == 3,
          "the compound body lost its parts");

    // the real test: both worlds must now simulate the same future
    uint64_t ca = 0, cb = 0;
    for (int i = 0; i < 120; ++i) { dai_step(a); dai_step(b); }
    ca = dai_checksum(a); cb = dai_checksum(b);
    std::printf("  after 120 more ticks: original %016llx | loaded %016llx\n",
                (unsigned long long)ca, (unsigned long long)cb);
    CHECK(ca == cb, "the loaded world drifted away from the original");

    // a truncated or foreign file must be refused, not crash
    FILE *junk = std::fopen((dir + "/junk.save").c_str(), "wb");
    if (junk) { std::fwrite("NOTASAVE", 1, 8, junk); std::fclose(junk); }
    dai_world *c = nullptr;
    CHECK(dai_world_load(&cfg, (dir + "/junk.save").c_str(), &c) != DAI_OK && c == nullptr,
          "a junk file was accepted as a save");
    CHECK(dai_world_load(&cfg, (dir + "/does_not_exist.save").c_str(), &c) != DAI_OK,
          "a missing file was accepted as a save");
    (void)checksum_at_save;

    dai_destroy(a);
    dai_destroy(b);
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
