// The asset layer end to end: a path in a scene file becomes a mesh on screen.
//
// Three things are being joined here and each one used to work alone:
//   Mnemosyne  finds the bytes (folder, pack file, mod priority) and caches
//   dai_gltf   turns bytes into meshes and materials inside the renderer
//   dai_doc    stores `asset models/crate.glb` and knows nothing about either
//
// So the checks below are mostly about the SEAMS: does a path resolve, does a
// missing file stay non fatal, does the same file load once rather than twice,
// does a pack file behave exactly like a folder, and does a document node
// actually end up drawing the imported mesh instead of its collision box.
//
//   DAI_SHADER_DIR=shaders ./build/test_assets [assets/test]

#include "dai_assets.h"
#include "dai_doc.h"
#include "dai_gltf.h"
#include "dai_render.h"
#include "dai_scene.h"
#include "daidalos.h"
#include "mnemosyne.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

static int g_fail = 0, g_pass = 0;

#define CHECK(cond, ...) do { \
    if (cond) { ++g_pass; } \
    else { ++g_fail; std::printf("  FAIL %s:%d  ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } \
} while (0)

// Pumps the asset registry the way a host's frame loop does: ask, poll, next
// frame. The sleep is not decoration - polling in a tight loop finishes a
// thousand iterations before a worker thread has even started, which makes an
// asynchronous load look broken when it is merely young.
static bool pump(dai_assets *a, const char *path, int max_frames = 300) {
    for (int i = 0; i < max_frames; ++i) {
        if (dai_assets_model(a, path)) return true;
        dai_assets_poll(a);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return dai_assets_model(a, path) != nullptr;
}

int main(int argc, char **argv) {
    std::string dir = argc > 1 ? argv[1] : "assets/test";

    char err[256] = { 0 };
    dai_render_desc rd{}; rd.width = 320; rd.height = 200; rd.msaa = 1;
    dai_renderer *r = dai_render_create(&rd, err, sizeof(err));
    if (!r) { std::printf("renderer unavailable: %s\n", err); return 77; }
    std::printf("assets on: %s\n", dai_render_device_name(r));

    // ---- 1. mounting ------------------------------------------------------
    std::printf("[1] mounts\n");
    dai_assets *a = dai_assets_create(r, 1);
    CHECK(a != nullptr, "dai_assets_create failed");
    if (!a) { dai_render_destroy(r); return 1; }

    CHECK(dai_assets_mount_dir(a, dir.c_str(), 0) == DAI_OK,
          "mounting %s failed: %s", dir.c_str(), dai_assets_last_error(a));
    CHECK(dai_assets_mount_dir(a, "/tmp/definitely_not_here_daidalos", 0) != DAI_OK,
          "mounting a missing directory reported success");

    // ---- 2. a model arrives ----------------------------------------------
    std::printf("[2] loading through the cache\n");
    uint32_t meshes_before = dai_render_mesh_count(r);
    CHECK(pump(a, "blender_scene.glb"), "blender_scene.glb never became ready: %s",
          dai_assets_error_of(a, "blender_scene.glb"));

    dai_model *m = dai_assets_model(a, "blender_scene.glb");
    CHECK(m != nullptr, "no model after the poll loop");
    if (!m) { dai_assets_destroy(a); dai_render_destroy(r); return 1; }

    dai_model_info info = dai_model_get_info(m);
    std::printf("     nodes %u | meshes %u | materials %u | tris %u\n",
                info.nodes, info.meshes, info.materials, info.triangles);
    CHECK(info.nodes >= 5, "the Blender scene has %u nodes, expected at least 5", info.nodes);
    CHECK(dai_render_mesh_count(r) > meshes_before,
          "importing created no meshes in the renderer");
    CHECK(dai_assets_ready(a) == 1, "%u assets resident, expected 1", dai_assets_ready(a));

    // The whole point of a cache: the second ask is free and hands back the
    // SAME model, not a second import of the same file.
    uint32_t meshes_after_first = dai_render_mesh_count(r);
    dai_model *again = dai_assets_model(a, "blender_scene.glb");
    dai_assets_poll(a);
    CHECK(again == m, "asking twice produced two different models");
    CHECK(dai_render_mesh_count(r) == meshes_after_first,
          "asking twice imported the file again (%u -> %u meshes)",
          meshes_after_first, dai_render_mesh_count(r));
    CHECK(dai_assets_tracked(a) == 1, "%u assets tracked after loading one file",
          dai_assets_tracked(a));

    // The blocking path is what a loading screen uses: no frame loop, no
    // polling, the model is simply there when the call returns.
    dai_assets *ba = dai_assets_create(r, 0);
    dai_assets_mount_dir(ba, dir.c_str(), 0);
    dai_model *bm = dai_assets_model_blocking(ba, "blender_scene.glb");
    CHECK(bm != nullptr, "the blocking load returned nothing: %s",
          dai_assets_error_of(ba, "blender_scene.glb"));
    if (bm) CHECK(dai_model_get_info(bm).nodes == info.nodes,
                  "the blocking load produced a different model");
    dai_assets_destroy(ba);

    // ---- 3. the resolver, which is what the document actually calls -------
    std::printf("[3] resolver\n");
    uint32_t mesh = 0xFFFFFFFFu, material = 0xFFFFFFFFu;
    dai_vec3 scale{ 0, 0, 0 };
    CHECK(dai_assets_resolve("blender_scene.glb", &mesh, &material, &scale, a) == 1,
          "resolving a loaded model failed");
    CHECK(mesh != 0xFFFFFFFFu, "the resolver returned no mesh");
    CHECK(scale.x != 0.0f || scale.y != 0.0f || scale.z != 0.0f,
          "the resolver returned a zero scale, which the sync layer reads as 'unset'");

    const dai_model_node *first = dai_model_node_at(m, 0);
    CHECK(first && mesh == first->mesh, "without a selector the resolver must take node 0");

    // ---- 4. sub-asset selector -------------------------------------------
    // One Blender file, five objects, and a scene node wants ONE of them.
    std::printf("[4] the #node selector\n");
    const dai_model_node *other = nullptr;
    for (uint32_t i = 1; i < dai_model_node_count(m); ++i) {
        const dai_model_node *n = dai_model_node_at(m, i);
        if (n && n->name[0] && n->mesh != first->mesh) { other = n; break; }
    }
    CHECK(other != nullptr, "the test file has no second named node with its own mesh");
    if (other) {
        std::string sel = "blender_scene.glb#" + std::string(other->name);
        uint32_t sm = 0xFFFFFFFFu, smat = 0;
        dai_vec3 ss{ 0, 0, 0 };
        CHECK(dai_assets_resolve(sel.c_str(), &sm, &smat, &ss, a) == 1,
              "resolving '%s' failed", sel.c_str());
        CHECK(sm == other->mesh, "the selector picked mesh %u, '%s' is mesh %u",
              sm, other->name, other->mesh);
        CHECK(sm != mesh, "the selector returned the same mesh as no selector at all");
        std::printf("     node 0 mesh %u, '%s' mesh %u\n", mesh, other->name, sm);
    }

    // A typo must not silently draw the wrong object.
    uint32_t bogus_mesh = 0xFFFFFFFFu;
    CHECK(dai_assets_resolve("blender_scene.glb#NoSuchObject", &bogus_mesh, &material, &scale, a) == 0,
          "an unknown node name resolved anyway");
    CHECK(std::strstr(dai_assets_last_error(a), "NoSuchObject") != nullptr,
          "the error does not name the missing node: '%s'", dai_assets_last_error(a));

    // ---- 5. a missing file is not fatal -----------------------------------
    std::printf("[5] missing and broken assets\n");
    uint32_t mm = 0xFFFFFFFFu;
    CHECK(dai_assets_resolve("models/not_here.glb", &mm, &material, &scale, a) == 0,
          "a missing file resolved");
    for (int i = 0; i < 200 && !dai_assets_failed(a); ++i) {
        dai_assets_poll(a);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(dai_assets_failed(a) > 0, "a missing file did not end up in the failed count");
    CHECK(dai_assets_error_of(a, "models/not_here.glb")[0] != 0,
          "a missing file has no error message");
    CHECK(dai_assets_resolve("", &mm, &material, &scale, a) == 0, "an empty path resolved");
    CHECK(dai_assets_resolve("blender_scene.glb", &mm, &material, &scale, nullptr) == 0,
          "resolving without a registry did not fail cleanly");

    // ---- 6. a pack file behaves exactly like a folder ---------------------
    // This is the part that needs the sidecar callback: inside a pack there is
    // no directory to look next to, so external buffers have to come back
    // through the same mount table.
    std::printf("[6] the same model out of a pack file\n");
    const char *pack = "/tmp/dai_assets_test.mnp";
    char perr[256] = { 0 };
    bool packed = mne_pack_write(pack, dir.c_str(), perr, sizeof(perr)) == MNE_OK;
    CHECK(packed, "building a pack from %s failed: %s", dir.c_str(), perr);
    if (packed) {
        dai_assets *pa = dai_assets_create(r, 0);
        CHECK(dai_assets_mount_pack(pa, pack, 0) == DAI_OK,
              "mounting the pack failed: %s", dai_assets_last_error(pa));
        CHECK(pump(pa, "blender_scene.glb"), "the packed model never loaded: %s",
              dai_assets_error_of(pa, "blender_scene.glb"));
        dai_model *pm = dai_assets_model(pa, "blender_scene.glb");
        CHECK(pm != nullptr, "no model out of the pack");
        if (pm) {
            dai_model_info pi = dai_model_get_info(pm);
            CHECK(pi.nodes == info.nodes && pi.triangles == info.triangles,
                  "the packed model differs: %u nodes / %u tris vs %u / %u",
                  pi.nodes, pi.triangles, info.nodes, info.triangles);
        }
        // .gltf + external .bin + external .png, all three inside the pack -
        // this only works if the sidecar callback is wired up
        if (pump(pa, "blender_scene.gltf")) {
            dai_model *gm = dai_assets_model(pa, "blender_scene.gltf");
            CHECK(gm != nullptr, "the .gltf variant did not load from the pack");
            if (gm) {
                dai_model_info gi = dai_model_get_info(gm);
                CHECK(gi.triangles == info.triangles,
                      "the packed .gltf has %u triangles, the .glb has %u - the external .bin was not found",
                      gi.triangles, info.triangles);
            }
        } else {
            CHECK(false, "blender_scene.gltf did not load out of the pack: %s",
                  dai_assets_error_of(pa, "blender_scene.gltf"));
        }
        dai_assets_destroy(pa);
    }

    // ---- 7. the document actually draws it --------------------------------
    // The end of the chain: a node stores a path, sync resolves it, and the
    // render instance carries the imported mesh rather than the box the
    // collision shape would have produced.
    std::printf("[7] document node -> render instance\n");
    dai_config cfg{};
    cfg.tick_hz = 60; cfg.max_bodies = 64; cfg.physics_threads = 1; cfg.seed = 7;
    dai_world *w = nullptr;
    CHECK(dai_create(&cfg, &w) == DAI_OK, "world creation failed");
    dai_scene *sc = dai_scene_create(w);
    dai_doc *doc = dai_doc_create();
    dai_doc_sync *sync = dai_doc_sync_create(doc, sc);

    dai_node_desc nd = dai_node_desc_default();
    std::snprintf(nd.name, sizeof(nd.name), "Crate");
    nd.motion = DAI_STATIC;
    nd.position = { 0, 0, 0 };
    std::snprintf(nd.asset, sizeof(nd.asset), "blender_scene.glb");
    dai_node node = dai_doc_add(doc, &nd);
    CHECK(node != 0, "adding the node failed");

    // No resolver yet: the node must still appear, drawn as its shape.
    dai_doc_sync_apply(sync);
    dai_render_instance inst[64];
    uint32_t n = dai_scene_instances(sc, inst, 64, 1.0f);
    CHECK(n == 1, "%u instances before binding the resolver, expected 1", n);
    uint32_t fallback_mesh = n ? inst[0].mesh : 0xFFFFFFFFu;

    dai_assets_bind(a, sync);
    dai_doc_sync_apply(sync);
    n = dai_scene_instances(sc, inst, 64, 1.0f);
    CHECK(n == 1, "%u instances after binding, expected 1", n);
    if (n) {
        CHECK(inst[0].mesh == mesh,
              "the node draws mesh %u, the asset resolves to %u - the resolver is not reaching the scene",
              inst[0].mesh, mesh);
        CHECK(inst[0].mesh != fallback_mesh,
              "the node still draws its collision shape (mesh %u) after the asset resolved",
              fallback_mesh);
    }

    // Point the same node at one specific object inside the file.
    if (other) {
        dai_node_desc up = nd;
        std::snprintf(up.asset, sizeof(up.asset), "blender_scene.glb#%s", other->name);
        dai_doc_set(doc, node, &up);
        dai_doc_sync_apply(sync);
        n = dai_scene_instances(sc, inst, 64, 1.0f);
        CHECK(n == 1 && inst[0].mesh == other->mesh,
              "retargeting to '%s' left the node on mesh %u, expected %u",
              other->name, n ? inst[0].mesh : 0, other->mesh);
    }

    // An asset that cannot be found leaves the node visible on its shape.
    dai_node_desc gone = nd;
    std::snprintf(gone.asset, sizeof(gone.asset), "models/vanished.glb");
    dai_doc_set(doc, node, &gone);
    dai_doc_sync_apply(sync);
    n = dai_scene_instances(sc, inst, 64, 1.0f);
    CHECK(n == 1, "a node with an unresolvable asset disappeared entirely");
    if (n) CHECK(inst[0].mesh == fallback_mesh,
                 "an unresolvable asset left mesh %u instead of the fallback %u",
                 inst[0].mesh, fallback_mesh);

    // ---- 8. saving and reopening keeps the path ---------------------------
    std::printf("[8] the path survives a save\n");
    dai_node_desc back = nd;
    dai_doc_set(doc, node, &back);
    const char *scene_path = "/tmp/dai_assets_test.scene";
    CHECK(dai_doc_save(doc, scene_path) == DAI_OK, "saving the scene failed");
    dai_doc *doc2 = dai_doc_create();
    char lerr[256] = { 0 };
    CHECK(dai_doc_load(doc2, scene_path, lerr, sizeof(lerr)) == DAI_OK,
          "reopening the scene failed: %s", lerr);
    dai_node reopened[8];
    uint32_t rn = dai_doc_nodes(doc2, reopened, 8);
    CHECK(rn == 1, "the reopened scene has %u nodes, expected 1", rn);
    dai_node_desc read{};
    CHECK(rn && dai_doc_get(doc2, reopened[0], &read) == DAI_OK, "reading the node back failed");
    CHECK(std::strcmp(read.asset, "blender_scene.glb") == 0,
          "the asset path came back as '%s'", read.asset);

    dai_doc_destroy(doc2);
    dai_doc_sync_destroy(sync);
    dai_doc_destroy(doc);
    dai_scene_destroy(sc);
    dai_destroy(w);
    dai_assets_destroy(a);
    dai_render_destroy(r);

    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
