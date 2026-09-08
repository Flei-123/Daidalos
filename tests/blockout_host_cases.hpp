// The host path, measured: dai_blockout_host_sync against a counting renderer.
//
// Included by tests/test_doc.cpp after blockout_csg_cases.hpp, same binary,
// same CHECK macro. test_doc links libdaidalos.a and NOT the Vulkan library,
// so the two renderer entry points the host calls - dai_render_mesh_create
// and dai_render_mesh_destroy - are defined HERE, as stubs that count. Every
// upload the host makes is a number in this file, and the numbers are what is
// checked:
//
//   * a Box node gets exactly ONE upload on its first frame, and a second
//     frame with the same fields uploads nothing - the cache is by content;
//   * one field change is exactly one rebuild, and the mesh it replaced is
//     given back to the renderer;
//   * undo restores the old key AND the old triangles - the stub keeps the
//     digest of what it was handed, so "same key" is proved to mean "same
//     mesh" and not just "same number";
//   * a CSG wall rebuilds when its child moves and not when it does not;
//   * a node that stops being a blockout node has its mesh destroyed;
//   * the host hands the mesh to the same functions the real host does - the
//     shape recipe under test is daiblockhost::shape_of, not a copy.
//
// The host's own g_built table is looked at directly: the file is a seam
// included into the host, and this test IS a host.
#ifndef DAI_BLOCKOUT_HOST_CASES_HPP
#define DAI_BLOCKOUT_HOST_CASES_HPP

#include "dai_ext.h"
#include "dai_blockout.h"

#include <cstdio>
#include <cstring>
#include <map>
#include <vector>

// ---- the counting renderer ---------------------------------------------
// A handle above the builtins, the digest of what was uploaded under it, and
// two counters. dai_render_mesh_load_obj and the rest are never reached by
// the host and stay undefined - an honest link error beats a stub nobody
// meant to call.
namespace daihoststub {
struct Upload { uint64_t digest; uint32_t vcount, icount; bool alive; };
static std::map<dai_mesh, Upload> g_uploads;
static uint32_t g_created = 0, g_destroyed = 0;
static dai_mesh g_next = DAI_MESH_BUILTIN_COUNT;
static int g_renderer_token = 0;  /* the address stands in for a renderer */

static uint64_t digest_of(const dai_vertex *v, uint32_t vc, const uint32_t *idx, uint32_t ic) {
    daiblock::Mesh m;
    m.verts.assign(v, v + vc);
    m.idx.assign(idx, idx + ic);
    return daiblock::digest(m);
}
static void reset() { g_uploads.clear(); g_created = g_destroyed = 0; g_next = DAI_MESH_BUILTIN_COUNT; }
static uint32_t live() {
    uint32_t n = 0;
    for (auto &u : g_uploads) if (u.second.alive) ++n;
    return n;
}
} /* namespace daihoststub */

extern "C" dai_mesh dai_render_mesh_create(dai_renderer *r, const dai_vertex *verts, uint32_t vcount,
                                           const uint32_t *indices, uint32_t icount) {
    if (r != (dai_renderer *)&daihoststub::g_renderer_token) return 0;
    if (!verts || !vcount || !indices || !icount) return 0;
    dai_mesh m = daihoststub::g_next++;
    daihoststub::g_uploads[m] = { daihoststub::digest_of(verts, vcount, indices, icount),
                                  vcount, icount, true };
    ++daihoststub::g_created;
    return m;
}

extern "C" void dai_render_mesh_destroy(dai_renderer *r, dai_mesh m) {
    if (r != (dai_renderer *)&daihoststub::g_renderer_token) return;
    auto it = daihoststub::g_uploads.find(m);
    if (it == daihoststub::g_uploads.end() || !it->second.alive) return;
    it->second.alive = false;
    ++daihoststub::g_destroyed;
}

// The host itself, included the way examples/editor_demo.cpp includes it. It
// sees the two stubs above through dai_render.h and links against them. The
// .inl is a file scope seam and carries no include guard of its own: when
// blockout_csg_cases.hpp is in the same translation unit it has already
// brought the host in, and including it twice would redefine every function
// in it.
#ifndef DAI_BLOCKOUT_CSG_CASES_HPP
#include "dai_blockout_host.inl"
#endif

static void test_blockout_host() {
    std::printf("blockout: the host uploads once per change and never otherwise\n");

    dai_config cfg{};
    cfg.tick_hz = 60; cfg.max_bodies = 64; cfg.physics_threads = 1; cfg.seed = 7;
    dai_world *w = nullptr;
    if (dai_create(&cfg, &w) != DAI_OK) { CHECK(false, "world creation failed for the host test"); return; }
    dai_scene *sc = dai_scene_create(w);
    dai_doc *d = dai_doc_create();
    dai_doc_sync *sy = dai_doc_sync_create(d, sc);
    CHECK(sy != nullptr, "sync creation failed for the host test");

    dai_ext_host ext{};
    ext.doc = d; ext.sync = sy; ext.scene = sc;
    ext.renderer = (dai_renderer *)&daihoststub::g_renderer_token;

    daihoststub::reset();
    daiblockhost::g_built.clear();

    auto frame = [&]() { dai_doc_sync_apply(sy); dai_blockout_host_sync(&ext); };
    auto built_of = [&](dai_node n) -> daiblockhost::Built {
        auto it = daiblockhost::g_built.find(n);
        return it == daiblockhost::g_built.end() ? daiblockhost::Built{} : it->second;
    };
    auto uploaded_digest = [&](dai_mesh m) -> uint64_t {
        auto it = daihoststub::g_uploads.find(m);
        return it == daihoststub::g_uploads.end() ? 0 : it->second.digest;
    };

    // --- a renderer of nullptr: the host does nothing and touches nothing --
    {
        dai_ext_host none = ext;
        none.renderer = nullptr;
        dai_blockout_host_sync(&none);
        dai_blockout_host_sync(nullptr);
        CHECK(daihoststub::g_created == 0 && daiblockhost::g_built.empty(),
              "the host built something with no renderer to build into");
    }

    // --- one box, one upload; a second frame uploads nothing ---------------
    dai_node_desc bd = dai_node_desc_default();
    std::snprintf(bd.name, sizeof(bd.name), "%s", "HostBox");
    bd.no_body = bd.no_collider = bd.no_rigidbody = 1;
    bd.blockout = DAI_BLOCKOUT_BOX;
    bd.blockout_size = { 2, 1, 1 };
    bd.render_extent = { 1, 1, 1 };
    dai_node box = dai_doc_add(d, &bd);
    frame();
    CHECK(daihoststub::g_created == 1, "first frame uploaded %u meshes for one box, expected 1", daihoststub::g_created);
    daiblockhost::Built b0 = built_of(box);
    CHECK(b0.has_mesh && b0.mesh >= DAI_MESH_BUILTIN_COUNT, "the box has no mesh in the host table");
    CHECK(b0.key != 0, "the box's key is zero");
    const uint64_t key_old = b0.key;
    const dai_mesh mesh_old = b0.mesh;
    // What went up is exactly finalise(build(shape_of(desc))) - the recipe the
    // inspector edits, taken from the host, not restated.
    const uint64_t want_old = daiblock::digest(daiblock::finalise(daiblock::build(daiblockhost::shape_of(bd))));
    CHECK(uploaded_digest(mesh_old) == want_old,
          "the uploaded box is not finalise(build(shape_of(desc))): %llx vs %llx",
          (unsigned long long)uploaded_digest(mesh_old), (unsigned long long)want_old);

    for (int i = 0; i < 5; ++i) frame();
    CHECK(daihoststub::g_created == 1, "five frames with nothing changed uploaded %u more meshes", daihoststub::g_created - 1);
    CHECK(daihoststub::g_destroyed == 0, "an unchanged box had its mesh destroyed");
    CHECK(built_of(box).key == key_old && built_of(box).mesh == mesh_old,
          "an unchanged box changed key or mesh between frames");

    // --- an edit that does not touch the shape is not a rebuild ------------
    {
        dai_node_desc q = bd;
        std::snprintf(q.name, sizeof(q.name), "%s", "HostBoxRenamed");
        q.roughness = 0.25f;
        dai_doc_begin(d, "Rename");
        dai_doc_set(d, box, &q);
        dai_doc_commit(d);
        frame();
        CHECK(daihoststub::g_created == 1, "renaming the box rebuilt its mesh");
        CHECK(built_of(box).key == key_old, "renaming the box changed its key");
        dai_doc_undo(d);
        frame();
        CHECK(daihoststub::g_created == 1, "undoing a rename rebuilt the mesh");
    }

    // --- one field change: exactly one rebuild, the old mesh given back ----
    dai_node_desc wide = bd;
    wide.blockout_size = { 3, 1, 1 };
    dai_doc_begin(d, "Widen");
    dai_doc_set(d, box, &wide);
    dai_doc_commit(d);
    frame();
    CHECK(daihoststub::g_created == 2, "one size change made %u uploads, expected exactly 1", daihoststub::g_created - 1);
    CHECK(daihoststub::g_destroyed == 1, "the replaced mesh was not destroyed (%u destroyed)", daihoststub::g_destroyed);
    CHECK(!daihoststub::g_uploads[mesh_old].alive, "the OLD mesh is the one still alive");
    daiblockhost::Built b1 = built_of(box);
    CHECK(b1.key != key_old, "a wider box has the same key as before");
    CHECK(b1.mesh != mesh_old, "a wider box was uploaded under the old handle");
    const uint64_t want_wide = daiblock::digest(daiblock::finalise(daiblock::build(daiblockhost::shape_of(wide))));
    CHECK(uploaded_digest(b1.mesh) == want_wide, "the wider box's upload is not its own recipe");
    CHECK(want_wide != want_old, "a 3 m box and a 2 m box gave the same digest");
    frame();
    CHECK(daihoststub::g_created == 2, "a frame after the size change uploaded again");

    // --- undo: the old key comes back, and the old triangles with it -------
    CHECK(dai_doc_undo(d) == 1, "undo of the size change failed");
    frame();
    CHECK(daihoststub::g_created == 3, "undo made %u uploads, expected exactly 1", daihoststub::g_created - 2);
    daiblockhost::Built b2 = built_of(box);
    CHECK(b2.key == key_old, "undo did not restore the old key: %llx vs %llx",
          (unsigned long long)b2.key, (unsigned long long)key_old);
    CHECK(uploaded_digest(b2.mesh) == want_old, "undo restored the key but not the triangles");
    CHECK(daihoststub::live() == 1, "%u meshes alive for one box after undo", daihoststub::live());
    frame();
    CHECK(daihoststub::g_created == 3, "a frame after undo uploaded again");

    // --- redo: one more rebuild, the wide key again --------------------------
    CHECK(dai_doc_redo(d) == 1, "redo of the size change failed");
    frame();
    CHECK(daihoststub::g_created == 4, "redo made %u uploads, expected exactly 1", daihoststub::g_created - 3);
    CHECK(built_of(box).key == b1.key, "redo did not bring the wide key back");
    CHECK(uploaded_digest(built_of(box).mesh) == want_wide, "redo brought a different mesh than the first widen");
    dai_doc_undo(d);
    frame();
    CHECK(built_of(box).key == key_old, "the second undo did not restore the old key");
    const uint32_t created_after_box = daihoststub::g_created;

    // --- a CSG wall: the child is consumed, moving it is one rebuild --------
    dai_node_desc wd = dai_node_desc_default();
    std::snprintf(wd.name, sizeof(wd.name), "%s", "HostWall");
    wd.no_body = wd.no_collider = wd.no_rigidbody = 1;
    wd.position = { 10, 0, 0 };
    wd.blockout = DAI_BLOCKOUT_BOX;
    wd.blockout_size = { 4, 3, 0.2f };
    wd.blockout_pivot = { 0, -1, 0 };
    wd.render_extent = { 1, 1, 1 };
    wd.csg = DAI_CSG_SUBTRACT;
    dai_node wall = dai_doc_add(d, &wd);
    dai_node_desc dd = dai_node_desc_default();
    std::snprintf(dd.name, sizeof(dd.name), "%s", "HostDoor");
    dd.no_body = dd.no_collider = dd.no_rigidbody = 1;
    dd.parent = wall;
    dd.blockout = DAI_BLOCKOUT_BOX;
    dd.blockout_size = { 1, 2, 0.5f };
    dd.blockout_pivot = { 0, -1, 0 };
    dd.render_extent = { 1, 1, 1 };
    dai_node door = dai_doc_add(d, &dd);
    frame();
    CHECK(daihoststub::g_created == created_after_box + 1,
          "a wall with a doorway made %u uploads, expected 1 (the child is a cutter, not a mesh)",
          daihoststub::g_created - created_after_box);
    CHECK(daiblockhost::g_built.count(door) == 0, "the doorway got a mesh of its own");
    daiblockhost::Built wb = built_of(wall);
    CHECK(wb.has_mesh, "the wall has no mesh");
    {
        // The upload is the boolean the host's own solid_of() computes.
        daiblock::Mesh want = daiblock::finalise(daiblockhost::solid_of(d, wall, 0));
        CHECK(uploaded_digest(wb.mesh) == daiblock::digest(want), "the wall's upload is not its own solid_of()");
        CHECK(std::fabs(daiblock::volume(want) - 2.0) < 1e-4,
              "the host's wall holds %.6f m3, the maths says 2.4 - 0.4 = 2.0", daiblock::volume(want));
    }
    frame();
    CHECK(daihoststub::g_created == created_after_box + 1, "an unchanged wall uploaded again");

    dai_node_desc moved = dd;
    moved.position = { 1.0f, 0, 0 };
    dai_doc_begin(d, "Move the doorway");
    dai_doc_set(d, door, &moved);
    dai_doc_commit(d);
    frame();
    CHECK(daihoststub::g_created == created_after_box + 2,
          "moving the doorway made %u uploads, expected exactly 1", daihoststub::g_created - created_after_box - 1);
    CHECK(built_of(wall).key != wb.key, "the doorway moved but the wall's key did not");
    CHECK(uploaded_digest(built_of(wall).mesh) != uploaded_digest(wb.mesh) || !daihoststub::g_uploads[wb.mesh].alive,
          "the doorway moved but the same wall went up");
    dai_doc_undo(d);
    frame();
    CHECK(built_of(wall).key == wb.key, "undoing the doorway move did not restore the wall's key");
    CHECK(uploaded_digest(built_of(wall).mesh) == uploaded_digest(wb.mesh),
          "undoing the doorway move did not restore the wall's triangles");

    // --- a node that stops being a blockout node gives its mesh back --------
    const uint32_t destroyed_before = daihoststub::g_destroyed;
    dai_node_desc plain = bd;
    plain.blockout = DAI_BLOCKOUT_NONE;
    dai_doc_begin(d, "Not a box any more");
    dai_doc_set(d, box, &plain);
    dai_doc_commit(d);
    frame();
    CHECK(daiblockhost::g_built.count(box) == 0, "a node that is no blockout node is still in the host table");
    CHECK(daihoststub::g_destroyed == destroyed_before + 1,
          "the mesh of a node that stopped being a box was not destroyed");
    dai_doc_undo(d);
    frame();
    CHECK(built_of(box).key == key_old, "undoing the removal did not bring the old key back");
    CHECK(uploaded_digest(built_of(box).mesh) == want_old, "undoing the removal brought different triangles");

    // --- removing the node destroys its mesh too ------------------------------
    const uint32_t destroyed_before_remove = daihoststub::g_destroyed;
    dai_doc_remove(d, wall);
    frame();
    CHECK(daiblockhost::g_built.count(wall) == 0, "a removed wall is still in the host table");
    CHECK(daihoststub::g_destroyed == destroyed_before_remove + 1, "a removed wall's mesh was not destroyed");
    CHECK(daihoststub::live() == 1, "%u meshes alive with one box left", daihoststub::live());

    // Every upload is either alive and in the table, or destroyed: nothing
    // leaked.
    CHECK(daihoststub::g_created == daihoststub::g_destroyed + daihoststub::live(),
          "%u created, %u destroyed, %u alive - the host leaked a mesh",
          daihoststub::g_created, daihoststub::g_destroyed, daihoststub::live());

    daiblockhost::g_built.clear();
    dai_doc_sync_destroy(sy);
    dai_doc_destroy(d);
    dai_scene_destroy(sc);
    dai_destroy(w);
}

#endif /* DAI_BLOCKOUT_HOST_CASES_HPP */
