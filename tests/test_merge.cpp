// Merging blocks into one body, and taking them apart again.
//
// The point of merging is performance, so the test measures it. The point of
// splitting is that it must not teleport or invent energy, so the test checks
// momentum. And because both are built out of ordinary create/destroy commands,
// a rollback across a merge has to reproduce bit for bit - which is checked too.
//
//   ./build/test_merge

#include "daidalos.h"
#include <cstdio>
#include <cmath>
#include <vector>

static int g_fail = 0, g_pass = 0;
#define CHECK(cond, ...) do { \
    if (cond) { ++g_pass; } \
    else { ++g_fail; std::printf("  FAIL "); std::printf(__VA_ARGS__); std::printf("\n"); } \
} while (0)

static dai_world *make_world(uint64_t seed = 4) {
    dai_config cfg{};
    cfg.tick_hz = 60; cfg.max_bodies = 4096; cfg.physics_threads = 3; cfg.seed = seed;
    dai_world *w = nullptr;
    if (dai_create(&cfg, &w) != DAI_OK) return nullptr;
    dai_body_desc f{};
    f.shape = DAI_SHAPE_BOX; f.motion = DAI_STATIC;
    f.half_extent = { 200, 1, 200 }; f.position = { 0, -1, 0 }; f.rotation = { 0,0,0,1 };
    dai_body_create(w, &f);
    return w;
}

// a wall of 1 m blocks, the classic "many welded parts" case
static std::vector<dai_body> build_wall(dai_world *w, int nx, int ny, int nz, float x0) {
    std::vector<dai_body> out;
    for (int x = 0; x < nx; ++x)
        for (int y = 0; y < ny; ++y)
            for (int z = 0; z < nz; ++z) {
                dai_body_desc d{};
                d.shape = DAI_SHAPE_BOX; d.motion = DAI_DYNAMIC;
                d.half_extent = { 0.5f, 0.5f, 0.5f };
                d.position = { x0 + x * 1.001f, 0.5f + y * 1.001f, z * 1.001f };
                d.rotation = { 0,0,0,1 };
                d.density = 500.0f;
                d.user_data = 1;
                out.push_back(dai_body_create(w, &d));
            }
    return out;
}

int main() {
    std::printf("merge / split\n");

    // ---- 1. one merged body behaves like the group did
    {
        dai_world *w = make_world();
        std::vector<dai_body> blocks = build_wall(w, 3, 3, 3, 0.0f);
        dai_body m = dai_body_merge(w, blocks.data(), (uint32_t)blocks.size(), 7);
        CHECK(m != DAI_INVALID_BODY, "merge failed: %s", dai_last_error(w));
        CHECK(dai_body_part_count(w, m) == 27, "merged body has %u parts, expected 27", dai_body_part_count(w, m));
        for (dai_body b : blocks) CHECK(!dai_body_valid(w, b), "a source body survived the merge");

        dai_transform t{};
        dai_body_get(w, m, &t);
        CHECK(fabsf(t.position.x - 1.0f) < 0.05f && fabsf(t.position.z - 1.0f) < 0.05f,
              "merged origin is (%.2f %.2f %.2f), expected the centre of the 3x3x3 group", t.position.x, t.position.y, t.position.z);

        for (int i = 0; i < 120; ++i) dai_step(w);
        dai_body_get(w, m, &t);
        CHECK(t.position.y > 1.0f && t.position.y < 1.8f,
              "merged block group settled at y=%.2f, expected ~1.5 (it should rest on the floor)", t.position.y);
        dai_destroy(w);
    }

    // ---- 2. splitting conserves position and momentum
    {
        dai_world *w = make_world();
        std::vector<dai_body> blocks = build_wall(w, 2, 2, 2, 0.0f);
        dai_body m = dai_body_merge(w, blocks.data(), (uint32_t)blocks.size(), 7);
        dai_body_set_velocity(w, m, dai_vec3{ 3.0f, 0, 0 }, dai_vec3{ 0, 2.0f, 0 });
        for (int i = 0; i < 10; ++i) dai_step(w);

        dai_transform before{};
        dai_body_get(w, m, &before);
        dai_vec3 lv{}, av{};
        dai_body_get_velocity(w, m, &lv, &av);

        dai_body pieces[16];
        uint32_t n = dai_body_split(w, m, pieces, 16);
        CHECK(n == 8, "split produced %u bodies, expected 8", n);
        CHECK(!dai_body_valid(w, m), "the compound survived the split");

        // centre of the pieces must still be where the compound was
        dai_vec3 c{ 0, 0, 0 };
        dai_vec3 vsum{ 0, 0, 0 };
        for (uint32_t i = 0; i < n; ++i) {
            dai_transform t{}; dai_body_get(w, pieces[i], &t);
            c.x += t.position.x / n; c.y += t.position.y / n; c.z += t.position.z / n;
            dai_vec3 l{}, a{};
            dai_body_get_velocity(w, pieces[i], &l, &a);
            vsum.x += l.x / n; vsum.y += l.y / n; vsum.z += l.z / n;
        }
        CHECK(fabsf(c.x - before.position.x) < 0.02f && fabsf(c.y - before.position.y) < 0.02f,
              "pieces recentre at (%.3f %.3f), compound was at (%.3f %.3f) - split teleports", c.x, c.y, before.position.x, before.position.y);
        CHECK(fabsf(vsum.x - lv.x) < 0.05f, "average piece velocity %.3f vs compound %.3f - momentum was invented", vsum.x, lv.x);
        dai_destroy(w);
    }

    // ---- 3. merging merged bodies stays one level deep
    {
        dai_world *w = make_world();
        std::vector<dai_body> a = build_wall(w, 2, 2, 1, 0.0f);
        std::vector<dai_body> b = build_wall(w, 2, 2, 1, 10.0f);
        dai_body ma = dai_body_merge(w, a.data(), (uint32_t)a.size(), 1);
        dai_body mb = dai_body_merge(w, b.data(), (uint32_t)b.size(), 1);
        dai_body both[2] = { ma, mb };
        dai_body m = dai_body_merge(w, both, 2, 2);
        CHECK(dai_body_part_count(w, m) == 8, "nested merge has %u parts, expected 8 flattened", dai_body_part_count(w, m));
        uint32_t n = dai_body_split(w, m, nullptr, 0);
        CHECK(n == 8, "splitting a twice merged body gave %u pieces, expected 8", n);
        dai_destroy(w);
    }

    // ---- 4. rollback across a merge reproduces exactly
    //
    // dai_rollback_to rewinds AND re-simulates back to where it started, so
    // the observable contract is: same tick, same checksum, same body set -
    // even though the window contains 12 destroys and a create.
    {
        dai_world *w = make_world(99);
        std::vector<dai_body> blocks = build_wall(w, 3, 2, 2, 0.0f);
        for (int i = 0; i < 20; ++i) dai_step(w);
        dai_tick t0 = dai_current_tick(w);
        uint64_t at_t0 = dai_checksum(w);

        dai_step(w);
        dai_body m = dai_body_merge(w, blocks.data(), (uint32_t)blocks.size(), 7);
        CHECK(m != DAI_INVALID_BODY, "merge inside the rollback window failed");
        for (int i = 0; i < 29; ++i) dai_step(w);
        uint64_t after = dai_checksum(w);
        dai_tick t1 = dai_current_tick(w);
        std::vector<dai_transform> tr(64);
        uint32_t bodies_after = dai_get_transforms(w, tr.data(), 64, 0.0f);

        dai_result rr = dai_rollback_to(w, t0);
        CHECK(rr == DAI_OK, "rollback across a merge failed: %d", (int)rr);
        CHECK(dai_current_tick(w) == t1, "rollback resumed at tick %llu, expected %llu",
              (unsigned long long)dai_current_tick(w), (unsigned long long)t1);
        CHECK(dai_checksum(w) == after,
              "replaying across a merge diverged: %016llx vs %016llx",
              (unsigned long long)dai_checksum(w), (unsigned long long)after);
        CHECK(dai_get_transforms(w, tr.data(), 64, 0.0f) == bodies_after,
              "body count changed across the rollback - the merge was not replayed identically");

        // and a second rollback to the same tick must be a no-op, not drift
        dai_rollback_to(w, t0);
        CHECK(dai_checksum(w) == after, "a second rollback across the merge drifted");
        (void)at_t0;
        dai_destroy(w);
    }

    // ---- 5. what it is all for: the cost
    {
        const int N = 8;                      // 8x8x8 = 512 blocks
        dai_world *w1 = make_world();
        std::vector<dai_body> blocks = build_wall(w1, N, N, N, 0.0f);
        for (int i = 0; i < 30; ++i) dai_step(w1);      // let it settle
        dai_stats s{};
        for (int i = 0; i < 60; ++i) dai_step(w1);
        dai_get_stats(w1, &s);
        double loose = s.avg_step_ms;
        uint32_t loose_bodies = s.bodies;

        dai_world *w2 = make_world();
        std::vector<dai_body> blocks2 = build_wall(w2, N, N, N, 0.0f);
        dai_body merged = dai_body_merge(w2, blocks2.data(), (uint32_t)blocks2.size(), 7);
        for (int i = 0; i < 30; ++i) dai_step(w2);
        dai_stats s2{};
        for (int i = 0; i < 60; ++i) dai_step(w2);
        dai_get_stats(w2, &s2);
        double merged_ms = s2.avg_step_ms;

        std::printf("  %d blocks loose: %.4f ms/tick (%u bodies) | merged: %.4f ms/tick (%u bodies) -> %.1fx cheaper\n",
                    N*N*N, loose, loose_bodies, merged_ms, s2.bodies, merged_ms > 0 ? loose / merged_ms : 0.0);
        CHECK(merged != DAI_INVALID_BODY, "merging %d blocks failed: %s", N*N*N, dai_last_error(w2));
        CHECK(merged_ms < loose * 0.6, "merged is %.4f ms/tick vs %.4f loose - merging is supposed to be much cheaper",
              merged_ms, loose);
        dai_destroy(w1); dai_destroy(w2);
    }

    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
