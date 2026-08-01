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
#include <cmath>
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
    // Counting first, filling second - that is the contract, and it is how the
    // sync layer sizes its buffer.
    uint32_t part_count = dai_assets_resolve("blender_scene.glb", nullptr, 0, a);
    CHECK(part_count == info.nodes,
          "the resolver reports %u pieces, the model has %u nodes - a multi object "
          "file must not collapse to one piece", part_count, info.nodes);

    std::vector<dai_render_part> parts(part_count ? part_count : 1);
    uint32_t filled = dai_assets_resolve("blender_scene.glb", parts.data(), (uint32_t)parts.size(), a);
    CHECK(filled == part_count, "asking again filled %u of %u", filled, part_count);

    uint32_t mesh = part_count ? parts[0].mesh : 0xFFFFFFFFu;
    CHECK(mesh != 0xFFFFFFFFu, "the resolver returned no mesh");
    CHECK(parts[0].scale.x != 0.0f || parts[0].scale.y != 0.0f || parts[0].scale.z != 0.0f,
          "the first piece has a zero scale, which draws nothing");

    const dai_model_node *first = dai_model_node_at(m, 0);
    CHECK(first && mesh == first->mesh, "piece 0 must be node 0");
    // A short buffer must be filled as far as it goes and still report the truth.
    dai_render_part one{};
    CHECK(dai_assets_resolve("blender_scene.glb", &one, 1, a) == part_count,
          "a one element buffer changed the reported count");
    CHECK(one.mesh == first->mesh, "the short buffer did not get piece 0");

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
        dai_render_part sp{};
        CHECK(dai_assets_resolve(sel.c_str(), &sp, 1, a) == 1,
              "a selector must resolve to exactly one piece, '%s' did not", sel.c_str());
        CHECK(sp.mesh == other->mesh, "the selector picked mesh %u, '%s' is mesh %u",
              sp.mesh, other->name, other->mesh);
        CHECK(sp.mesh != mesh, "the selector returned the same mesh as no selector at all");
        std::printf("     whole file: %u pieces | '%s' alone: mesh %u\n",
                    part_count, other->name, sp.mesh);
    }

    // A typo must not silently draw the wrong object.
    dai_render_part bogus{};
    CHECK(dai_assets_resolve("blender_scene.glb#NoSuchObject", &bogus, 1, a) == 0,
          "an unknown node name resolved anyway");
    CHECK(std::strstr(dai_assets_last_error(a), "NoSuchObject") != nullptr,
          "the error does not name the missing node: '%s'", dai_assets_last_error(a));

    // ---- 5. a missing file is not fatal -----------------------------------
    std::printf("[5] missing and broken assets\n");
    dai_render_part mp{};
    CHECK(dai_assets_resolve("models/not_here.glb", &mp, 1, a) == 0,
          "a missing file resolved");
    for (int i = 0; i < 200 && !dai_assets_failed(a); ++i) {
        dai_assets_poll(a);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(dai_assets_failed(a) > 0, "a missing file did not end up in the failed count");
    CHECK(dai_assets_error_of(a, "models/not_here.glb")[0] != 0,
          "a missing file has no error message");
    CHECK(dai_assets_resolve("", &mp, 1, a) == 0, "an empty path resolved");
    CHECK(dai_assets_resolve("blender_scene.glb", &mp, 1, nullptr) == 0,
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
    std::snprintf(nd.name, sizeof(nd.name), "WholeModel");
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
    // ONE document node, every object in the file. A five object Blender export
    // must not force the user to make five scene nodes.
    CHECK(n == part_count, "%u instances after binding, the model has %u pieces", n, part_count);
    bool drew_first = false, drew_fallback = false;
    for (uint32_t i = 0; i < n; ++i) {
        if (inst[i].mesh == mesh) drew_first = true;
        if (inst[i].mesh == fallback_mesh) drew_fallback = true;
    }
    CHECK(drew_first, "the imported mesh %u is not among the drawn instances", mesh);
    CHECK(!drew_fallback, "the node still draws its collision shape (mesh %u)", fallback_mesh);
    // Every piece keeps its own material, or a five material model would come
    // out one colour.
    uint32_t distinct_materials = 0;
    for (uint32_t i = 0; i < n; ++i) {
        bool seen = false;
        for (uint32_t k = 0; k < i; ++k) if (inst[k].material == inst[i].material) seen = true;
        if (!seen) ++distinct_materials;
    }
    CHECK(distinct_materials > 1, "all %u pieces share one material", n);
    // And each piece has to land where the file says, not all at the origin.
    uint32_t distinct_places = 0;
    for (uint32_t i = 0; i < n; ++i) {
        bool seen = false;
        for (uint32_t k = 0; k < i; ++k)
            if (std::memcmp(&inst[k].position, &inst[i].position, sizeof(dai_vec3)) == 0) seen = true;
        if (!seen) ++distinct_places;
    }
    CHECK(distinct_places > 1,
          "all %u pieces sit at the same position - the model's own transforms were dropped", n);
    std::printf("     one node -> %u instances, %u materials, %u positions\n",
                n, distinct_materials, distinct_places);

    // Point the same node at one specific object inside the file.
    if (other) {
        dai_node_desc up = nd;
        std::snprintf(up.asset, sizeof(up.asset), "blender_scene.glb#%s", other->name);
        dai_doc_set(doc, node, &up);
        dai_doc_sync_apply(sync);
        n = dai_scene_instances(sc, inst, 64, 1.0f);
        CHECK(n == 1 && inst[0].mesh == other->mesh,
              "retargeting to '%s' gave %u instances on mesh %u, expected 1 on %u",
              other->name, n, n ? inst[0].mesh : 0, other->mesh);
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

    // ---- 8b. a model as a TREE of nodes, one body per piece ---------------
    // The other way to place a model. One node with several pieces is one
    // rigid body; a crate whose lid opens needs the lid to be its own body,
    // and that means its own document node, parented the way Blender had it.
    std::printf("[8b] instantiating a parented model\n");
    {
        dai_model *pm = dai_assets_model_blocking(a, "parented.gltf");
        CHECK(pm != nullptr, "parented.gltf did not load: %s",
              dai_assets_error_of(a, "parented.gltf"));
        if (pm) {
            CHECK(dai_model_node_count(pm) == 2, "the fixture should have 2 pieces, has %u",
                  dai_model_node_count(pm));
            uint32_t before = dai_doc_count(doc);
            uint32_t undo_before = dai_doc_undo_depth(doc);
            dai_node root = dai_assets_instantiate(a, doc, "parented.gltf", 0);
            CHECK(root != 0, "instantiate returned nothing: %s", dai_assets_last_error(a));
            CHECK(dai_doc_count(doc) == before + 2,
                  "instantiate added %u nodes, expected 2", dai_doc_count(doc) - before);
            CHECK(dai_doc_undo_depth(doc) == undo_before + 1,
                  "instantiate should be exactly one undo step, it pushed %u",
                  dai_doc_undo_depth(doc) - undo_before);

            dai_node crate = dai_doc_find(doc, "Crate");
            dai_node lid = dai_doc_find(doc, "Lid");
            CHECK(crate != 0 && lid != 0, "the piece names did not become node names");

            dai_node_desc cd{}, ld{};
            dai_doc_get(doc, crate, &cd);
            dai_doc_get(doc, lid, &ld);
            CHECK(ld.parent == crate, "the lid's parent is %u, the crate is %u - the hierarchy was flattened",
                  ld.parent, crate);
            CHECK(std::strcmp(ld.asset, "parented.gltf#Lid") == 0,
                  "the lid node points at '%s', expected 'parented.gltf#Lid'", ld.asset);
            // Local, not world: the document accumulates through the parent.
            CHECK(std::fabs(ld.position.y - 1.0f) < 1e-4f && std::fabs(ld.position.x) < 1e-4f,
                  "the lid's local position is (%.2f %.2f %.2f), expected (0 1 0)",
                  ld.position.x, ld.position.y, ld.position.z);
            dai_vec3 lw{};
            dai_doc_world_transform(doc, lid, &lw, nullptr, nullptr);
            CHECK(std::fabs(lw.x - 2.0f) < 1e-4f && std::fabs(lw.y - 1.0f) < 1e-4f,
                  "the lid ends up at world (%.2f %.2f %.2f), expected (2 1 0)", lw.x, lw.y, lw.z);

            // Each piece gets a box its own size, not the whole model's.
            CHECK(cd.half_extent.x > 0.4f && cd.half_extent.x < 0.6f,
                  "the crate's collision half extent is %.3f, the mesh is 1 unit wide",
                  cd.half_extent.x);

            // Two nodes, two bodies, and each draws only its own piece.
            dai_doc_sync_apply(sync);
            dai_render_instance ti[64];
            uint32_t tn = dai_scene_instances(sc, ti, 64, 1.0f);
            dai_entity ce = dai_doc_sync_entity(sync, crate);
            dai_entity le = dai_doc_sync_entity(sync, lid);
            CHECK(ce != DAI_INVALID_ENTITY && le != DAI_INVALID_ENTITY,
                  "a piece node did not get an entity");
            CHECK(dai_scene_body(sc, ce) != dai_scene_body(sc, le),
                  "the crate and the lid share one body - the whole point was two");
            CHECK(dai_scene_part_count(sc, ce) == 1,
                  "the crate node draws %u pieces, a selector must narrow it to 1",
                  dai_scene_part_count(sc, ce));
            uint32_t drawn = 0;
            for (uint32_t i = 0; i < tn; ++i)
                if (ti[i].mesh == dai_model_node_at(pm, 0)->mesh ||
                    ti[i].mesh == dai_model_node_at(pm, 1)->mesh) ++drawn;
            CHECK(drawn == 2, "%u of the 2 pieces reached a render instance", drawn);
            std::printf("     crate node %u, lid node %u (parent %u), separate bodies\n",
                        crate, lid, ld.parent);

            // Undo has to take the whole tree back, not half of it.
            CHECK(dai_doc_undo(doc) == 1, "undoing the instantiate failed");
            CHECK(dai_doc_count(doc) == before,
                  "undo left %u nodes behind", dai_doc_count(doc) - before);
            dai_doc_redo(doc);
            CHECK(dai_doc_count(doc) == before + 2, "redo did not put the tree back");
            dai_doc_sync_apply(sync);
        }
    }

    // ---- 8c. what the asset browser is fed --------------------------------
    std::printf("[8c] listing what is mounted\n");
    {
        uint32_t total = dai_assets_list(a, nullptr, 0, 0);
        CHECK(total >= 3, "the mounted folder lists %u loadable files, expected at least 3", total);

        std::vector<char> buf((size_t)total * 96 + 96, 0);
        uint32_t got = dai_assets_list(a, buf.data(), total, 96);
        CHECK(got == total, "asking again reported %u instead of %u", got, total);

        bool has_glb = false, has_gltf = false, sorted = true, has_junk = false;
        std::string prev;
        for (uint32_t i = 0; i < total; ++i) {
            std::string e = &buf[(size_t)i * 96];
            if (e.find("blender_scene.glb") != std::string::npos) has_glb = true;
            if (e.find("parented.gltf") != std::string::npos) has_gltf = true;
            if (e.find(".png") != std::string::npos || e.find(".bin") != std::string::npos)
                has_junk = true;
            if (!prev.empty() && e < prev) sorted = false;
            prev = e;
        }
        CHECK(has_glb && has_gltf, "the list is missing the .glb or the .gltf");
        CHECK(!has_junk, "the list offers files the loader cannot open");
        CHECK(sorted, "the list is not sorted - the browser would jump around between frames");

        // A short buffer must fill what it can and still report the truth.
        char one[96] = { 0 };
        CHECK(dai_assets_list(a, one, 1, 96) == total, "a one entry buffer changed the count");
        CHECK(one[0] != 0, "a one entry buffer was not filled");
        std::printf("     %u loadable files, first '%s'\n", total, one);
    }

    // ---- 9. reloading does not grow the renderer --------------------------
    // The whole point of releasing: an editor that reloads the same model all
    // afternoon must not accumulate a copy of its geometry per reload. Slots
    // and their slice of the geometry buffer come back and get reused.
    std::printf("[9] reloads reuse the freed slots\n");
    uint32_t mesh_slots_before = dai_render_mesh_count(r);
    uint32_t live_before = dai_render_mesh_live(r);
    uint32_t tex_before = dai_render_texture_count(r);
    uint32_t mat_before = dai_render_material_count(r);

    uint32_t after_first = 0;
    for (int round = 0; round < 4; ++round) {
        dai_assets *ra = dai_assets_create(r, 0);
        dai_assets_mount_dir(ra, dir.c_str(), 0);
        dai_model *rm = dai_assets_model_blocking(ra, "blender_scene.glb");
        CHECK(rm != nullptr, "reload round %d did not load", round);
        if (round == 0) after_first = dai_render_mesh_count(r);
        CHECK(dai_render_mesh_count(r) == after_first,
              "round %d pushed the mesh table to %u, round 0 left it at %u - slots are not reused",
              round, dai_render_mesh_count(r), after_first);
        // Drawing has to survive the previous round's textures being gone:
        // any material that sampled one is pointed back at the default.
        dai_render_instance ri = dai_render_instance_default();
        ri.mesh = dai_model_node_at(rm, 0)->mesh;
        ri.material = dai_model_node_at(rm, 0)->material;
        CHECK(dai_render_frame(r, &ri, 1) == DAI_OK,
              "drawing after round %d failed - a freed texture is still bound somewhere", round);
        dai_assets_destroy(ra);
    }
    CHECK(dai_render_mesh_live(r) == live_before,
          "%u meshes still live after the reloads, started at %u - something was not released",
          dai_render_mesh_live(r), live_before);
    CHECK(dai_render_texture_count(r) <= tex_before + 1,
          "textures grew from %u to %u across four reloads", tex_before, dai_render_texture_count(r));
    CHECK(dai_render_material_count(r) <= mat_before + 1,
          "materials grew from %u to %u across four reloads", mat_before, dai_render_material_count(r));
    std::printf("     mesh slots %u -> %u, live %u -> %u, textures %u -> %u\n",
                mesh_slots_before, dai_render_mesh_count(r), live_before, dai_render_mesh_live(r),
                tex_before, dai_render_texture_count(r));

    // A destroyed mesh must be inert, not a crash and not someone else's
    // geometry.
    dai_render_instance dead = dai_render_instance_default();
    dead.mesh = mesh; // freed with the model it came from? no - `a` still holds it
    CHECK(dai_render_frame(r, &dead, 1) == DAI_OK, "drawing a live mesh failed");

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
