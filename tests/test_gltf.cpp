// glTF import test - against a file BLENDER exported, not a hand rolled one.
//
// assets/test/blender_scene.glb was produced on a real machine by
// tools/make_testscene.py running inside Blender 4.5: five objects, five
// materials (metal, rough dielectric, emissive, textured, ground), an embedded
// PNG colour grid, and per object transforms including a non uniform scale.
//
//   DAI_SHADER_DIR=shaders ./build/test_gltf [assets/test] [outdir]

#include "dai_gltf.h"
#include "dai_render.h"
#include <cstdio>
#include <cstring>
#include <cmath>
#include <cstdlib>
#include <string>
#include <vector>

static int g_fail = 0, g_pass = 0;

#define CHECK(cond, ...) do { \
    if (cond) { ++g_pass; } \
    else { ++g_fail; std::printf("  FAIL "); std::printf(__VA_ARGS__); std::printf("\n"); } \
} while (0)

static float frame_mean(dai_renderer *r, std::vector<uint8_t> &px) {
    uint32_t w = dai_render_width(r), h = dai_render_height(r);
    px.resize((size_t)w * h * 4);
    dai_render_readback(r, px.data(), px.size());
    double s = 0;
    for (size_t i = 0; i < px.size(); i += 4) s += (0.2126*px[i] + 0.7152*px[i+1] + 0.0722*px[i+2]) / 255.0;
    return (float)(s / (px.size() / 4));
}

int main(int argc, char **argv) {
    std::string dir = argc > 1 ? argv[1] : "assets/test";
    std::string outdir = argc > 2 ? argv[2] : "/tmp";

    char err[256] = {0};
    dai_render_desc rd{}; rd.width = 960; rd.height = 540; rd.msaa = 4;
    dai_renderer *r = dai_render_create(&rd, err, sizeof(err));
    if (!r) { std::printf("renderer unavailable: %s\n", err); return 77; }
    std::printf("glTF import on: %s\n", dai_render_device_name(r));

    uint32_t meshes_before = dai_render_mesh_count(r);

    // ---- 1. the GLB Blender wrote
    dai_model *m = dai_gltf_load(r, (dir + "/blender_scene.glb").c_str(), err, sizeof(err));
    CHECK(m != nullptr, "loading blender_scene.glb failed: %s", err);
    if (!m) { dai_render_destroy(r); return 1; }

    dai_model_info info = dai_model_get_info(m);
    std::printf("  nodes %u | meshes %u | materials %u | textures %u | tris %u | verts %u\n",
                info.nodes, info.meshes, info.materials, info.textures, info.triangles, info.vertices);
    std::printf("  bounds (%.2f %.2f %.2f) .. (%.2f %.2f %.2f)\n",
                info.bounds_min.x, info.bounds_min.y, info.bounds_min.z,
                info.bounds_max.x, info.bounds_max.y, info.bounds_max.z);

    CHECK(info.nodes == 5, "expected 5 drawable nodes (cube, sphere, cone, monkey, ground), got %u", info.nodes);
    CHECK(info.materials == 5, "expected 5 materials, got %u", info.materials);
    CHECK(info.textures >= 1, "the packed colour grid texture did not arrive (%u textures)", info.textures);
    CHECK(info.triangles > 900 && info.triangles < 4000, "triangle count %u is implausible for this scene", info.triangles);
    CHECK(dai_render_mesh_count(r) > meshes_before, "no meshes were created in the renderer");

    // Blender's exporter converts Z-up to Y-up. The ground plane sits at the
    // origin and the three props are lifted 1 unit; if the axis conversion is
    // dropped, this comes out as Z instead of Y.
    CHECK(info.bounds_max.y > 1.0f && info.bounds_max.y < 4.0f,
          "scene height %.2f - Y is not up, the axis conversion is wrong", info.bounds_max.y);
    CHECK(fabsf(info.bounds_max.x) > 3.0f, "scene is only %.2f wide in X - transforms were dropped", info.bounds_max.x);

    // the ground plane keeps its non uniform scale (12 x 1 x 12)
    bool found_wide = false;
    for (uint32_t i = 0; i < dai_model_node_count(m); ++i) {
        const dai_model_node *n = dai_model_node_at(m, i);
        if (fabsf(n->scale.x) > 8.0f && fabsf(n->scale.z) > 8.0f) found_wide = true;
    }
    CHECK(found_wide, "the 12x scaled ground plane lost its scale - matrix decomposition is wrong");

    // ---- 2. render it, and make sure something is actually on screen
    dai_render_sun(r, dai_vec3{ 0.42f, 0.80f, 0.42f }, dai_vec3{ 1.0f, 0.95f, 0.86f }, 1.3f);
    dai_render_ambient(r, dai_vec3{ 0.24f, 0.40f, 0.72f }, dai_vec3{ 0.26f, 0.24f, 0.20f }, 0.38f);
    dai_render_fog(r, 0.0035f, dai_vec3{ 0.56f, 0.64f, 0.74f });
    dai_render_exposure(r, 0.58f);
    dai_render_shadow_extent(r, 14.0f);
    dai_render_sky(r, 1);

    std::vector<dai_render_instance> inst(256);
    uint32_t n = dai_model_instances(m, inst.data(), (uint32_t)inst.size(),
                                     dai_vec3{ 0,0,0 }, dai_quat{ 0,0,0,1 }, 1.0f);
    CHECK(n == info.nodes, "instance builder produced %u of %u nodes", n, info.nodes);

    dai_render_camera(r, dai_vec3{ 7.5f, 5.0f, 9.0f }, dai_vec3{ 0, 1.0f, 0.5f }, dai_vec3{ 0,1,0 }, 45.0f, 0.1f, 200.0f);
    dai_render_frame(r, inst.data(), n);
    std::vector<uint8_t> a, b;
    frame_mean(r, a);
    dai_render_write_png(r, (outdir + "/gltf_blender_scene.png").c_str());
    dai_render_frame(r, nullptr, 0);          // same camera, empty scene
    frame_mean(r, b);
    // compare per pixel: mean brightness can coincide, coverage cannot
    size_t changed = 0;
    for (size_t i = 0; i < a.size(); i += 4) {
        int d = std::abs((int)a[i] - (int)b[i]) + std::abs((int)a[i+1] - (int)b[i+1]) + std::abs((int)a[i+2] - (int)b[i+2]);
        if (d > 12) ++changed;
    }
    double coverage = 100.0 * (double)changed / (double)(a.size() / 4);
    CHECK(coverage > 20.0, "the model covers only %.1f%% of the frame - geometry is missing", coverage);
    std::printf("  model covers %.1f%% of the frame\n", coverage);
    std::printf("  rendered %u instances, %u draw calls -> %s\n",
                n, dai_render_last_draws(r), (outdir + "/gltf_blender_scene.png").c_str());

    // ---- 3. the same scene as .gltf + .bin + external PNG
    dai_model *m2 = dai_gltf_load(r, (dir + "/blender_scene.gltf").c_str(), err, sizeof(err));
    CHECK(m2 != nullptr, "loading the separate .gltf failed: %s", err);
    if (m2) {
        dai_model_info i2 = dai_model_get_info(m2);
        CHECK(i2.nodes == info.nodes && i2.triangles == info.triangles,
              "GLB and .gltf disagree: %u/%u nodes, %u/%u triangles", i2.nodes, info.nodes, i2.triangles, info.triangles);
        CHECK(i2.textures >= 1, "the external PNG texture was not loaded");
        dai_model_free(m2);
    }

    // ---- 4. a missing file must fail cleanly, not crash
    char e2[128] = {0};
    dai_model *bad = dai_gltf_load(r, (dir + "/does_not_exist.glb").c_str(), e2, sizeof(e2));
    CHECK(bad == nullptr && e2[0], "loading a missing file did not report an error");

    // ---- parenting: the crate and its lid ---------------------------------
    // Blender's answer to "a box that opens" is a child object. The importer
    // flattens transforms to world space so a piece can be drawn without
    // walking a hierarchy - but the hierarchy itself has to survive, or the
    // lid can never be animated relative to the crate it belongs to.
    {
        std::printf("parenting\n");
        char perr[256] = { 0 };
        dai_model *pm = dai_gltf_load(r, (dir + "/parented.gltf").c_str(), perr, sizeof(perr));
        CHECK(pm != nullptr, "loading parented.gltf failed: %s", perr);
        if (pm) {
            CHECK(dai_model_node_count(pm) == 2, "expected 2 pieces, got %u",
                  dai_model_node_count(pm));
            const dai_model_node *crate = dai_model_find(pm, "Crate");
            const dai_model_node *lid = dai_model_find(pm, "Lid");
            CHECK(crate && lid, "the fixture's names did not survive the import");
            if (crate && lid) {
                int crate_index = -1;
                for (uint32_t i = 0; i < dai_model_node_count(pm); ++i)
                    if (dai_model_node_at(pm, i) == crate) crate_index = (int)i;

                CHECK(crate->parent == -1, "the crate is a root, its parent is %d", crate->parent);
                CHECK(lid->parent == crate_index,
                      "the lid points at %d, the crate is piece %d - the hierarchy was lost",
                      lid->parent, crate_index);

                // world: the lid is where the crate is, one metre up
                CHECK(std::fabs(lid->position.x - 2.0f) < 1e-4f &&
                      std::fabs(lid->position.y - 1.0f) < 1e-4f,
                      "the lid's world position is (%.3f %.3f %.3f), expected (2 1 0)",
                      lid->position.x, lid->position.y, lid->position.z);
                // local: one metre up, and NOT carrying the crate's offset
                CHECK(std::fabs(lid->local_position.x) < 1e-4f &&
                      std::fabs(lid->local_position.y - 1.0f) < 1e-4f,
                      "the lid's local position is (%.3f %.3f %.3f), expected (0 1 0)",
                      lid->local_position.x, lid->local_position.y, lid->local_position.z);
                CHECK(std::fabs(crate->local_position.x - crate->position.x) < 1e-4f,
                      "a root piece's local and world position must agree");
                CHECK(crate->material != lid->material,
                      "both pieces came out with the same material");
                std::printf("  crate world (%.1f %.1f %.1f) | lid world (%.1f %.1f %.1f) local (%.1f %.1f %.1f) parent %d\n",
                            crate->position.x, crate->position.y, crate->position.z,
                            lid->position.x, lid->position.y, lid->position.z,
                            lid->local_position.x, lid->local_position.y, lid->local_position.z,
                            lid->parent);
            }
            dai_model_release(r, pm);
        }
    }

    dai_model_free(m);
    dai_render_destroy(r);
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
