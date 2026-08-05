// Diagnostic: where does a compound body land after a save/load round trip?
//
// test_save reports a 0.62 m error, which is exactly the spacing of the three
// parts in its compound - the shape of a centre-of-mass offset that is applied
// once on create and a second time on load. This prints every number involved
// so the guess can be replaced by a measurement.
//
//   g++ -std=c++17 -Iinclude -Isrc tools/diag_compound.cpp build/*.o -o build/diag_compound

#include "daidalos.h"
#include <cstdio>
#include <cmath>

int main() {
    dai_config cfg{};
    cfg.tick_hz = 60; cfg.max_bodies = 64; cfg.physics_threads = 1; cfg.seed = 7;
    dai_world *a = nullptr;
    if (dai_create(&cfg, &a) != DAI_OK) { std::printf("create failed\n"); return 1; }

    dai_compound_part parts[3]{};
    for (int i = 0; i < 3; ++i) {
        parts[i].shape = DAI_SHAPE_BOX;
        parts[i].half_extent = { 0.3f, 0.3f, 0.3f };
        parts[i].offset = { (float)i * 0.62f, 0, 0 };
        parts[i].rotation = { 0,0,0,1 };
    }
    dai_body_desc cd{};
    cd.shape = DAI_SHAPE_COMPOUND; cd.motion = DAI_KINEMATIC;   // kinematic: gravity must not muddy the numbers
    cd.position = { 4, 3, 0 }; cd.rotation = { 0,0,0,1 };
    cd.parts = parts; cd.part_count = 3; cd.user_data = 999;
    dai_body b = dai_body_create(a, &cd);
    std::printf("asked for position      (4, 3, 0)\n");

    dai_transform t[8];
    uint32_t n = dai_get_transforms(a, t, 8, 1.0f);
    std::printf("after create   n=%u  pos (%.4f %.4f %.4f)\n", n, t[0].position.x, t[0].position.y, t[0].position.z);

    for (int i = 0; i < 5; ++i) dai_step(a);
    n = dai_get_transforms(a, t, 8, 1.0f);
    std::printf("after 5 steps       pos (%.4f %.4f %.4f)\n", t[0].position.x, t[0].position.y, t[0].position.z);
    dai_vec3 before = t[0].position;

    const char *path = "/tmp/diag_compound.save";
    if (dai_world_save(a, path) != DAI_OK) { std::printf("save failed\n"); return 1; }

    dai_world *c = nullptr;
    if (dai_world_load(&cfg, path, &c) != DAI_OK) { std::printf("load failed\n"); return 1; }
    n = dai_get_transforms(c, t, 8, 1.0f);
    std::printf("after load     n=%u  pos (%.4f %.4f %.4f)   parts=%u\n", n,
                t[0].position.x, t[0].position.y, t[0].position.z,
                dai_body_part_count(c, t[0].body));
    std::printf("delta                   (%.4f %.4f %.4f)\n",
                t[0].position.x - before.x, t[0].position.y - before.y, t[0].position.z - before.z);

    // And the same body created a SECOND time in a fresh world, to separate
    // "load is wrong" from "create is wrong".
    dai_world *d2 = nullptr;
    dai_create(&cfg, &d2);
    dai_body_desc cd2 = cd;
    cd2.position = before;                       // ask for exactly what we read back
    cd2.parts = parts; cd2.part_count = 3;
    dai_body_create(d2, &cd2);
    n = dai_get_transforms(d2, t, 8, 1.0f);
    std::printf("re-create at readback   (%.4f %.4f %.4f)  -> off by (%.4f %.4f %.4f)\n",
                t[0].position.x, t[0].position.y, t[0].position.z,
                t[0].position.x - before.x, t[0].position.y - before.y, t[0].position.z - before.z);
    (void)b;
    return 0;
}
